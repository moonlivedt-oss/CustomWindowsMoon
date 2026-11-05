#include "core/usage.h"
// Яркость по программе: основной поток замечает смену программы впереди, свой поток читает и ставит яркость.
#include <objbase.h>
#include <shlobj.h>

#include <atomic>

#include "system/auto/autos.h"
#include "core/config.h"
#include "core/text.h"
#include "desktop/brightness.h"
#include "desktop/appbright.h"

namespace appbright {

namespace {

const Config* g_cfg = nullptr;
bool (*g_paused)() = nullptr;
HANDLE g_thread = nullptr, g_wake = nullptr, g_stop = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_want;  // программа впереди (для потока)
HMONITOR g_mon = nullptr;
std::wstring g_rulesText;  // правила из radial.cfg (поток перечитывает, если поменялись)
std::wstring g_seenExe;
DWORD g_seenPid = 0;
ULONGLONG g_at = 0;

std::wstring File() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir.empty() ? L"" : dir + L"\\MoonDesk\\appbright.tsv";
}

std::wstring ReadLearned() {
    HANDLE f = CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string u(std::min<DWORD>(GetFileSize(f, nullptr), 1 << 16), '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    return Trim(Widen(u));
}

void WriteLearned(const std::wstring& t) {
    std::string u = Narrow(t);
    HANDLE f = CreateFileW(File().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
    CloseHandle(f);
}

std::wstring ExeOf(HWND w, DWORD& pid) {
    pid = 0;
    GetWindowThreadProcessId(w, &pid);
    static DWORD cachePid = 0;
    static std::wstring cacheExe;
    if (pid == cachePid) return cacheExe;
    std::wstring name;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
            name = buf;
            name = name.substr(name.find_last_of(L'\\') + 1);
        }
        CloseHandle(p);
    }
    cachePid = pid;
    cacheExe = name;
    return name;
}

DWORD WINAPI Worker(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Planner plan;
    std::wstring rulesFrom;
    HANDLE h[2] = {g_stop, g_wake};
    while (WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        AcquireSRWLockExclusive(&g_lock);
        std::wstring exe = g_want, rules = g_rulesText;
        HMONITOR mon = g_mon;
        ReleaseSRWLockExclusive(&g_lock);
        if (rules != rulesFrom) {  // свои правила, поверх - выученные поправки
            rulesFrom = rules;
            plan.rules = ParseRules(rules);
            for (auto& [k, r] : ParseRules(ReadLearned())) plan.rules[k] = r;
        }
        int cur = -1;
        if (!brightness::Get(mon, cur)) continue;
        if (!plan.Active().empty() && Lower(exe) != plan.Active() && plan.Learn(cur)) {
            std::map<std::wstring, Rule> learned = ParseRules(ReadLearned());
            learned[plan.Active()] = plan.rules[plan.Active()];
            WriteLearned(SaveRules(learned));
            autos::Note(L"Яркость по программе: для " + plan.Active() + L" запомнено " +
                        SaveRules({{plan.Active(), plan.rules[plan.Active()]}}));
        }
        int set = plan.OnApp(exe, cur);
        if (set >= 0 && set != cur && brightness::Set(mon, set)) usage::Hit(L"bright_by_app");
    }
    CoUninitialize();
    return 0;
}

}  // namespace

void Init(const Config* cfg, bool (*paused)()) {
    g_cfg = cfg;
    g_paused = paused;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

void Tick() {
    ULONGLONG now = GetTickCount64();
    if (!g_cfg || !g_thread || now - g_at < 700) return;
    g_at = now;
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    DWORD pid = 0;
    std::wstring exe = ExeOf(fg, pid);
    // выключили или игра - как будто впереди обычная программа: яркость вернётся
    if (!g_cfg->brightByApp || (g_paused && g_paused())) exe = L"-";
    if (exe == g_seenExe && pid == g_seenPid && g_rulesText == g_cfg->brightApps) return;
    g_seenExe = exe;
    g_seenPid = pid;
    AcquireSRWLockExclusive(&g_lock);
    g_want = exe;
    g_rulesText = g_cfg->brightApps;
    g_mon = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_wake);
}

void Shutdown() {
    if (!g_thread) return;
    AcquireSRWLockExclusive(&g_lock);
    g_want = L"-";  // выходим - яркость как была
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_wake);
    Sleep(300);
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

}  // namespace appbright
