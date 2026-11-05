#include "net/net_internal.h"
#include "net/background.h"
#include <bcrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <sstream>
#include "core/text.h"

namespace net {
namespace {
std::wstring Hash(const std::wstring& value) {
    auto text = Narrow(value);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD bytes = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&bytes, sizeof(bytes), &got, 0);
    std::vector<UCHAR> object(bytes), digest(32);
    bool ok = BCryptCreateHash(alg, &hash, object.data(), bytes, nullptr, 0, 0) >= 0;
    if (ok)
        ok = BCryptHashData(hash, (PUCHAR)text.data(), (ULONG)text.size(), 0) >= 0 &&
             BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::wstring result;
    if (ok)
        for (auto c : digest) {
            result += L"0123456789abcdef"[c >> 4];
            result += L"0123456789abcdef"[c & 15];
        }
    return result;
}
std::wstring RegText(const wchar_t* name, bool& ok) {
    wchar_t value[8192]{};
    DWORD bytes = sizeof(value);
    auto rc = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", name,
                           RRF_RT_REG_SZ | RRF_RT_REG_DWORD, nullptr, value, &bytes);
    if (rc == ERROR_FILE_NOT_FOUND) return {};
    if (rc != ERROR_SUCCESS) {
        ok = false;
        return {};
    }
    if (std::wstring(name) == L"ProxyEnable") return std::to_wstring(*(DWORD*)value);
    return value;
}
std::wstring Clean(std::wstring s) {
    for (auto& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    if (s.size() > 512) s.resize(512);
    return s;
}
std::wstring ProxyDisplay(std::wstring s) {
    // Не сохраняем user:password, query и путь PAC. Для сравнения используется полный хэш.
    size_t scheme = s.find(L"://");
    if (scheme != s.npos) s.erase(0, scheme + 3);
    size_t at = s.rfind(L'@');
    if (at != s.npos) s.erase(0, at + 1);
    size_t end = s.find_first_of(L"/?#");
    if (end != s.npos) s.resize(end);
    return Clean(s);
}
}  // namespace

std::vector<SettingStamp> ReadSettingStamps() {
    InitSockets();
    std::vector<SettingStamp> out;
    bool ok = true;
    auto enabled = RegText(L"ProxyEnable", ok), server = RegText(L"ProxyServer", ok),
         pac = RegText(L"AutoConfigURL", ok);
    WINHTTP_PROXY_INFO win{};
    bool http = WinHttpGetDefaultProxyConfiguration(&win) != FALSE;
    std::wstring machine = win.lpszProxy ? win.lpszProxy : L"";
    if (win.lpszProxy) GlobalFree(win.lpszProxy);
    if (win.lpszProxyBypass) GlobalFree(win.lpszProxyBypass);
    if (ok && http) {
        auto raw = enabled + L"\n" + server + L"\n" + pac + L"\n" + machine;
        std::wstring summary = enabled == L"1" ? L"прокси: " + ProxyDisplay(server) : L"прокси выключен";
        summary += pac.empty() ? L"; PAC нет" : L"; PAC: " + ProxyDisplay(pac);
        if (!machine.empty()) summary += L"; WinHTTP: " + ProxyDisplay(machine);
        out.push_back({L"Прокси", Hash(raw), summary});
    }
    ULONG bytes = 32768;
    std::vector<BYTE> buf(bytes);
    auto rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
                                   (IP_ADAPTER_ADDRESSES*)buf.data(), &bytes);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bytes);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
                                  (IP_ADAPTER_ADDRESSES*)buf.data(), &bytes);
    }
    if (rc == NO_ERROR) {
        bool dnsComplete = true;
        std::vector<std::wstring> lines;
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
                wchar_t host[128]{};
                // по имени адаптера, а не номеру: переподключённый VPN получает новый IfIndex, и те же DNS
                // выглядели бы сменой
                if (GetNameInfoW(d->Address.lpSockaddr, d->Address.iSockaddrLength, host, 128, nullptr, 0,
                                 NI_NUMERICHOST) == 0)
                    lines.push_back(std::wstring(a->FriendlyName ? a->FriendlyName : L"?") + L": " + host);
                else
                    dnsComplete = false;
            }
        }
        std::sort(lines.begin(), lines.end());
        std::wstring raw;
        for (const auto& s : lines) raw += s + L"; ";
        if (dnsComplete) out.push_back({L"DNS", Hash(raw), raw.empty() ? L"активных DNS нет" : Clean(raw)});
    }
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\drivers\\etc\\hosts", path, MAX_PATH);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart <= 8 * 1024 * 1024) {
            std::string text((size_t)size.QuadPart, '\0');
            DWORD read = 0;
            if (ReadFile(file, text.data(), (DWORD)text.size(), &read, nullptr) && read == text.size()) {
                std::vector<std::wstring> lines;
                for (auto line : Split(Widen(text), L'\n')) {
                    line = Trim(line.substr(0, line.find(L'#')));
                    if (!line.empty()) lines.push_back(line);
                }
                std::sort(lines.begin(), lines.end());
                std::wstring raw;
                for (auto& line : lines) raw += line + L"\n";
                auto digest = Hash(raw);
                out.push_back(
                    {L"hosts", digest, std::to_wstring(lines.size()) + L" записей; отпечаток " + digest.substr(0, 12)});
            }
        }
        CloseHandle(file);
    }
    return out;
}

std::vector<SettingStamp> LoadSettingBaseline() {
    std::vector<SettingStamp> out;
    HANDLE f = CreateFileW((BackupDir() + L"\\settings-baseline.tsv").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char data[16384];
    DWORD got = 0;
    if (ReadFile(f, data, sizeof(data), &got, nullptr))
        for (auto line : Split(Widen(std::string(data, got)), L'\n')) {
            auto p = Split(line, L'\t');
            // DNS прежнего вида («12: 8.8.8.8» - с номером адаптера) не берём: запишется заново без ложного «сменился»
            if (p.size() == 3 && p[0] == L"DNS" && !p[2].empty() && iswdigit(p[2][0])) continue;
            if (p.size() == 3 && p[1].size() == 64) out.push_back({p[0], p[1], Trim(p[2])});
        }
    CloseHandle(f);
    return out;
}
bool SaveSettingBaseline(const std::vector<SettingStamp>& stamps) {
    std::wstring text;
    for (const auto& s : stamps) text += Clean(s.name) + L"\t" + s.digest + L"\t" + Clean(s.summary) + L"\n";
    auto data = Narrow(text);  // данные только отпечатки и очищенные описания
    auto path = BackupDir() + L"\\settings-baseline.tsv";
    HANDLE f = CreateFileW((path + L".tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    CloseHandle(f);
    return ok &&
           MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}  // namespace net
