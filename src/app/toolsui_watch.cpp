// Слежка раз в минуту (свой поток) и что с ней делает основной поток: карточки о новом автозапуске, утечках,
// нехватке памяти, итог видеокарты после игры.
#include "app/toolsui_internal.h"

#include <map>
#include <set>

#include "core/perf.h"
#include "tools/devserv.h"
#include "tools/drain.h"

namespace toolsui {

// ---------------- слежка раз в минуту ----------------
// Свой поток: снимок процессов копится в истории (утечки, фоновый процессор), трафик физических адаптеров,
// выделенная память, раз в 5 минут - новый автозапуск. Карточки показывает основной поток, по одной и не в игре.

SRWLOCK g_watchLock = SRWLOCK_INIT;
tools::ProcHistory g_hist;
std::vector<tools::ProcInfo> g_procs;  // последний снимок
std::vector<tools::ProcHistory::Leak> g_leaks;
std::vector<tools::StartItem> g_newStarts;  // ждут вопроса
std::vector<tools::SysItem> g_newSys;       // новые службы, драйверы, задачи (И17) - ждут вопроса
std::vector<std::wstring> g_wakeCache;      // кто будит: powercfg раз в полчаса, не при открытии кольца
tools::CommitInfo g_commit;
double g_netDown = 0, g_netUp = 0;  // байт/с за последнюю минуту
HANDLE g_watchStop = nullptr, g_watchThread = nullptr;

std::atomic<bool> g_game{false};
std::atomic<ULONGLONG> g_watchBeat{0};  // когда слежка прошла последний раз (Л27)
bool g_dev = false;
GpuStats g_gpu;
bool g_ddcAsk = false;     // Unreal закрыт: спросить про старый DDC, если он большой
ULONGLONG g_devEnded = 0;  // когда закрылся Unreal: через 2 минуты - смотреть хвосты
std::set<std::wstring> g_leakWarned;
ULONGLONG g_commitWarned = 0, g_proactiveAt = 0;

std::vector<tools::Suspect> g_netAlerts;    // новое в hosts, DNS, прокси - ждёт карточки
std::vector<tools::Forgotten> g_forgotten;  // забытые серверы разработки (Л19) - ждут вопроса; под g_watchLock
std::set<std::wstring> g_devKeep;           // проекты, чьи серверы велели оставить (до перезапуска программы)

BOOL CALLBACK TitleProc(HWND w, LPARAM lp) {
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t t[512];
    int n = GetWindowTextW(w, t, 512);
    if (n > 0) ((std::vector<std::wstring>*)lp)->push_back(std::wstring(t, n));
    return TRUE;
}

// Л38: проход сеанса от батареи. Начался - запомнить заряд и процессор; идёт - копить прирост процессора по
// именам; кончился (зарядку подключили) - итог основному потоку (DrainMsg), если было что сказать.
void BatteryPass(const std::vector<tools::ProcInfo>& ps, bool& on, ULONGLONG& at, int& from,
                 std::map<std::wstring, ULONGLONG>& cpu, std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>>& prev) {
    tools::BatteryInfo b = tools::Battery();
    if (!b.present) return;
    bool bat = !b.ac;
    auto feed = [&](bool add) {
        std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> now;
        for (const tools::ProcInfo& p : ps) {
            now[p.pid] = {p.created, p.cpu};
            auto it = prev.find(p.pid);
            ULONGLONG was = it != prev.end() && it->second.first == p.created ? it->second.second : p.cpu;
            if (add && p.cpu > was) cpu[p.name] += p.cpu - was;
        }
        prev.swap(now);
    };
    if (bat && !on) {
        on = true;
        at = GetTickCount64();
        from = b.percent;
        cpu.clear();
        feed(false);
    } else if (bat && on) {
        feed(true);
    } else if (!bat && on) {
        on = false;
        tools::DrainIn in;
        in.minutes = (int)((GetTickCount64() - at) / 60000);
        in.fromPct = from;
        in.toPct = b.percent;
        in.cpu = cpu;
        auto* m = new DrainMsg;
        if (tools::DrainSummary(in, m->out)) {
            if (!PostMessageW(g_app, WM_TOOLS_JOB, kDrainTag, (LPARAM)m)) delete m;
        } else {
            delete m;
        }
        cpu.clear();
        prev.clear();
    }
}

// Л19: серверы разработки (слушают порт, процесс вроде node/python) и заголовки окон - решение в tools/devserv
std::vector<tools::Forgotten> FindForgotten(const std::vector<tools::ProcInfo>& ps,
                                            std::map<std::wstring, ULONGLONG>& lastSeen, ULONGLONG idleMs,
                                            ULONGLONG minBytes) {
    bool any = false;
    for (const tools::ProcInfo& p : ps) any = any || tools::DevServerName(p.name);
    if (!any) {
        lastSeen.clear();
        return {};
    }
    std::vector<tools::Listener> ls = tools::Listeners();
    for (auto it = ls.begin(); it != ls.end();)
        it = tools::DevServerName(it->process) ? (it->dev = true, std::next(it)) : ls.erase(it);
    tools::ListenerProjects(ls);  // проект ищется только у dev - а сервер может слушать любой порт
    std::vector<tools::DevServer> servers;
    for (const tools::Listener& l : ls) {
        if (l.project.empty()) continue;
        tools::DevServer s{l.pid, l.process, l.project, l.port, 0};
        for (const tools::ProcInfo& p : ps)  // своя память и дочерних (vite, воркеры)
            if (p.pid == l.pid || p.parent == l.pid) s.bytes += p.mem;
        servers.push_back(s);
    }
    std::vector<std::wstring> titles;
    EnumWindows(TitleProc, (LPARAM)&titles);
    return tools::ForgottenServers(servers, titles, lastSeen, GetTickCount64(), idleMs, minBytes);
}
std::set<std::wstring> g_netKnown;  // что уже видели (ключ находки)

std::wstring NetKey(const tools::Suspect& s) { return s.name + L"|" + s.path; }

// Мгновенная часть слежки: поменялись прокси, DNS адаптеров, hosts или Run - сразу сравнить с тем, что было.
void QuickPass() {
    std::vector<tools::Suspect> nets = tools::ScanNetwork();
    std::vector<tools::StartItem> fresh = tools::NewStartupItems();
    AcquireSRWLockExclusive(&g_watchLock);
    for (const tools::Suspect& s : nets)
        if (s.score >= tools::kSuspectScore && g_netKnown.insert(NetKey(s)).second) g_netAlerts.push_back(s);
    g_newStarts.insert(g_newStarts.end(), fresh.begin(), fresh.end());
    bool any = !g_netAlerts.empty() || !fresh.empty();
    ReleaseSRWLockExclusive(&g_watchLock);
    if (any) PostMessageW(g_app, WM_TOOLS_JOB, kWatchTag, 0);
}

std::wstring JobTitle(net::Job j) {
    switch (j) {
        case net::JOB_REPAIR:
            return L"Починка Windows";
        case net::JOB_SYS_CLEAN:
            return L"Остатки обновлений";
        case net::JOB_VHD_COMPACT:
            return L"Сжатие дисков";
        case net::JOB_MFT_SIZES:
            return L"Чтение таблицы MFT";
        default:
            return L"Задание с правами";
    }
}

DWORD WINAPI WatchLoop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // что уже было до запуска - не новость
    for (const tools::Suspect& s : tools::ScanNetwork()) g_netKnown.insert(NetKey(s));
    // уведомления об изменениях: ключи реестра и папка с hosts. Каждое срабатывание взводится заново.
    struct Key {
        HKEY root;
        const wchar_t* path;
        HKEY h = nullptr;
        HANDLE ev = nullptr;
    } keys[] = {
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"},
        {HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces"},
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"},
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"},
    };
    auto arm = [](Key& k) {
        if (k.h)
            RegNotifyChangeKeyValue(k.h, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, k.ev,
                                    TRUE | REG_NOTIFY_THREAD_AGNOSTIC);
    };
    std::vector<HANDLE> waits{g_watchStop};
    for (Key& k : keys) {
        if (RegOpenKeyExW(k.root, k.path, 0, KEY_NOTIFY, &k.h) != ERROR_SUCCESS) k.h = nullptr;
        k.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        arm(k);
        if (k.h && k.ev) waits.push_back(k.ev);
    }
    wchar_t etc[MAX_PATH] = {};
    GetSystemDirectoryW(etc, MAX_PATH);
    wcscat_s(etc, L"\\drivers\\etc");
    HANDLE hosts =
        FindFirstChangeNotificationW(etc, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (hosts != INVALID_HANDLE_VALUE) waits.push_back(hosts);

    ULONGLONG lastIn = 0, lastOut = 0, lastAt = 0, nextFull = 0, quickAt = 0, gpuAt = 0, selfAt = 0;
    std::map<std::wstring, ULONGLONG> devSeen;  // Л19: когда проект последний раз был виден в окнах
    // Л38: сеанс от батареи - когда начался, с какого заряда, процессор по программам (pid -> время создания и
    // процессор на прошлом проходе: прирост складываем по имени)
    bool batOn = false;
    ULONGLONG batAt = 0;
    int batFrom = -1;
    std::map<std::wstring, ULONGLONG> batCpu;
    std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> batPrev;
    std::vector<tools::SelfSample> self;  // раз в час, за последние сутки с запасом
    int n = 0;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= selfAt) {  // своя память и дескрипторы: в журнал, рост за сутки - карточка (утечка у себя)
            self.push_back(tools::SampleSelf());
            if (self.size() > 30) self.erase(self.begin());
            auto* m = new SelfMsg{tools::SelfLine(self.back()), L""};
            tools::SelfLeak(self, m->leak);
            if (!PostMessageW(g_app, WM_TOOLS_JOB, kSelfTag, (LPARAM)m)) delete m;
            selfAt = now + 3600 * 1000;
        }
        if (now >= nextFull) {
            PERF_CPU("cpu.watch");
            std::vector<tools::ProcInfo> ps = tools::Processes();
            ULONGLONG in = 0, out = 0;
            tools::NetCounters(in, out);
            tools::CommitInfo c = tools::Commit();
            std::vector<tools::StartItem> fresh;
            std::vector<tools::SysItem> freshSys;
            if (n % 5 == 0) fresh = tools::NewStartupItems();
            if (n % 5 == 2) freshSys = tools::NewSysItems();  // службы и задачи - раз в 5 минут, не в тот же проход
            std::vector<tools::Forgotten> forgot;
            bool devPass = n % 5 == 4 && !g_game;
            if (devPass)
                forgot = FindForgotten(ps, devSeen, 3600 * 1000, 100ull << 20);  // забытые серверы - раз в 5 минут
            std::vector<std::wstring> wake;
            bool wakeNew = n % 30 == 0;
            if (wakeNew) wake = tools::WakeDevices();
            AcquireSRWLockExclusive(&g_watchLock);
            g_hist.Feed(ps, now);
            g_procs = std::move(ps);
            g_leaks = g_hist.Leaks();
            g_commit = c;
            if (lastAt && in >= lastIn && out >= lastOut && now > lastAt) {
                double dt = (now - lastAt) / 1000.0;
                g_netDown = (in - lastIn) / dt;
                g_netUp = (out - lastOut) / dt;
            }
            g_newStarts.insert(g_newStarts.end(), fresh.begin(), fresh.end());
            g_newSys.insert(g_newSys.end(), freshSys.begin(), freshSys.end());
            if (wakeNew) g_wakeCache = std::move(wake);
            if (devPass) g_forgotten = std::move(forgot);
            ReleaseSRWLockExclusive(&g_watchLock);
            lastIn = in, lastOut = out, lastAt = now;
            PostMessageW(g_app, WM_TOOLS_JOB, kWatchTag, 0);
            ++n;
            g_watchBeat = GetTickCount64();
            nextFull = now + (g_game ? 300000 : 60000);  // в игре реже: и так ни о чём не спросим
            tools::RecordBattery();                      // износ по дням и сколько времени на зарядке
            BatteryPass(g_procs, batOn, batAt, batFrom, batCpu, batPrev);
        }
        if (quickAt && now >= quickAt) {
            quickAt = 0;
            QuickPass();
        }
        // в игре - видеокарта раз в 10 секунд: максимум температуры и сколько упиралась в перегрев
        if (g_game && now >= gpuAt) {
            gpuAt = now + 10000;
            tools::GpuSample g = tools::GpuNow();
            if (g.ok) {
                AcquireSRWLockExclusive(&g_watchLock);
                g_gpu.max = std::max(g_gpu.max, g.temp);
                g_gpu.samples++;
                g_gpu.thermal += g.thermal;
                g_gpu.power += g.powerCap;
                ReleaseSRWLockExclusive(&g_watchLock);
            }
        }
        now = GetTickCount64();
        ULONGLONG due = quickAt ? std::min(quickAt, nextFull) : nextFull;
        if (g_game) due = std::min(due, gpuAt);
        DWORD wait = due > now ? (DWORD)std::min<ULONGLONG>(due - now, 60000) : 0;
        DWORD r = WaitForMultipleObjects((DWORD)waits.size(), waits.data(), FALSE, wait);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;  // выход
        if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + waits.size()) {
            HANDLE h = waits[r - WAIT_OBJECT_0];
            if (h == hosts)
                FindNextChangeNotification(hosts);
            else
                for (Key& k : keys)
                    if (k.ev == h) arm(k);
            // правки обычно идут пачкой (установщик пишет несколько значений): ждём, пока утихнет
            quickAt = GetTickCount64() + 1500;
        }
    }
    for (Key& k : keys) {
        if (k.h) RegCloseKey(k.h);
        if (k.ev) CloseHandle(k.ev);
    }
    if (hosts != INVALID_HANDLE_VALUE) FindCloseChangeNotification(hosts);
    CoUninitialize();
    return 0;
}

double NetDownBps() { return g_netDown; }

ULONGLONG WatchAge(ULONGLONG& periodMs) {
    periodMs = g_game ? 300000 : 60000;
    ULONGLONG b = g_watchBeat;
    return b ? std::max<ULONGLONG>(1, GetTickCount64() - b) : 0;
}

Watched Watch() {
    Watched w;
    AcquireSRWLockShared(&g_watchLock);
    w.leaks = g_leaks;
    w.hogs = g_hist.CpuHogs(10, true);
    w.hogsReady = g_hist.Ready(10);
    w.commit = g_commit;
    ReleaseSRWLockShared(&g_watchLock);
    if (!w.commit.limit) w.commit = tools::Commit();
    return w;
}

std::vector<tools::ToolVersion> Versions() {
    AcquireSRWLockShared(&g_scanLock);
    std::vector<tools::ToolVersion> v = g_scan.versions;
    ReleaseSRWLockShared(&g_scanLock);
    return v;
}

void SetVersions(const std::vector<tools::ToolVersion>& v) {
    AcquireSRWLockExclusive(&g_scanLock);
    g_scan.versions = v;
    ReleaseSRWLockExclusive(&g_scanLock);
}

// программа целиком (все процессы с этим именем) - для мягкого закрытия, как в секторе «Процессы»
tools::HeavyApp AppByName(const std::wstring& name) {
    tools::HeavyApp a;
    a.name = name;
    AcquireSRWLockShared(&g_watchLock);
    for (const tools::ProcInfo& p : g_procs)
        if (_wcsicmp(p.name.c_str(), name.c_str()) == 0) a.pids.push_back(p.pid), a.bytes += p.mem;
    ReleaseSRWLockShared(&g_watchLock);
    return a;
}

void KillLeftovers(const std::vector<tools::Leftover>& tails) {
    RunJob(L"Хвосты сборки", [tails](const tools::Log&, std::wstring& res) {
        int ok = 0;
        ULONGLONG freed = 0;
        for (const tools::Leftover& t : tails)
            if (tools::KillPid(t.pid, t.name)) ++ok, freed += t.mem;
        res = L"Завершено " + std::to_wstring(ok) + L" из " + std::to_wstring(tails.size()) + L", освободилось " +
              tools::Bytes(freed) + L" памяти.";
        return ok > 0;
    });
}

void OpenPagefile() {
    ShellExecuteW(nullptr, L"open", L"SystemPropertiesPerformance.exe", nullptr, nullptr, SW_SHOWNORMAL);
    g_card.Show(BoostCard::DONE, L"Файл подкачки",
                L"Вкладка «Дополнительно», «Виртуальная память» - «Изменить». Для 16 ГБ памяти и Unreal: размер "
                L"по выбору системы или вручную 16-24 ГБ на SSD. Нужны права, применится после перезагрузки.",
                Theme(), 30000, Toast::G_INFO);
}

void SecurityAction(const tools::Suspect& s);
int LastDay(const std::wstring& what);
void SetDay(const std::wstring& what, int day);

// Перезагрузка через 30 секунд (без прав): успеть сохранить, отменить - shutdown /a.
void RebootSoon() {
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", Sys32(L"shutdown.exe").c_str(), L"/r /t 30", Sys32(L"").c_str(),
                               SW_HIDE) <= 32) {
        g_card.Show(BoostCard::DONE, L"Не получилось", L"Перезагрузите через меню Пуск.", Theme(), 0, Toast::G_WARN);
        return;
    }
    g_card.Show(BoostCard::DONE, L"Перезагрузка через 30 секунд",
                L"Сохраните открытое. Передумали - Win+R, shutdown /a, Enter.", Theme(), 30000, Toast::G_INFO);
}

// Минута загрузки Windows (от 1601 года): по ней «уже говорили в эту загрузку».
int BootMinute() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int)(now / 600000000ull - GetTickCount64() / 60000);
}

// Новая служба, драйвер или задача планировщика (И17): «Оставить / Выключить». Задача выключается сама (если права
// позволяют), служба и драйвер - задачей с правами; прежний тип запуска - в журнал автоматики, чтобы вернуть.
void AskSysItem(const tools::SysItem& it) {
    std::wstring what = it.kind == L"svc"   ? L"Новая служба: "
                        : it.kind == L"drv" ? L"Новый драйвер: "
                                            : L"Новая задача: ";
    std::wstring sign = it.sign == tools::SIGN_OK       ? L"подпись: " + it.signer
                        : it.sign == tools::SIGN_NONE   ? L"без подписи"
                        : it.sign == tools::SIGN_NOFILE ? L"файла нет"
                                                        : L"подпись не проходит проверку";
    std::wstring sub = Short(it.path.empty() ? it.command : it.path, 90) + L". Запуск: " + it.how + L", " + sign + L".";
    bool canOff = it.kind == L"task" || tools::SysItemRefuse(it).empty();
    tools::SysItem copy = it;
    Ask(
        what + it.display, sub, L"Оставить", canOff ? L"Выключить" : L"Понятно", nullptr,
        [copy, canOff] {
            if (!canOff) return;
            if (copy.kind == L"task") {
                bool ok = tools::SetTaskEnabled(copy.name, false);
                autos::Note(L"Задача " + copy.name +
                            (ok ? L" выключена" : L": выключить не получилось (нужны права?)"));
                g_card.Show(BoostCard::DONE, ok ? L"Задача выключена" : L"Не получилось",
                            ok ? copy.display + L" больше не запускается. Включить - Планировщик заданий."
                               : L"Похоже, задача создана с правами администратора - выключите её в Планировщике.",
                            Theme());
                return;
            }
            RunJob(L"Выключаю " + copy.display, [copy](const tools::Log& log, std::wstring& res) {
                elevate::Put(L"SysItem", copy.kind + L"|" + copy.name);
                std::wstring start;
                bool ok = net::RunAdmin(net::JOB_SYS_ITEM, [&](const std::wstring& s) {
                    if (s.rfind(L"@start=", 0) == 0)
                        start = s.substr(7);
                    else
                        log(s);
                });
                if (ok)
                    autos::Note(L"Отключена " + copy.kind + L" " + copy.name + L" (прежний запуск " + start +
                                L"; вернуть: sc config " + copy.name + L" start= demand)");
                res = ok ? copy.display + L" отключена." : L"";
                return ok;
            });
        },
        40000, it.sign == tools::SIGN_OK ? Toast::G_INFO : Toast::G_WARN);
}

// По одной карточке: новый автозапуск, утечка, память на пределе, хвосты Unreal. Не в игре и не поверх другой.
void TryProactive() {
    ULONGLONG now = GetTickCount64();
    if (g_game || g_busy || g_card.Visible() || now - g_proactiveAt < 5000) return;
    AcquireSRWLockExclusive(&g_watchLock);
    bool haveStart = !g_newStarts.empty();
    tools::StartItem st = haveStart ? g_newStarts.front() : tools::StartItem{};
    if (haveStart) g_newStarts.erase(g_newStarts.begin());
    bool haveSys = !haveStart && !g_newSys.empty();
    tools::SysItem sys = haveSys ? g_newSys.front() : tools::SysItem{};
    if (haveSys) g_newSys.erase(g_newSys.begin());
    std::vector<tools::ProcHistory::Leak> leaks = g_leaks;
    tools::CommitInfo c = g_commit;
    std::vector<tools::ProcInfo> procs = g_dev || !g_devEnded ? std::vector<tools::ProcInfo>{} : g_procs;
    std::vector<tools::Forgotten> forgot = g_forgotten;
    bool haveNet = !g_netAlerts.empty();
    tools::Suspect netFind = haveNet ? g_netAlerts.front() : tools::Suspect{};
    if (haveNet) g_netAlerts.erase(g_netAlerts.begin());
    ReleaseSRWLockExclusive(&g_watchLock);
    g_proactiveAt = now;
    if (haveNet) {  // hosts, DNS или прокси только что поменялись - сказать сразу, а не через полчаса
        SecurityAction(netFind);
        return;
    }
    if (haveStart) {
        Ask(
            L"Новое в автозапуске: " + st.name,
            st.where + L": " + Short(st.command, 120) + L". Будет запускаться при каждом входе.", L"Оставить",
            st.canToggle ? L"Выключить" : L"Понятно", nullptr,
            [st] {
                if (!st.canToggle) return;
                std::wstring msg;
                bool ok = tools::SetStartup(st, false, [&](const std::wstring& t) { msg = t; });
                g_card.Show(BoostCard::DONE, ok ? L"Автозагрузка" : L"Не получилось", msg, Theme());
            },
            40000, Toast::G_INFO);
        return;
    }
    if (haveSys) {
        AskSysItem(sys);
        return;
    }
    for (const tools::Forgotten& f : forgot) {  // Л19: тихо, по одному проекту, оставленные - не спрашивать снова
        if (!g_devKeep.insert(Lower(f.project)).second) continue;
        std::wstring what;
        for (const tools::DevServer& s : f.servers)
            if (what.find(NoExe(s.process) + L" ") == std::wstring::npos || s.port)
                what +=
                    (what.empty() ? L"" : L", ") + NoExe(s.process) + (s.port ? L" :" + std::to_wstring(s.port) : L"");
        std::vector<tools::DevServer> servers = f.servers;
        std::wstring proj = Leaf(f.project);
        Ask(
            L"Забытый сервер: " + proj,
            what + L" ещё работает, а проекта не видно в окнах " + std::to_wstring(f.minutes) + L" мин. Занимает " +
                tools::Bytes(f.bytes) + L".",
            L"Завершить", L"Оставить",
            [servers, proj] {
                bg::Run([servers, proj] {
                    int killed = 0;
                    for (const tools::DevServer& s : servers) killed += tools::KillPid(s.pid, s.process);
                    autos::Note(L"Забытый сервер " + proj + L": завершено " + std::to_wstring(killed) + L" из " +
                                std::to_wstring(servers.size()));
                });
            },
            nullptr, 30000, Toast::G_INFO);
        return;
    }
    for (const tools::ProcHistory::Leak& l : leaks) {
        std::wstring key = std::to_wstring(l.pid) + L"|" + Lower(l.name);
        if (!g_leakWarned.insert(key).second) continue;
        std::wstring name = l.name;
        Ask(
            L"Растёт память: " + NoExe(l.name),
            L"Было " + tools::Bytes(l.was) + L" " + std::to_wstring(l.minutes) + L" мин назад, сейчас " +
                tools::Bytes(l.now) + L", и рост не останавливается. Похоже на утечку - помогает перезапуск.",
            L"Закрыть", L"Не сейчас", [name] { CloseHeavy(AppByName(name)); }, nullptr, 40000, Toast::G_WARN);
        return;
    }
    if (c.limit && c.total * 100 >= c.limit * 90 && (!g_commitWarned || now - g_commitWarned > 6 * 3600 * 1000ull)) {
        g_commitWarned = now;
        Ask(
            L"Память на пределе",
            L"Занято " + tools::Bytes(c.total) + L" из " + tools::Bytes(c.limit) + L" (память и подкачка " +
                tools::Bytes(tools::PagefileBytes()) +
                L"). Дальше программы начнут падать, Unreal - при сборке шейдеров.",
            L"Что закрыть", L"Подкачка", [] { MemoryOffer(L"память на пределе", {}, true); }, [] { OpenPagefile(); },
            40000, Toast::G_WARN);
        return;
    }
    if (g_ddcAsk && !g_dev) {
        g_ddcAsk = false;
        AcquireSRWLockShared(&g_scanLock);
        ULONGLONG old = g_scan.ddcOld;
        ReleaseSRWLockShared(&g_scanLock);
        if (old > 5ull * 1024 * 1024 * 1024) {
            Ask(
                L"Почистить старый кэш Unreal?",
                tools::Bytes(old) +
                    L" в DDC не открывалось больше месяца. Unreal пересоберёт нужное сам, когда понадобится.",
                L"Почистить", L"Оставить", [] { Run(L"ddc-trim"); }, nullptr, 30000, Toast::G_INFO);
            return;
        }
    }
    // Windows ждёт перезагрузки: смотреть раз в 10 минут, говорить один раз за загрузку
    if (now - g_started > 5 * 60 * 1000 && (!g_rebootAt || now - g_rebootAt > 10 * 60 * 1000)) {
        g_rebootAt = now;
        std::vector<std::wstring> why = tools::PendingReboot();
        int boot = BootMinute();
        if (!why.empty() && std::abs(LastDay(L"reboot-said") - boot) > 2) {
            SetDay(L"reboot-said", boot);
            std::wstring list;
            for (const std::wstring& w : why) list += (list.empty() ? L"" : L", ") + w;
            Ask(
                L"Windows ждёт перезагрузки",
                list +
                    L". «Завершение работы» не поможет: с быстрым запуском Windows не выключается до конца и "
                    L"установку не доделывает. Нужна именно перезагрузка.",
                L"Перезагрузить", L"Позже", [] { RebootSoon(); }, nullptr, 40000, Toast::G_INFO);
            return;
        }
    }
    if (g_devEnded && now - g_devEnded > 2 * 60 * 1000) {
        g_devEnded = 0;
        std::vector<tools::Leftover> tails;
        ULONGLONG mem = 0;
        for (const tools::Leftover& t : tools::BuildLeftovers(procs))
            if (t.why.find(L"Unreal") != std::wstring::npos) tails.push_back(t), mem += t.mem;
        g_ddcAsk = true;  // следующим заходом - старый кэш Unreal
        if (tails.empty()) return;
        Ask(
            L"Unreal закрыт, а его процессы остались",
            Plural((int)tails.size(), L"процесс", L"процесса", L"процессов") + L" (" + tails[0].name +
                (tails.size() > 1 ? L" и другие" : L"") + L") держат " + tools::Bytes(mem) + L" памяти.",
            L"Завершить", L"Оставить", [tails] { KillLeftovers(tails); }, nullptr, 30000, Toast::G_INFO);
    }
}

// Игра закончилась: как жила видеокарта (замеры раз в 10 секунд из слежки).
void GameGpuSummary() {
    AcquireSRWLockShared(&g_watchLock);
    GpuStats g = g_gpu;
    ReleaseSRWLockShared(&g_watchLock);
    if (g.samples < 6) return;  // меньше минуты - не о чем говорить
    int hot = g.thermal * 100 / g.samples, power = g.power * 100 / g.samples;
    if (g.max < 80 && hot < 5) return;  // всё спокойно - карточку не показываем
    g_card.Show(BoostCard::DONE, L"Видеокарта в игре: до " + std::to_wstring(g.max) + L" °C",
                hot >= 5
                    ? L"Упиралась в перегрев " + std::to_wstring(hot) +
                          L"% времени - частота падала. Помогут подставка, чистка вентиляторов и режим "
                          L"«Турбо» в Armoury Crate."
                    : L"Горячо, но без торможения. Ограничение по питанию: " + std::to_wstring(power) + L"% времени.",
                Theme(), 20000, Toast::G_INFO);
}

}  // namespace toolsui
