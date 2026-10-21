// Сторожа процессов (свой поток, раз в 5 с, низкий приоритет): тихий баланс процессора (И12), «не спать, пока
// работает» (И13), карточка, когда долгая команда закончилась (И14). Решения - tools/guard (под тестами), здесь
// снимки, приоритеты, запрет сна и очередь для основного потока (карточки и журнал показывает Tick).
#include "app/toolsui_internal.h"

#include "tools/guard.h"

namespace toolsui {

namespace {

struct GuardCfg {
    bool balance = true, noSleep = true, longWatch = true;
    std::wstring noSleepApps, longApps, keepFast, procRules;
    int longMin = 3;
};

SRWLOCK g_guardLock = SRWLOCK_INIT;
GuardCfg g_gcfg;                         // под g_guardLock: снимок настроек (их меняет основной поток)
std::vector<std::wstring> g_guardNotes;  // под g_guardLock: строки журнала для основного потока
struct LongDone {
    std::wstring name;
    ULONGLONG ms;
    DWORD code;
    bool haveCode;
};
std::vector<LongDone> g_longDone;             // под g_guardLock: долгие команды закончились
std::wstring g_awake;                         // под g_guardLock: что держит без сна
std::vector<guard::Balancer::Act> g_lowered;  // под g_guardLock: кто сейчас понижен
std::atomic<bool> g_restoreAll{false};
HANDLE g_guardStop = nullptr, g_guardThread = nullptr;
ULONGLONG g_gcfgAt = 0;

bool UserActive() {
    LASTINPUTINFO li{sizeof(li)};
    return GetLastInputInfo(&li) && GetTickCount() - li.dwTime < 120000;
}

DWORD FgPid() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid;
}

bool SetPrio(DWORD pid, DWORD from, DWORD to) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    bool ok = GetPriorityClass(h) == from && SetPriorityClass(h, to);
    CloseHandle(h);
    return ok;
}

// Правило процесса (И25): приоритет, ядра, EcoQoS - один раз на каждый новый процесс.
bool ApplyRule(DWORD pid, const guard::ProcRule& r) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    bool ok = true;
    if (r.prio) ok = SetPriorityClass(h, r.prio) && ok;
    if (r.cores) {
        DWORD_PTR proc = 0, sys = 0;
        if (GetProcessAffinityMask(h, &proc, &sys)) {
            DWORD_PTR want = (DWORD_PTR)r.cores & sys;  // ядер больше, чем есть, - только существующие
            ok = (want && SetProcessAffinityMask(h, want)) && ok;
        }
    }
    if (r.eco >= 0) {
        PROCESS_POWER_THROTTLING_STATE st{};
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = r.eco ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
        ok = SetProcessInformation(h, ProcessPowerThrottling, &st, sizeof(st)) && ok;
    }
    CloseHandle(h);
    return ok;
}

DWORD WINAPI GuardLoop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    guard::Balancer bal;
    guard::LongWatch lw;
    std::set<DWORD> lowered;          // кому приоритет понизили мы (возвращаем только им)
    std::map<DWORD, ULONGLONG> ruled;  // кому правило уже применено (pid -> время создания: номер мог смениться)
    std::wstring rulesText;
    std::vector<guard::ProcRule> rules;
    std::map<DWORD, HANDLE> handles;  // долгие команды: держим, чтобы узнать код выхода
    DWORD self = GetCurrentProcessId();
    int cores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    bool awakeOn = false;
    do {
        AcquireSRWLockShared(&g_guardLock);
        GuardCfg c = g_gcfg;
        ReleaseSRWLockShared(&g_guardLock);
        std::vector<guard::Proc> procs;
        for (const tools::ProcInfo& p : tools::Processes()) {
            guard::Proc q;
            q.pid = p.pid, q.parent = p.parent, q.name = p.name, q.created = p.created, q.cpu = p.cpu;
            procs.push_back(std::move(q));
        }
        ULONGLONG now = GetTickCount64();
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULONGLONG nowFt = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

        // ---- тихий баланс ----
        std::vector<guard::Balancer::Act> lo, re;
        bool on = c.balance && !g_game && !g_restoreAll;
        std::set<DWORD> protect;
        DWORD fg = FgPid();
        protect.insert(self);
        protect.insert(fg);
        for (const guard::Proc& p : procs) {
            if (p.parent == fg && fg) protect.insert(p.pid);  // то, что запустило окно впереди (вкладки, сборка)
            if (guard::Match(p.name,
                             L"explorer.exe|dwm.exe|audiodg.exe|csrss.exe|ctfmon.exe|RadialMenu*.exe|"
                             L"RadialTools.exe|MsMpEng.exe|avp*.exe|obs64.exe|obs32.exe|" +
                                 c.keepFast))
                protect.insert(p.pid);
        }
        bal.Feed(now, procs, cores, on && UserActive(), protect, lo, re);
        for (const auto& a : lo)
            if (SetPrio(a.pid, NORMAL_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS)) {
                lowered.insert(a.pid);
                AcquireSRWLockExclusive(&g_guardLock);
                g_guardNotes.push_back(L"Баланс: " + a.name + L" ел " + std::to_wstring((int)(a.share * 100)) +
                                       L"% процессора полторы минуты, пока вы работали в другом окне, - приоритет "
                                       L"ниже обычного");
                ReleaseSRWLockExclusive(&g_guardLock);
            }
        auto giveBack = [&](DWORD pid, const std::wstring& name) {
            if (!lowered.erase(pid)) return;
            SetPrio(pid, BELOW_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS);
            AcquireSRWLockExclusive(&g_guardLock);
            g_guardNotes.push_back(L"Баланс: " + name + L" - приоритет снова обычный");
            ReleaseSRWLockExclusive(&g_guardLock);
        };
        for (const auto& a : re) giveBack(a.pid, a.name);
        if (!on || g_restoreAll) {  // выключили, игра или «вернуть всё»: всем, кого понижали, - обычный
            for (const auto& a : bal.Lowered()) giveBack(a.pid, a.name);
            for (DWORD pid : std::vector<DWORD>(lowered.begin(), lowered.end()))
                giveBack(pid, L"процесс " + std::to_wstring(pid));
            bal = guard::Balancer();
            g_restoreAll = false;
        }
        for (auto it = lowered.begin(); it != lowered.end();)  // закрылся - помнить нечего
            it = std::any_of(procs.begin(), procs.end(), [&](const guard::Proc& p) { return p.pid == *it; })
                     ? std::next(it)
                     : lowered.erase(it);

        // ---- постоянные правила (И25) ----
        if (c.procRules != rulesText) {  // правила поменяли - применить заново ко всем
            rulesText = c.procRules;
            rules = guard::ParseRules(rulesText);
            ruled.clear();
        }
        if (!rules.empty()) {
            std::map<DWORD, ULONGLONG> alive;
            for (const guard::Proc& p : procs) {
                auto was = ruled.find(p.pid);
                if (was != ruled.end() && was->second == p.created) {
                    alive[p.pid] = p.created;
                    continue;
                }
                if (p.pid == self || guard::Match(p.name, L"explorer.exe|dwm.exe|csrss.exe|RadialMenu*.exe")) continue;
                for (const guard::ProcRule& r : rules)
                    if (guard::Match(p.name, r.mask)) {
                        bool ok = ApplyRule(p.pid, r);
                        alive[p.pid] = p.created;
                        AcquireSRWLockExclusive(&g_guardLock);
                        g_guardNotes.push_back(L"Правило: " + guard::DescribeRule(r) +
                                               (ok ? L"" : L" (не всё получилось - процесс с правами?)") + L", pid " +
                                               std::to_wstring(p.pid));
                        ReleaseSRWLockExclusive(&g_guardLock);
                        break;
                    }
            }
            ruled = std::move(alive);
        }

        // ---- не спать ----
        std::wstring awake = c.noSleep ? guard::KeepAwake(procs, c.noSleepApps) : L"";
        if (!awake.empty() != awakeOn) {
            awakeOn = !awake.empty();
            SetThreadExecutionState(awakeOn ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
        }

        // ---- долгие команды ----
        std::vector<guard::LongWatch::Done> done;
        if (c.longWatch) {
            for (const guard::Proc& p : procs)
                if (guard::Match(p.name, c.longApps) && !handles.count(p.pid))
                    if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid))
                        handles[p.pid] = h;
            done = lw.Feed(nowFt, procs, c.longApps, (ULONGLONG)c.longMin * 60000);
        }
        for (const auto& d : done) {
            bool watching = d.pid == fg || std::find(d.ancestors.begin(), d.ancestors.end(), fg) != d.ancestors.end();
            DWORD code = 0;
            bool have = handles.count(d.pid) && GetExitCodeProcess(handles[d.pid], &code) && code != STILL_ACTIVE;
            if (watching || g_game) continue;
            if (g_dev && guard::Match(d.name, L"UnrealBuildTool.exe|ShaderCompileWorker.exe"))
                continue;  // режим разработки скажет сам
            AcquireSRWLockExclusive(&g_guardLock);
            g_longDone.push_back({d.name, d.ms, code, have});
            ReleaseSRWLockExclusive(&g_guardLock);
        }
        for (auto it = handles.begin(); it != handles.end();) {  // закончившиеся закрываем
            DWORD code = 0;
            if (GetExitCodeProcess(it->second, &code) && code != STILL_ACTIVE &&
                std::none_of(procs.begin(), procs.end(), [&](const guard::Proc& p) { return p.pid == it->first; })) {
                CloseHandle(it->second);
                it = handles.erase(it);
            } else {
                ++it;
            }
        }
        AcquireSRWLockExclusive(&g_guardLock);
        g_awake = awake;
        g_lowered = bal.Lowered();
        ReleaseSRWLockExclusive(&g_guardLock);
    } while (WaitForSingleObject(g_guardStop, 5000) == WAIT_TIMEOUT);
    // выход: приоритеты - как были, сон - как был
    for (DWORD pid : lowered) SetPrio(pid, BELOW_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS);
    SetThreadExecutionState(ES_CONTINUOUS);
    for (auto& [pid, h] : handles) CloseHandle(h);
    return 0;
}

std::wstring Minutes(ULONGLONG ms) {
    ULONGLONG m = (ms + 30000) / 60000;
    if (m < 60) return Plural((int)std::max<ULONGLONG>(1, m), L"минуту", L"минуты", L"минут");
    return std::to_wstring(m / 60) + L" ч " + std::to_wstring(m % 60) + L" мин";
}

}  // namespace

void GuardStart() {
    if (g_guardThread) return;
    g_guardStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_guardThread = CreateThread(nullptr, 0, GuardLoop, nullptr, 0, nullptr);
}

void GuardStop() {
    if (g_guardStop) SetEvent(g_guardStop);
    if (g_guardThread) WaitForSingleObject(g_guardThread, 4000), CloseHandle(g_guardThread);
    g_guardThread = nullptr;
}

// из Tick (основной поток): снимок настроек для потока, журнал и карточки из очереди
void GuardTick() {
    ULONGLONG now = GetTickCount64();
    if (now - g_gcfgAt > 2000 && g_cfg) {
        g_gcfgAt = now;
        GuardCfg c;
        c.balance = g_cfg->cpuBalance, c.noSleep = g_cfg->noSleep, c.longWatch = g_cfg->longWatch;
        c.noSleepApps = g_cfg->noSleepApps, c.longApps = g_cfg->longApps, c.keepFast = g_cfg->boostKeepFast;
        c.longMin = g_cfg->longMin;
        c.procRules = g_cfg->procRules;
        AcquireSRWLockExclusive(&g_guardLock);
        g_gcfg = c;
        ReleaseSRWLockExclusive(&g_guardLock);
    }
    AcquireSRWLockExclusive(&g_guardLock);
    std::vector<std::wstring> notes = std::move(g_guardNotes);
    std::vector<LongDone> done = std::move(g_longDone);
    g_guardNotes.clear();
    g_longDone.clear();
    ReleaseSRWLockExclusive(&g_guardLock);
    for (const std::wstring& n : notes) autos::Note(n);
    for (const LongDone& d : done) {
        bool fail = d.haveCode && d.code != 0;
        std::wstring name = NoExe(d.name);
        autos::Note(L"Долгая команда: " + name + (fail ? L" упала" : L" закончилась") + L" за " + Minutes(d.ms));
        if (g_busy) continue;  // карточка занята делом - хватит журнала
        g_card.Show(BoostCard::DONE, name + (fail ? L": упало за " : L": готово за ") + Minutes(d.ms),
                    fail ? L"Код выхода " + std::to_wstring((int)d.code) + L". Подробности - в окне, где запускали."
                         : L"Закончилось, пока вы были в другом окне.",
                    Theme(), 15000, fail ? Toast::G_FAIL : Toast::G_OK);
    }
}

std::wstring AwakeReason() {
    AcquireSRWLockShared(&g_guardLock);
    std::wstring s = g_awake;
    ReleaseSRWLockShared(&g_guardLock);
    return s;
}

std::vector<std::wstring> BalanceLowered() {
    AcquireSRWLockShared(&g_guardLock);
    std::vector<std::wstring> v;
    for (const auto& a : g_lowered) v.push_back(a.name);
    ReleaseSRWLockShared(&g_guardLock);
    return v;
}

void BalanceRestoreAll() { g_restoreAll = true; }

}  // namespace toolsui
