// Починка. Без прав: системный прокси, PAC/WPAD, переменные окружения, кэш DNS.
// С правами - через задачу планировщика (core/elevate): задание - строкой в HKCU, задача запускает помощника из
// Program Files и пишет журнал в его папку data; бэкапы процесса с правами - там же, не в папке пользователя.
#include "net/net_internal.h"

#include <shellapi.h>
#include <wininet.h>

#include <algorithm>
#include <chrono>
#include <mutex>

#include "core/bg.h"
#include "core/elevate.h"
#include "core/perf.h"
#include "core/text.h"
#include "tools/tools.h"

namespace net {

namespace {

constexpr wchar_t kInet[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

const wchar_t* const kJobKeys[kJobs] = {
    L"smart",       L"winhttp",       L"routes",   L"ip",     L"vpn",         L"dns-cf",        L"dns-google",
    L"dns-auto",    L"fw-restore",    L"adapter",  L"deep",   L"disk-health", L"syspath-clean", L"net-top",
    L"power",       L"restore-point", L"path-acl", L"repair", L"wu-reset",    L"unbreak",       L"defender-offline",
    L"vhd-compact", L"sys-clean",     L"mft-sizes"};

std::wstring ResultFile() { return elevate::DataDir() + L"\\job-result.txt"; }

// Бэкапы hosts и брандмауэра: их пишет и читает только процесс с правами - в папке, куда пишут администраторы.
// Импорт брандмауэра из папки пользователя дал бы любой программе пользователя открыть порты чужими руками.
std::wstring AdminBackupDir() {
    std::wstring d = elevate::DataDir() + L"\\backups";
    CreateDirectoryW(elevate::DataDir().c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::string ReadAll(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    // целиком: hosts со списками блокировок бывает в мегабайты, а CleanHosts записывает прочитанное обратно -
    // с прежним пределом 64 КБ хвост файла пропадал
    std::string s;
    char buf[65536];
    DWORD got = 0;
    while (s.size() < 64u * 1024 * 1024 && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) s.append(buf, got);
    CloseHandle(f);
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

bool AdminReady() { return elevate::TaskReady(elevate::TASK_NET); }

bool SetupAdmin() { return elevate::Setup(elevate::TASK_NET); }

namespace {

// Брошенное ожидание («Стоп»): задание с правами доработает само, но журнал и задача у него те же - следующее
// задание сначала дожидается его «#done», иначе планировщик не запустит второй экземпляр, а журнал перепишется.
std::wstring g_leftHead;    // «#run <метка>» брошенного задания
ULONGLONG g_leftUntil = 0;  // до когда его ждать (его собственный тайм-аут)

std::function<void(Job, bool, const std::wstring&)> g_onLeftDone;  // SetLeftDone

bool Stopped(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

// Ждать «#done» задания с меткой head; строки журнала - в log (если он есть). 1 - готово успешно, 0 - готово с
// ошибкой, -1 - тайм-аут, -2 - отменено.
int WaitDone(const std::wstring& head, ULONGLONG until, const Log* log, const std::atomic<bool>* cancel, bool& ok) {
    size_t shown = head.size();
    while (GetTickCount64() < until) {
        if (Stopped(cancel)) return -2;
        Sleep(200);
        std::wstring all = Widen(ReadAll(ResultFile()));
        if (all.compare(0, head.size(), head) != 0) continue;
        size_t end;
        while ((end = all.find(L'\n', shown)) != std::wstring::npos) {
            std::wstring line = Trim(all.substr(shown, end - shown));
            shown = end + 1;
            if (line.rfind(L"#done", 0) == 0) {
                ok = line.find(L'1') != std::wstring::npos;
                return ok ? 1 : 0;
            }
            if (!line.empty() && log) (*log)(line);
        }
    }
    return -1;
}

// Брошенное по «Стоп» задание: дождаться его «#done» в фоне и сказать итог (последняя строка журнала - что было)
void WatchLeft(Job j, const std::wstring& head, ULONGLONG until) {
    bg::Run(
        [j, head, until] {
            std::wstring last;
            Log keep = [&](const std::wstring& s) {
                if (s.rfind(L"@", 0) != 0) last = s;  // строки хода и служебные - не итог
            };
            int r = -1;
            bool ok = false;
            // кусками по 5 с: при выходе из программы bg::Quit не ждёт до конца DISM
            while (r == -1 && !bg::Quitting() && GetTickCount64() < until) {
                last.clear();
                r = WaitDone(head, std::min(until, GetTickCount64() + 5000), &keep, nullptr, ok);
            }
            if (r >= 0 && g_onLeftDone) g_onLeftDone(j, ok, last);
        },
        THREAD_PRIORITY_LOWEST);
}

}  // namespace

void SetLeftDone(std::function<void(Job, bool, const std::wstring&)> fn) { g_onLeftDone = std::move(fn); }

bool RunAdmin(Job j, const Log& log, int timeoutMs, const std::atomic<bool>* cancel) {
    if (j < 0 || j >= kJobs) return false;
    // одно задание за раз: зовут из разных потоков (студия, кольцо, недельная проверка, подсчёт MFT), а задание в
    // реестре и журнал у задачи одни - второе затёрло бы первое, и первый ждал бы ответа до тайм-аута
    static std::timed_mutex one;
    std::unique_lock<std::timed_mutex> hold(one, std::defer_lock);
    if (!hold.try_lock()) {
        log(L"Задача с правами занята другим действием - жду, пока закончит.");
        ULONGLONG until = GetTickCount64() + (ULONGLONG)timeoutMs;
        while (!hold.try_lock_for(std::chrono::milliseconds(200))) {
            if (Stopped(cancel)) return log(L"Остановлено."), false;
            if (GetTickCount64() >= until) {
                log(L"Не дождался - это действие пропущено.");
                return false;
            }
        }
    }
    if (!g_leftHead.empty()) {  // прошлое задание бросили по «Стоп», а оно, может, ещё работает
        bool ok = false;
        ULONGLONG now = GetTickCount64();
        if (now < g_leftUntil) {
            log(L"Прошлое остановленное задание с правами ещё доделывается - жду его.");
            ULONGLONG until = std::min(g_leftUntil, now + (ULONGLONG)timeoutMs);
            int r = WaitDone(g_leftHead, until, nullptr, cancel, ok);
            if (r == -2) return log(L"Остановлено."), false;
            if (r == -1 && until < g_leftUntil) {
                log(L"Оно ещё идёт - это действие пропущено, повтори позже.");
                return false;
            }
        }
        g_leftHead.clear();
    }
    // задачу уже не пускал антивирус: не ставить её заново после каждой пересборки, а сразу одно окно UAC
    bool blocked = elevate::Blocked(elevate::TASK_NET);
    if (!blocked && !AdminReady()) {
        log(elevate::TaskExists(elevate::TASK_NET)
                ? L"Помощник с правами устарел или стоит не там - обновляю его, Windows спросит разрешение."
                : L"Нужны права администратора: один раз создаю задачу, Windows спросит разрешение.");
        if (!SetupAdmin()) {
            log(L"Разрешение не дано - это действие пропущено.");
            return false;
        }
    }
    // метка запуска: журнал в папке помощника остаётся от прошлого раза, чужой не читаем
    wchar_t nonce[48];
    swprintf(nonce, 48, L"%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    elevate::Put(L"NetJob", std::wstring(kJobKeys[j]) + L" " + nonce);
    if (blocked || !elevate::Run(elevate::TASK_NET)) {
        if (!blocked) {
            elevate::SetBlocked(elevate::TASK_NET, true);
            log(L"Задача с правами не запускается (похоже, её не пускает антивирус) - дальше каждое такое действие "
                L"спросит разрешение.");
        }
        if (!elevate::RunOnce(elevate::TASK_NET)) {
            log(L"Разрешение не дано - это действие пропущено.");
            return false;
        }
    }
    // журнал задачи приходит файлом: читаем по мере появления строк
    std::wstring head = std::wstring(L"#run ") + nonce + L"\n";
    ULONGLONG until = GetTickCount64() + (ULONGLONG)timeoutMs;
    bool ok = false;
    switch (WaitDone(head, until, &log, cancel, ok)) {
        case -2:
            g_leftHead = head;  // под замком one: следующее задание дождётся этого
            g_leftUntil = until;
            log(L"Остановлено: само задание с правами доработает в фоне, когда закончит - скажу.");
            if (g_onLeftDone) WatchLeft(j, head, until);
            return false;
        case -1:
            log(L"Задача с правами не ответила вовремя.");
            return false;
        default:
            return ok;
    }
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
    // путь - от системы, не из переменных окружения: их задаёт пользователь, а пишет сюда процесс с правами
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring path = std::wstring(sys) + L"\\drivers\\etc\\hosts";
    std::string raw = ReadAll(path);
    // не прочитался (занят) или слишком велик - не трогать: иначе записали бы пустой или обрезанный hosts
    if (raw.empty() || raw.size() >= 64u * 1024 * 1024) {
        log(L"hosts не прочитался целиком - оставлен как есть.");
        return;
    }
    WriteAll(AdminBackupDir() + L"\\hosts-" + Stamp() + L".txt", raw);
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
                std::wstring file = AdminBackupDir() + L"\\firewall-" + Stamp() + L".wfw";
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
            HANDLE h = FindFirstFileW((AdminBackupDir() + L"\\firewall-*.wfw").c_str(), &fd);
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
            int code = Exec(L"netsh.exe", L"advfirewall import " + Q(AdminBackupDir() + L"\\" + best), &out);
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
        case JOB_DISK_HEALTH:
            for (const std::wstring& line : Split(tools::DiskHealthAdminMain(), L'\n'))
                if (!Trim(line).empty()) log(Trim(line));
            return true;
        case JOB_SYSPATH_CLEAN:
            tools::CleanSystemPath(log);
            return true;
        case JOB_NET_TOP:  // строки «@net<TAB>pid<TAB>имя<TAB>приём<TAB>отдача» (байт/с)
            for (const tools::NetUse& u : tools::NetTopAdminMain(3000))
                log(L"@net\t" + std::to_wstring(u.pid) + L"\t" + u.name + L"\t" + std::to_wstring(u.down) + L"\t" +
                    std::to_wstring(u.up));
            return true;
        case JOB_POWER:
            for (const std::wstring& line : Split(tools::PowerRequestsAdminMain(), L'\n'))
                if (!Trim(line).empty()) log(L"@power\t" + Trim(line));
            return true;
        case JOB_RESTORE_POINT: {
            // сравниваем число точек до и после: Windows молча пропускает, если точка была меньше суток назад
            // модули - только системные: PSModulePath берётся из переменных пользователя, и модуль, подложенный в
            // Документы, выполнился бы с правами администратора
            std::wstring ps =
                L"-NoProfile -ExecutionPolicy Bypass -Command \"$env:PSModulePath=$PSHOME+'\\Modules'; "
                L"$b=@(Get-ComputerRestorePoint).Count; "
                L"try { Checkpoint-Computer -Description 'MoonRadial' -RestorePointType MODIFY_SETTINGS "
                L"-ErrorAction Stop } catch { 'error: ' + $_.Exception.Message; exit 2 }; "
                L"$a=@(Get-ComputerRestorePoint).Count; if ($a -gt $b) { 'created' } else { 'skipped' }\"";
            // powershell лежит не в самой System32, а Exec берёт только оттуда: с коротким именем задача не запускалась
            int code = Exec(L"WindowsPowerShell\\v1.0\\powershell.exe", ps, &out, 300000);
            if (out.find(L"created") != std::wstring::npos) {
                log(L"Точка восстановления создана.");
                return true;
            }
            if (out.find(L"skipped") != std::wstring::npos) {
                log(L"Windows не создаёт точки чаще раза в сутки - последняя свежая, она и пригодится.");
                return true;
            }
            log(L"Не получилось (код " + std::to_wstring(code) + L"): " + Trim(out) +
                L". Возможно, на C: выключена «Защита системы».");
            return false;
        }
        case JOB_PATH_ACL:
            return tools::FixPathAclAdminMain(log) >= 0;
        case JOB_REPAIR:
            return tools::RepairWindowsAdminMain(log);
        case JOB_WU_RESET:
            return tools::ResetWindowsUpdateAdminMain(log);
        case JOB_UNBREAK:
            return tools::FixBrokenAdminMain(log) >= 0;
        case JOB_DEFENDER_OFFLINE:
            return tools::DefenderOfflineAdminMain(log);
        case JOB_VHD_COMPACT:
            return tools::CompactVhdAdminMain(log) > 0;
        case JOB_SYS_CLEAN:
            return tools::SystemCleanAdminMain(log);
        case JOB_MFT_SIZES: {
            // таблица всех папок тома уходит файлом в папку помощника, в журнал - только итог
            std::wstring lines = tools::MftAdminMain(elevate::Take(L"MftDir"));
            while (!lines.empty() && lines.back() == L'\n') lines.pop_back();
            log(lines);
            return lines.rfind(L"@mftok", 0) == 0;
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
    // задание: «ключ метка» из HKCU, одноразовое; журнал - в папку помощника, начинается с метки
    std::wstring req = Trim(elevate::Take(L"NetJob"));
    size_t sp = req.find(L' ');
    std::wstring key = req.substr(0, sp), nonce = sp == std::wstring::npos ? L"" : req.substr(sp + 1);
    CreateDirectoryW(elevate::DataDir().c_str(), nullptr);
    // файл открыт на всё время задания, каждая строка дописывается в конец одной записью: RunAdmin читает его
    // по мере появления строк и берёт только законченные (до \n)
    HANDLE f = CreateFileW(ResultFile().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    auto put = [&](const std::string& s) {
        DWORD w = 0;
        if (f != INVALID_HANDLE_VALUE) WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr);
    };
    put("#run " + Narrow(nonce) + "\n");
    Log add = [&](const std::wstring& s) { put(Narrow(s) + "\n"); };
    int job = -1;
    for (int i = 0; i < kJobs; ++i)
        if (key == kJobKeys[i]) job = i;  // только известные действия: из файла выполнять нечего другого
    bool ok = job >= 0 && Job_((Job)job, add);
    if (job < 0) add(L"Неизвестное задание.");
    put(ok ? "#done 1\n" : "#done 0\n");
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
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
