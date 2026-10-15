// Починка. Без прав: системный прокси, PAC/WPAD, переменные окружения, кэш DNS.
// С правами - через задачу планировщика: задание кладётся файлом, задача выполняет его и пишет журнал.
#include "net/net_internal.h"

#include <shellapi.h>
#include <wininet.h>

#include <algorithm>

#include "core/perf.h"
#include "core/text.h"

namespace net {

namespace {

constexpr wchar_t kInet[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
constexpr wchar_t kTask[] = L"MoonDesk\\RadialMenuNet";

const wchar_t* const kJobKeys[kJobs] = {L"smart",      L"winhttp",  L"routes",     L"ip",      L"vpn", L"dns-cf",
                                        L"dns-google", L"dns-auto", L"fw-restore", L"adapter", L"deep"};

std::wstring JobFile() { return DataDir() + L"\\job.txt"; }
std::wstring ResultFile() { return DataDir() + L"\\job-result.txt"; }

std::string ReadAll(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(64 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return s;
}

void WriteAll(const std::wstring& path, const std::string& s) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr);
    CloseHandle(f);
}

bool Okish(int code, const std::wstring& out) {
    std::wstring o = Lower(out);
    return code == 0 || o.find(L"ok.") != std::wstring::npos || o.find(L"успеш") != std::wstring::npos;
}

std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

}  // namespace

const wchar_t* JobKey(Job j) { return j >= 0 && j < kJobs ? kJobKeys[j] : L""; }

// ---- без прав ----

void ResetProxy(const Log& log) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kInet, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        DWORD zero = 0;
        RegSetValueExW(k, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
        RegDeleteValueW(k, L"ProxyServer");
        RegDeleteValueW(k, L"AutoConfigURL");
        RegCloseKey(k);
        log(L"Системный прокси выключен и очищен.");
    }
    // PAC и авто-определение (WPAD) живут битами в DefaultConnectionSettings: 0x04 - PAC, 0x08 - WPAD
    std::wstring conn = std::wstring(kInet) + L"\\Connections";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, conn.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        BYTE blob[4096];
        DWORD size = sizeof(blob), type = 0;
        if (RegQueryValueExW(k, L"DefaultConnectionSettings", nullptr, &type, blob, &size) == ERROR_SUCCESS &&
            type == REG_BINARY && size >= 12) {
            blob[8] &= (BYTE)~0x0C;
            DWORD counter = 0;
            memcpy(&counter, blob + 4, 4);
            ++counter;
            memcpy(blob + 4, &counter, 4);
            RegSetValueExW(k, L"DefaultConnectionSettings", 0, REG_BINARY, blob, size);
            log(L"Автонастройка прокси (PAC и WPAD) выключена.");
        }
        RegCloseKey(k);
    }
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    // переменные окружения прокси - и в реестре, и у самой программы
    int removed = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        for (const wchar_t* n : {L"HTTP_PROXY", L"HTTPS_PROXY", L"ALL_PROXY", L"FTP_PROXY", L"http_proxy",
                                 L"https_proxy", L"all_proxy", L"ftp_proxy"})
            if (RegDeleteValueW(k, n) == ERROR_SUCCESS) ++removed;
        RegCloseKey(k);
    }
    for (const wchar_t* n : {L"HTTP_PROXY", L"HTTPS_PROXY", L"ALL_PROXY", L"FTP_PROXY"})
        SetEnvironmentVariableW(n, nullptr);
    if (removed) {
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 3000,
                            nullptr);
        log(L"Удалены переменные прокси: " + std::to_wstring(removed) + L".");
    }
    FlushDns(log);
}

void FlushDns(const Log& log) {
    std::wstring out;
    int code = Exec(L"ipconfig.exe", L"/flushdns", &out, 15000);
    log(code == 0 ? L"Кэш DNS очищен." : L"Кэш DNS очистить не удалось.");
}

// ---- задача планировщика ----

bool AdminReady() {
    std::wstring key =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\" + std::wstring(kTask);
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

bool SetupAdmin() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // запускается только по требованию: дата старта - далёкое будущее
    std::wstring args = L"/Create /TN \"" + std::wstring(kTask) + L"\" /TR \"\\\"" + exe +
                        L"\\\" --net-job\" /SC ONCE /ST 00:00 /SD 01/01/2030 /RL HIGHEST /F";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"schtasks.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 20000);
        CloseHandle(sei.hProcess);
    }
    return AdminReady();
}

bool RunAdmin(Job j, const Log& log, int timeoutMs) {
    if (j < 0 || j >= kJobs) return false;
    if (!AdminReady()) {
        log(L"Нужны права администратора: один раз создаю задачу, Windows спросит разрешение.");
        if (!SetupAdmin()) {
            log(L"Разрешение не дано - это действие пропущено.");
            return false;
        }
    }
    DeleteFileW(ResultFile().c_str());
    WriteAll(JobFile(), Narrow(kJobKeys[j]));
    std::wstring out;
    if (Exec(L"schtasks.exe", L"/Run /TN \"" + std::wstring(kTask) + L"\"", &out, 15000) != 0) {
        log(L"Задача с правами не запустилась.");
        return false;
    }
    // журнал задачи приходит файлом: читаем по мере появления строк
    size_t shown = 0;
    ULONGLONG t0 = GetTickCount64();
    while ((int)(GetTickCount64() - t0) < timeoutMs) {
        Sleep(200);
        std::wstring all = Widen(ReadAll(ResultFile()));
        size_t end;
        while ((end = all.find(L'\n', shown)) != std::wstring::npos) {
            std::wstring line = Trim(all.substr(shown, end - shown));
            shown = end + 1;
            if (line.rfind(L"#done", 0) == 0) return line.find(L'1') != std::wstring::npos;
            if (!line.empty()) log(line);
        }
    }
    log(L"Задача с правами не ответила вовремя.");
    return false;
}

// ---- выполняется в процессе с правами (--net-job) ----

namespace {

void DeleteStaleRoutes(const Log& log) {
    std::vector<StaleRoute> st = StaleRoutes();
    if (st.empty()) {
        log(L"Зависших маршрутов нет.");
        return;
    }
    int n = 0;
    for (StaleRoute& r : st)
        if (DeleteIpForwardEntry2(&r.row) == NO_ERROR) {
            ++n;
            log(L"Удалён маршрут " + r.prefix + L" через «" + r.adapter + L"».");
        }
    if (!n) log(L"Маршруты удалить не удалось.");
}

void CleanHosts(const Log& log) {
    std::vector<std::wstring> bad = SuspiciousHosts();
    if (bad.empty()) return;
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\drivers\\etc\\hosts", path, MAX_PATH);
    std::string raw = ReadAll(path);
    WriteAll(BackupDir() + L"\\hosts-" + Stamp() + L".txt", raw);
    std::string kept;
    int removed = 0;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find('\n', pos);
        std::string line = raw.substr(pos, end == std::string::npos ? std::string::npos : end - pos + 1);
        pos = end == std::string::npos ? raw.size() : end + 1;
        std::wstring w = Trim(Widen(line));
        size_t hash = w.find(L'#');
        if (hash != std::wstring::npos) w = Trim(w.substr(0, hash));
        bool drop = !w.empty() && std::find(bad.begin(), bad.end(), w) != bad.end();
        if (drop)
            ++removed;
        else
            kept += line;
    }
    WriteAll(path, kept);
    log(L"Из hosts убрано записей: " + std::to_wstring(removed) + L" (копия в бэкапах).");
}

void SetDns(const Log& log, const wchar_t* a, const wchar_t* b) {
    Adapter ad;
    if (!PhysicalAdapter(ad)) {
        log(L"Не нашёл подключённый адаптер.");
        return;
    }
    std::wstring out;
    int code;
    if (!a) {
        code = Exec(L"netsh.exe", L"interface ipv4 set dnsservers name=" + Q(ad.name) + L" source=dhcp", &out);
    } else {
        code = Exec(L"netsh.exe",
                    L"interface ipv4 set dnsservers name=" + Q(ad.name) + L" source=static address=" + a +
                        L" register=primary validate=no",
                    &out);
        Exec(L"netsh.exe",
             L"interface ipv4 add dnsservers name=" + Q(ad.name) + L" address=" + b + L" index=2 validate=no");
    }
    Exec(L"ipconfig.exe", L"/flushdns");
    log(Okish(code, out)
            ? L"DNS адаптера «" + ad.name + L"»: " + (a ? std::wstring(a) + L", " + b : L"от роутера") + L"."
            : L"DNS не сменился: " + Trim(out));
}

bool Job_(Job j, const Log& log) {
    std::wstring out;
    switch (j) {
        case JOB_SMART:
        case JOB_WINHTTP: {
            int code = Exec(L"netsh.exe", L"winhttp reset proxy", &out);
            log(Okish(code, out) ? L"WinHTTP-прокси сброшен." : L"WinHTTP-прокси сбросить не удалось.");
            if (j == JOB_SMART) DeleteStaleRoutes(log);
            Exec(L"ipconfig.exe", L"/flushdns");
            return true;
        }
        case JOB_ROUTES:
            DeleteStaleRoutes(log);
            return true;
        case JOB_IP: {
            Adapter ad;
            std::wstring name = PhysicalAdapter(ad) ? L" " + Q(ad.name) : L"";
            log(L"Обновляю IP" + (ad.name.empty() ? L"" : L" адаптера «" + ad.name + L"»") + L"...");
            Exec(L"ipconfig.exe", L"/release" + name, &out, 40000);
            int code = Exec(L"ipconfig.exe", L"/renew" + name, &out, 60000);
            log(code == 0 ? L"IP обновлён." : L"IP обновить не удалось.");
            return code == 0;
        }
        case JOB_VPN: {
            // брандмауэр: сначала бэкап, потом только правила, похожие на kill-switch VPN (свои правила целы)
            std::vector<std::wstring> names;
            for (const FwRule& f : FirewallBlocks())
                if (f.vpn) names.push_back(f.name);
            if (!names.empty()) {
                std::wstring file = BackupDir() + L"\\firewall-" + Stamp() + L".wfw";
                int code = Exec(L"netsh.exe", L"advfirewall export " + Q(file), &out);
                log(Okish(code, out) ? L"Бэкап брандмауэра сохранён." : L"Бэкап брандмауэра не получился - продолжаю.");
                int n = RemoveFirewallRules(names);
                log(L"Убрано правил kill-switch: " + std::to_wstring(n) + L".");
            } else {
                log(L"Правил kill-switch нет.");
            }
            DeleteStaleRoutes(log);
            Adapter ad;
            if (PhysicalAdapter(ad) && ad.mtu > 0 && ad.mtu < 1500) {
                int code = Exec(L"netsh.exe",
                                L"interface ipv4 set subinterface " + Q(ad.name) + L" mtu=1500 store=persistent", &out);
                log(Okish(code, out) ? L"MTU «" + ad.name + L"» возвращён к 1500." : L"MTU сменить не удалось.");
            }
            Exec(L"netsh.exe", L"interface ip delete arpcache");
            Exec(L"netsh.exe", L"interface ip delete destinationcache");
            CleanHosts(log);
            Exec(L"ipconfig.exe", L"/flushdns");
            log(L"Кэши ARP и DNS очищены.");
            return true;
        }
        case JOB_DNS_CF:
            SetDns(log, L"1.1.1.1", L"1.0.0.1");
            return true;
        case JOB_DNS_GOOGLE:
            SetDns(log, L"8.8.8.8", L"8.8.4.4");
            return true;
        case JOB_DNS_AUTO:
            SetDns(log, nullptr, nullptr);
            return true;
        case JOB_FW_RESTORE: {
            std::wstring best;
            FILETIME bestT{};
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((BackupDir() + L"\\firewall-*.wfw").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do
                    if (CompareFileTime(&fd.ftLastWriteTime, &bestT) > 0) {
                        bestT = fd.ftLastWriteTime;
                        best = fd.cFileName;
                    }
                while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            if (best.empty()) {
                log(L"Бэкапов брандмауэра нет.");
                return false;
            }
            int code = Exec(L"netsh.exe", L"advfirewall import " + Q(BackupDir() + L"\\" + best), &out);
            log(Okish(code, out) ? L"Брандмауэр возвращён из " + best + L"." : L"Вернуть брандмауэр не удалось.");
            return Okish(code, out);
        }
        case JOB_ADAPTER: {
            Adapter ad;
            if (!PhysicalAdapter(ad)) {
                log(L"Не нашёл подключённый адаптер.");
                return false;
            }
            Exec(L"netsh.exe", L"interface set interface name=" + Q(ad.name) + L" admin=disabled", &out);
            Sleep(1500);
            int code = Exec(L"netsh.exe", L"interface set interface name=" + Q(ad.name) + L" admin=enabled", &out);
            log(Okish(code, out) ? L"Адаптер «" + ad.name + L"» перезапущен." : L"Адаптер перезапустить не удалось.");
            return Okish(code, out);
        }
        case JOB_DEEP: {
            int a = Exec(L"netsh.exe", L"winsock reset", &out);
            int b = Exec(L"netsh.exe", L"int ip reset", &out);
            log(L"Winsock и TCP/IP сброшены" + std::wstring(a == 0 || b == 0 ? L"" : L" частично") +
                L". Подействует после перезагрузки.");
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

int AdminMain() {
    InitSockets();
    std::wstring key = Trim(Widen(ReadAll(JobFile())));
    DeleteFileW(JobFile().c_str());  // задание одноразовое
    std::string log;
    auto flush = [&] { WriteAll(ResultFile(), log); };
    Log add = [&](const std::wstring& s) {
        log += Narrow(s) + "\n";
        flush();
    };
    int job = -1;
    for (int i = 0; i < kJobs; ++i)
        if (key == kJobKeys[i]) job = i;  // только известные действия: из файла выполнять нечего другого
    bool ok = job >= 0 && Job_((Job)job, add);
    if (job < 0) add(L"Неизвестное задание.");
    log += ok ? "#done 1\n" : "#done 0\n";
    flush();
    return ok ? 0 : 1;
}

// ---- «Починить умно» ----

bool SmartFix(const Log& log) {
    PERF_SCOPE("net.SmartFix");
    if (HttpsOk(6000)) {
        log(L"Интернет и так работает.");
        return true;
    }
    log(L"Шаг 1: сброс прокси и DNS.");
    ResetProxy(log);
    Sleep(800);
    if (HttpsOk(6000)) {
        log(L"Интернет вернулся после сброса прокси.");
        return true;
    }
    log(L"Шаг 2: WinHTTP-прокси и зависшие маршруты.");
    RunAdmin(JOB_SMART, log);
    if (HttpsOk(6000)) {
        log(L"Интернет вернулся.");
        return true;
    }
    log(L"Шаг 3: обновление IP.");
    RunAdmin(JOB_IP, log);
    Sleep(2000);
    if (HttpsOk(8000)) {
        log(L"Интернет вернулся после обновления IP.");
        return true;
    }
    log(L"Само не починилось. Дальше: перезапуск адаптера или глубокий сброс в студии.");
    return false;
}

}  // namespace net
