#include "net/net_internal.h"
#include "net/vpnguard.h"
#include "core/elevate.h"
#include "core/text.h"
#include "system/tools.h"
#include <set>

namespace net {
namespace {
std::wstring Path() { return elevate::DataDir() + L"\\vpn-guard-authorized.txt"; }
std::wstring Read() {
    HANDLE f = CreateFileW(Path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    char data[32768];
    DWORD got = 0;
    bool ok = ReadFile(f, data, sizeof(data), &got, nullptr) && got < sizeof(data);
    CloseHandle(f);
    return ok ? Widen(std::string(data, got)) : L"";
}
std::vector<std::wstring> OwnRules() {
    std::vector<std::wstring> out;
    for (const auto& r : FirewallBlocks())
        if (r.name.rfind(kVpnGuardPrefix, 0) == 0) out.push_back(r.name);
    return out;
}
bool Remove(const std::vector<std::wstring>& rules, const Log& log) {
    if (rules.empty()) return true;
    bool ok = RemoveFirewallRules(rules) == (int)rules.size();
    if (!ok) log(L"Не все правила защиты VPN удалось снять. Повторите снятие блокировок.");
    return ok;
}
}  // namespace
bool VpnGuardConfigured() { return !Read().empty(); }
bool VpnGuardMatches(const std::wstring& route, const std::wstring& apps) {
    std::wstring expected = Trim(route) + L"\n";
    for (auto app : Split(apps, L'|'))
        if (!Trim(app).empty()) expected += Trim(app) + L"\n";
    return !route.empty() && Read() == expected;
}

bool VpnGuardJob(int mode, const Log& log) {
    if (mode == 2) {
        bool ok = Remove(OwnRules(), log);
        if (ok) DeleteFileW(Path().c_str());
        log(ok ? L"Собственные блокировки VPN сняты; другие правила не менялись." : L"Снятие блокировок не завершено.");
        return ok;
    }
    if (mode == 1) {
        auto route = Trim(elevate::Take(L"VpnGuardRoute"));
        auto apps = Split(elevate::Take(L"VpnGuardApps"), L'|');
        if (route.empty() || route.size() > 256 || route.find_first_of(L"\r\n\t") != route.npos || apps.empty() ||
            apps.size() > 16)
            return log(L"Выберите туннель и от 1 до 16 программ."), false;
        std::wstring config = route + L"\n";
        for (auto app : apps) {
            app = Trim(app);
            auto why = tools::CutRefuse(app);
            DWORD attrs = GetFileAttributesW(app.c_str());
            if (!why.empty() || attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) ||
                app.find_first_of(L"\r\n\t") != app.npos)
                return log(L"Недопустимая программа: " + (why.empty() ? L"файл не найден - " + app : why)), false;
            config += app + L"\n";
        }
        if (!Remove(OwnRules(), log)) return false;
        auto data = Narrow(config);
        HANDLE f = CreateFileW((Path() + L".tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD put = 0;
        bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
        CloseHandle(f);
        if (!ok || !MoveFileExW((Path() + L".tmp").c_str(), Path().c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return false;
        log(L"Список программ и выбранный туннель разрешены. Блокировка управляется отдельно в студии.");
        return true;
    }
    auto config = Split(Read(), L'\n');
    if (config.size() < 2 || config[0].empty())
        return log(L"Защита VPN не настроена: сначала разрешите список через UAC."), false;
    std::wstring route;
    bool tunnel = RouteTo(L"1.1.1.1", &route);
    bool block = route.empty() || !tunnel || Lower(route) != Lower(config[0]);
    auto own = OwnRules();
    std::set<std::wstring> existing(own.begin(), own.end());
    bool ok = true;
    for (size_t i = 1; i < config.size(); ++i) {
        auto app = Trim(config[i]);
        if (app.empty()) continue;
        if (!tools::CutRefuse(app).empty()) return false;
        auto name = std::wstring(kVpnGuardPrefix) + app;
        if (!block) {
            if (existing.count(name)) ok = Remove({name}, log) && ok;
        } else if (!existing.count(name)) {
            std::wstring output;
            int rc = Exec(L"netsh.exe",
                          L"advfirewall firewall add rule name=\"" + name + L"\" dir=out action=block program=\"" +
                              app + L"\" enable=yes profile=any",
                          &output, 15000);
            ok = rc == 0 && ok;
        }
    }
    log(ok ? (block ? L"Выбранные программы заблокированы: ожидаемый туннель не подтверждён."
                    : L"Маршрут через выбранный туннель подтверждён; блокировки сняты.")
           : L"Не удалось применить все правила защиты VPN.");
    return ok;
}
}  // namespace net
