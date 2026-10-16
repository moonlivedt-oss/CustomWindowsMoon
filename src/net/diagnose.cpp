// Диагностика: пробы складываются в строки с выводом и одно лучшее действие.
// Логика NetworkDoctor, но точнее: прежде чем звать прокси «застрявшим», проверяем, жив ли он.
#include "net/net_internal.h"

#include <objbase.h>
#include <oleauto.h>
#include <netfw.h>
#include <shlobj.h>

#include <algorithm>

#include "core/perf.h"
#include "core/text.h"

namespace net {

namespace {

// GUID брандмауэра - свои копии: в libuuid MinGW их может не быть
const CLSID kFwPolicy = {0xe2b3c97f, 0x6ae1, 0x41ac, {0x81, 0x7a, 0xf6, 0xf9, 0x21, 0x66, 0xd7, 0xdd}};
const IID kIFwPolicy2 = {0x98325047, 0xc671, 0x4174, {0x8d, 0x81, 0xde, 0xfc, 0xd3, 0xf0, 0x31, 0x86}};
const IID kIFwRule = {0xaf230d27, 0xbaba, 0x4e42, {0xac, 0xed, 0xf5, 0x24, 0xf2, 0x2c, 0xfc, 0xe2}};

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

// Две работы параллельно: сетевые пробы ждут ответа, а не процессор.
void Parallel(const std::function<void()>& a, const std::function<void()>& b) {
    struct Box {
        const std::function<void()>* f;
    } box{&b};
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            (*((Box*)p)->f)();
            return 0;
        },
        &box, 0, nullptr);
    a();
    if (th) {
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    } else {
        b();
    }
}

std::wstring Ms(int ms) { return std::to_wstring(ms) + L" мс"; }

}  // namespace

std::wstring DataDir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\net", p, MAX_PATH);
    SHCreateDirectoryExW(nullptr, p, nullptr);
    return p;
}

std::wstring BackupDir() {
    std::wstring d = DataDir() + L"\\backups";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

int Exec(const std::wstring& exe, const std::wstring& args, std::wstring* out, int timeoutMs) {
    // только из System32: имя без пути Windows ищет и в текущей папке - подложенный netsh.exe выполнился бы с правами
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring path = std::wstring(sys) + L"\\" + exe;
    std::wstring cmd = L"\"" + path + L"\" " + args;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    std::string raw;
    char buf[4096];
    DWORD got = 0;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
               ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) && got)
            raw.append(buf, got);
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        if ((int)(GetTickCount64() - t0) > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) raw.append(buf, got);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    if (out) {
        int n = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), nullptr, 0);
        out->assign(n, 0);
        MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), out->data(), n);
    }
    return (int)code;
}

bool PhysicalAdapter(Adapter& out) {
    // подключённый, со шлюзом и не туннель; если таких несколько - с наименьшей метрикой маршрута
    std::vector<Adapter> list = ReadAdapters();
    ULONG bestMetric = ~0u;
    bool found = false;
    MIB_IPFORWARD_TABLE2* t = nullptr;
    if (GetIpForwardTable2(AF_INET, &t) == NO_ERROR && t) {
        for (ULONG i = 0; i < t->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2& r = t->Table[i];
            if (r.DestinationPrefix.PrefixLength != 0) continue;  // только маршрут по умолчанию
            for (const Adapter& a : list)
                if (a.index == r.InterfaceIndex && a.up && !a.vpn && r.Metric < bestMetric) {
                    bestMetric = r.Metric;
                    out = a;
                    found = true;
                }
        }
        FreeMibTable(t);
    }
    if (!found)
        for (const Adapter& a : list)
            if (a.up && !a.vpn && a.gateway) {
                out = a;
                return true;
            }
    return found;
}

std::wstring Gateway(std::wstring* adapter) {
    Adapter a;
    if (!PhysicalAdapter(a)) return L"";
    std::wstring ip;
    MIB_IPFORWARD_TABLE2* t = nullptr;
    if (GetIpForwardTable2(AF_INET, &t) == NO_ERROR && t) {
        for (ULONG i = 0; i < t->NumEntries && ip.empty(); ++i) {
            const MIB_IPFORWARD_ROW2& r = t->Table[i];
            if (r.DestinationPrefix.PrefixLength != 0 || r.InterfaceIndex != a.index) continue;
            const BYTE* b = (const BYTE*)&r.NextHop.Ipv4.sin_addr;
            if (!b[0]) continue;
            ip = std::to_wstring(b[0]) + L"." + std::to_wstring(b[1]) + L"." + std::to_wstring(b[2]) + L"." +
                 std::to_wstring(b[3]);
        }
        FreeMibTable(t);
    }
    if (adapter) *adapter = a.name;
    return ip;
}

std::vector<FwRule> FirewallBlocks(bool* readOk) {
    PERF_SCOPE("net.FirewallBlocks");
    ComScope com;
    std::vector<FwRule> out;
    if (readOk) *readOk = false;
    INetFwPolicy2* pol = nullptr;
    if (FAILED(CoCreateInstance(kFwPolicy, nullptr, CLSCTX_INPROC_SERVER, kIFwPolicy2, (void**)&pol))) return out;
    INetFwRules* rules = nullptr;
    IUnknown* unk = nullptr;
    IEnumVARIANT* en = nullptr;
    if (SUCCEEDED(pol->get_Rules(&rules)) && SUCCEEDED(rules->get__NewEnum(&unk)) &&
        SUCCEEDED(unk->QueryInterface(IID_IEnumVARIANT, (void**)&en))) {
        if (readOk) *readOk = true;
        VARIANT v;
        VariantInit(&v);
        ULONG fetched = 0;
        while (en->Next(1, &v, &fetched) == S_OK) {
            INetFwRule* rule = nullptr;
            if (v.vt == VT_DISPATCH && v.pdispVal && SUCCEEDED(v.pdispVal->QueryInterface(kIFwRule, (void**)&rule))) {
                NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
                NET_FW_ACTION act = NET_FW_ACTION_ALLOW;
                VARIANT_BOOL on = VARIANT_FALSE;
                rule->get_Direction(&dir);
                rule->get_Action(&act);
                rule->get_Enabled(&on);
                if (dir == NET_FW_RULE_DIR_OUT && act == NET_FW_ACTION_BLOCK && on) {
                    BSTR name = nullptr, app = nullptr;
                    rule->get_Name(&name);
                    rule->get_ApplicationName(&app);
                    FwRule r;
                    r.name = name ? name : L"";
                    std::wstring low = Lower(r.name + L" " + (app ? app : L""));
                    r.vpn = VpnName(low) || low.find(L"kill") != std::wstring::npos ||
                            low.find(L"block all") != std::wstring::npos ||
                            low.find(L"lockdown") != std::wstring::npos || low.find(L"vpn") != std::wstring::npos;
                    out.push_back(r);
                    SysFreeString(name);
                    SysFreeString(app);
                }
                rule->Release();
            }
            VariantClear(&v);
        }
    }
    if (en) en->Release();
    if (unk) unk->Release();
    if (rules) rules->Release();
    pol->Release();
    return out;
}

int RemoveFirewallRules(const std::vector<std::wstring>& names) {
    ComScope com;
    int removed = 0;
    INetFwPolicy2* pol = nullptr;
    if (FAILED(CoCreateInstance(kFwPolicy, nullptr, CLSCTX_INPROC_SERVER, kIFwPolicy2, (void**)&pol))) return 0;
    INetFwRules* rules = nullptr;
    if (SUCCEEDED(pol->get_Rules(&rules))) {
        for (const std::wstring& n : names) {
            BSTR b = SysAllocString(n.c_str());
            if (SUCCEEDED(rules->Remove(b))) ++removed;
            SysFreeString(b);
        }
        rules->Release();
    }
    pol->Release();
    return removed;
}

Report Diagnose(bool remnants) {
    PERF_SCOPE("net.Diagnose");
    InitSockets();
    Report r;
    Exit exit;
    // HTTPS и выходной узел - в своём потоке, остальное - здесь
    Parallel(
        [&] {
            r.proxy = ReadProxy();
            r.adapters = ReadAdapters();
            r.viaTunnel = RouteTo(L"1.1.1.1", &r.route);
            r.tcp = TcpConnectMs(L"1.1.1.1", 443, 3000) >= 0 || TcpConnectMs(L"8.8.8.8", 53, 3000) >= 0;
            addrinfoW hints{}, *res = nullptr;
            hints.ai_family = AF_UNSPEC;
            r.dns = GetAddrInfoW(L"example.com", nullptr, &hints, &res) == 0;
            if (res) FreeAddrInfoW(res);
            r.pingMs = Ping(L"1.1.1.1", 3, 1000, &r.lossPct);
        },
        [&] {
            exit = ExitNode(5000);
            r.https = exit.ok || HttpsOk(6000);
        });
    r.ip = exit.ip;
    r.country = exit.country;
    r.colo = exit.colo;
    bool viaHttp = false;
    if (r.viaTunnel && (r.pingMs < 0 || Faked(r.pingMs)) && r.https) {
        r.pingMs = HttpRtt(4000);  // TUN отвечает на пинг сам - меряем по-настоящему
        r.lossPct = 0;
        viaHttp = true;
    }
    auto add = [&](Level l, const std::wstring& t, const std::wstring& d) { r.checks.push_back({l, t, d}); };

    // кто держит прокси: имя процесса по порту
    std::wstring owner;
    if (r.proxy.on && r.proxy.local) {
        size_t c = r.proxy.server.find_last_of(L':');
        int port = c == std::wstring::npos ? 0 : _wtoi(r.proxy.server.c_str() + c + 1);
        for (const ProxyProc& p : ProxyProcesses())
            if (std::find(p.ports.begin(), p.ports.end(), port) != p.ports.end()) owner = p.name;
    }

    // --- прокси ---
    const Proxy& px = r.proxy;
    if (px.on && !px.alive)
        add(BAD, L"Системный прокси",
            L"Включён " + px.server +
                L", но по этому адресу никто не отвечает. Прокси-клиент закрылся, а настройка "
                L"осталась: через неё сайты не открываются.");
    else if (px.on)
        add(INFO, L"Системный прокси",
            L"Включён " + px.server + (owner.empty() ? L"" : L" (" + owner + L")") +
                L" и отвечает: трафик браузеров идёт через прокси-клиент.");
    else if (!px.machine.empty())
        add(WARN, L"Системный прокси",
            L"Прокси уровня всего компьютера: " + px.machine + L". Обычно его ставит политика или другая программа.");
    else if (!px.pac.empty())
        add(WARN, L"Системный прокси", L"Автонастройка прокси (PAC): " + px.pac);
    else
        add(OK, L"Системный прокси", L"Выключен, прямое подключение.");
    add(px.winhttp.empty() ? OK : WARN, L"WinHTTP-прокси",
        px.winhttp.empty() ? L"Прямой доступ." : L"Задан " + px.winhttp + L" - через него ходят службы Windows.");
    if (!px.env.empty()) {
        std::wstring s;
        for (const std::wstring& e : px.env) s += (s.empty() ? L"" : L", ") + e;
        add(WARN, L"Переменные окружения", L"Задан прокси: " + s + L". Программы в терминале пойдут через него.");
    }

    // --- туннели ---
    std::wstring vpnUp;
    for (const Adapter& a : r.adapters)
        if (a.vpn && a.up) vpnUp += (vpnUp.empty() ? L"" : L", ") + a.name;
    if (r.viaTunnel)
        add(INFO, L"Туннель", L"Интернет идёт через " + r.route + L". Пинг и выход зависят от сервера туннеля.");
    else if (!vpnUp.empty())
        add(INFO, L"Туннель", L"Подключены " + vpnUp + L", но интернет идёт мимо них, через " + r.route + L".");

    // --- связь ---
    add(r.tcp ? OK : BAD, L"Связь напрямую (TCP)",
        r.tcp ? L"Сеть отвечает." : L"Нет связи даже напрямую - проблема глубже прокси.");
    add(r.dns ? OK : BAD, L"DNS", r.dns ? L"Имена сайтов находятся." : L"Имена сайтов не находятся.");
    add(r.https ? OK : (px.on ? BAD : WARN), L"HTTPS, как в браузере",
        r.https ? L"Сайты открываются." : L"Сайты не открываются.");
    if (viaHttp && r.pingMs >= 0)
        add(r.pingMs > 150 ? WARN : OK, L"Задержка через туннель",
            Ms(r.pingMs) + L" до выхода туннеля и обратно. Обычный пинг здесь врёт: туннель отвечает на него сам "
                           L"за 0 мс, поэтому меряю повтором HTTPS-запроса.");
    else if (r.pingMs >= 0)
        add(r.pingMs > 150 || r.lossPct > 10 ? WARN : OK, L"Пинг до 1.1.1.1",
            Ms(r.pingMs) + L", потерь " + std::to_wstring(r.lossPct) + L"%." +
                (r.viaTunnel && r.pingMs > 100 ? L" Это время до выхода туннеля и обратно." : L""));
    if (!r.ip.empty())
        add(INFO, L"Выходной узел",
            CountryName(r.country) + (r.colo.empty() ? L"" : L" (" + r.colo + L")") + L", адрес " + r.ip + L".");
    Adapter phys;
    if (PhysicalAdapter(phys) && phys.mtu > 0 && phys.mtu < 1400)
        add(WARN, L"MTU", L"«" + phys.name + L"»: " + std::to_wstring(phys.mtu) +
                              L" - занижен (туннель мог оставить). Часть сайтов может «висеть».");

    // --- остатки VPN ---
    std::vector<FwRule> fw;
    std::vector<StaleRoute> stale;
    std::vector<std::wstring> hosts;
    if (remnants) {
        fw = FirewallBlocks();
        stale = StaleRoutes();
        hosts = SuspiciousHosts();
        std::wstring vpnRules;
        int nv = 0;
        for (const FwRule& f : fw)
            if (f.vpn && nv++ < 5) vpnRules += L"\n" + f.name;
        if (nv)
            add(WARN, L"Брандмауэр: kill-switch",
                L"Блок-правила, похожие на VPN: после падения туннеля они держат интернет закрытым." + vpnRules);
        else
            add(OK, L"Брандмауэр",
                fw.empty() ? L"Исходящих блок-правил нет."
                           : L"Исходящих блок-правил " + std::to_wstring(fw.size()) + L", на VPN не похожи.");
        if (!stale.empty()) {
            std::wstring s;
            for (size_t i = 0; i < stale.size() && i < 5; ++i)
                s += L"\n" + stale[i].prefix + L" через «" + stale[i].adapter + L"»";
            add(WARN, L"Зависшие маршруты", L"Маршруты на отключённых адаптерах (остатки туннелей):" + s);
        } else {
            add(OK, L"Маршруты", L"Зависших нет.");
        }
        if (!hosts.empty()) {
            std::wstring s;
            for (size_t i = 0; i < hosts.size() && i < 5; ++i) s += L"\n" + hosts[i];
            add(WARN, L"Файл hosts", L"Настоящие сайты уведены в никуда:" + s);
        }
    }

    // --- вывод и одно лучшее действие ---
    auto fix = [&](Fix f, const std::wstring& label, const std::wstring& why) {
        if (r.fix != FIX_NONE) return;
        r.fix = f;
        r.fixLabel = label;
        r.fixReason = why;
    };
    if (!r.tcp) {
        if (r.viaTunnel)
            fix(FIX_EXIT, L"Переподключить туннель",
                L"Интернет идёт в туннель " + r.route + L", а он молчит. Переподключи его или выключи.");
        else
            fix(FIX_IP, L"Обновить IP", L"Нет связи даже напрямую. Начни с обновления IP, потом перезапуск адаптера.");
    } else if (!r.https && px.on && !px.alive) {
        fix(FIX_PROXY, L"Сбросить прокси",
            L"Застрял мёртвый прокси " + px.server + L" - сброс вернёт прямое подключение.");
    } else if (!r.https && px.on) {
        fix(FIX_EXIT, L"Сменить сервер",
            L"Прокси" + (owner.empty() ? L"" : L" " + owner) +
                L" жив, но через него сайты не открываются - не отвечает его сервер. Смени сервер в клиенте, "
                L"сбрасывать "
                L"прокси не нужно.");
    } else if (!r.https && r.viaTunnel) {
        fix(FIX_EXIT, L"Сменить сервер",
            L"Туннель " + r.route + L" не пропускает сайты - не отвечает его выход. Смени сервер в клиенте.");
    }
    if (!fw.empty() && std::any_of(fw.begin(), fw.end(), [](const FwRule& f) { return f.vpn; }))
        fix(FIX_VPN, L"Убрать остатки VPN",
            L"Kill-switch VPN остался в брандмауэре и может держать интернет закрытым.");
    if (!stale.empty() || !hosts.empty() || (phys.mtu > 0 && phys.mtu < 1400))
        fix(FIX_VPN, L"Убрать остатки VPN", L"Остались следы туннелей: маршруты, MTU или записи hosts.");
    if (!r.dns || !r.https) fix(FIX_SMART, L"Починить умно", L"Сброс прокси и DNS, при необходимости обновление IP.");
    if (!px.winhttp.empty() || !px.env.empty() || (!px.pac.empty() && !px.on))
        fix(FIX_PROXY, L"Сбросить прокси", L"Остались настройки прокси, которые сейчас не нужны.");
    return r;
}

std::wstring Summary(const Report& r) {
    std::wstring s = r.Online() ? L"Интернет есть" : r.tcp ? L"Сайты не открываются" : L"Нет интернета";
    if (!r.country.empty()) s += L"   |   " + CountryName(r.country);
    if (r.pingMs >= 0) s += L"   |   " + Ms(r.pingMs);
    if (r.viaTunnel) s += L"   |   через " + r.route;
    return s;
}

// ---- история ----

namespace {
SRWLOCK g_histLock = SRWLOCK_INIT;
}

void Remember(const std::wstring& kind, const std::wstring& text) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[32];
    swprintf(ts, 32, L"%02d.%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
    std::string line = Narrow(std::wstring(ts) + L"\t" + kind + L"\t" + text) + "\n";
    AcquireSRWLockExclusive(&g_histLock);
    std::wstring path = DataDir() + L"\\history.txt";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, line.data(), (DWORD)line.size(), &w, nullptr);
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        CloseHandle(f);
        if (sz.QuadPart > 256 * 1024) {  // разросся - оставить вторую половину
            HANDLE rf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            std::string all((size_t)sz.QuadPart, '\0');
            DWORD got = 0;
            if (rf != INVALID_HANDLE_VALUE) {
                ReadFile(rf, all.data(), (DWORD)all.size(), &got, nullptr);
                CloseHandle(rf);
            }
            all.resize(got);
            size_t cut = all.find('\n', all.size() / 2);
            if (cut != std::string::npos) {
                HANDLE wf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (wf != INVALID_HANDLE_VALUE) {
                    WriteFile(wf, all.data() + cut + 1, (DWORD)(all.size() - cut - 1), &w, nullptr);
                    CloseHandle(wf);
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_histLock);
}

std::vector<std::pair<std::wstring, std::wstring>> History(int max) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    AcquireSRWLockShared(&g_histLock);
    std::wstring path = DataDir() + L"\\history.txt";
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string all;
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        all.resize((size_t)std::min<LONGLONG>(sz.QuadPart, 512 * 1024));
        DWORD got = 0;
        ReadFile(f, all.data(), (DWORD)all.size(), &got, nullptr);
        all.resize(got);
        CloseHandle(f);
    }
    ReleaseSRWLockShared(&g_histLock);
    std::vector<std::wstring> lines = Split(Widen(all), L'\n');
    for (auto it = lines.rbegin(); it != lines.rend() && (int)out.size() < max; ++it) {
        std::vector<std::wstring> p = Split(*it, L'\t');
        if (p.size() >= 3) out.push_back({p[0], p[2]});
    }
    return out;
}

}  // namespace net
