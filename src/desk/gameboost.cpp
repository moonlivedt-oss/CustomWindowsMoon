#include "desk/gameboost.h"

#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <set>

#include "core/files.h"
#include "core/text.h"
#include "desk/boost.h"
#include "tools/builderr.h"
#include "tools/tools.h"

namespace gboost {

namespace {

// ---------------- общее ----------------

std::wstring DataDir() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", ad, MAX_PATH);
    CreateDirectoryW(ad, nullptr);
    return ad;
}

std::wstring StateFile() { return DataDir() + L"\\gameboost.ini"; }

std::string ReadAll(const std::wstring& file) {
    HANDLE f =
        CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s;
    char buf[16384];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) s.append(buf, got);
    CloseHandle(f);
    return s;
}

void WriteAll(const std::wstring& file, const std::string& s, bool append = false) {
    HANDLE f = CreateFileW(file.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &put, nullptr);
    CloseHandle(f);
}

std::vector<std::wstring> Lines(const std::wstring& file) {
    std::vector<std::wstring> out;
    for (std::wstring& l : Split(Widen(ReadAll(file)), L'\n'))
        if (!l.empty()) out.push_back(l);
    return out;
}

std::vector<std::wstring> Cells(const std::wstring& line) {
    std::vector<std::wstring> out;
    size_t p = 0;
    for (;;) {
        size_t t = line.find(L'\t', p);
        out.push_back(line.substr(p, t == std::wstring::npos ? std::wstring::npos : t - p));
        if (t == std::wstring::npos) break;
        p = t + 1;
    }
    return out;
}

std::wstring StripExe(const std::wstring& n) { return EndsWithI(n, L".exe") ? n.substr(0, n.size() - 4) : n; }

bool InList(const std::wstring& name, const std::vector<std::wstring>& list) {
    for (const std::wstring& m : list)
        if (!_wcsicmp(name.c_str(), m.c_str()) ||
            ((m.find(L'*') != std::wstring::npos || m.find(L'?') != std::wstring::npos) &&
             PathMatchSpecW(name.c_str(), m.c_str())))
            return true;
    return false;
}

ULONGLONG Created(HANDLE h) {
    FILETIME c{}, e{}, k{}, u{};
    return GetProcessTimes(h, &c, &e, &k, &u) ? ((ULONGLONG)c.dwHighDateTime << 32 | c.dwLowDateTime) : 0;
}

// ---------------- изменённые процессы ----------------

struct Change {
    DWORD pid = 0;
    ULONGLONG created = 0;
    DWORD oldPrio = 0;  // 0 - приоритет не меняли
    bool eco = false;
    wchar_t tag = L'g';  // g - игра и фон, s - усыплённые, c - компиляция
};
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Change> g_changes;
std::wstring g_quietWas;  // прежнее состояние тишины («wnf:N» или «reg:N»), пусто - не трогали

void SaveState() {
    std::wstring s;
    if (!g_quietWas.empty()) s += L"quiet\t" + g_quietWas + L"\n";
    for (const Change& c : g_changes)
        s += L"proc\t" + std::to_wstring(c.pid) + L"\t" + std::to_wstring(c.created) + L"\t" +
             std::to_wstring(c.oldPrio) + L"\t" + (c.eco ? L"1" : L"0") + L"\t" + std::wstring(1, c.tag) + L"\n";
    if (s.empty())
        DeleteFileW(StateFile().c_str());
    else
        WriteAll(StateFile(), Narrow(s));
}

bool SetEco(HANDLE h, bool on) {
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    st.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    return SetProcessInformation(h, ProcessPowerThrottling, &st, sizeof(st));
}

bool Known(DWORD pid) {
    for (const Change& c : g_changes)
        if (c.pid == pid) return true;
    return false;
}

// Изменить процесс и запомнить, как было. prio 0 - приоритет не трогать.
bool Tame(DWORD pid, DWORD prio, bool eco, wchar_t tag) {
    if (Known(pid)) return false;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    Change c{pid, Created(h), 0, false, tag};
    if (prio) {
        DWORD old = GetPriorityClass(h);
        if (old && old != prio && SetPriorityClass(h, prio)) c.oldPrio = old;
    }
    if (eco) c.eco = SetEco(h, true);
    CloseHandle(h);
    if (!c.oldPrio && !c.eco) return false;
    g_changes.push_back(c);
    return true;
}

void Untame(const Change& c) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, c.pid);
    if (!h) return;
    if (Created(h) == c.created) {  // тот же процесс, а не тёзка по номеру
        if (c.oldPrio) SetPriorityClass(h, c.oldPrio);
        if (c.eco) SetEco(h, false);
    }
    CloseHandle(h);
}

// Процессы с видимыми окнами верхнего уровня (то, что человек видит на панели задач).
std::set<DWORD> WindowPids() {
    std::set<DWORD> out;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            reinterpret_cast<std::set<DWORD>*>(lp)->insert(pid);
            return TRUE;
        },
        (LPARAM)&out);
    return out;
}

std::map<DWORD, std::wstring> ProcessNames() {
    std::map<DWORD, std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        out[pe.th32ProcessID] = StripExe(pe.szExeFile);
    CloseHandle(snap);
    return out;
}

// всегда в стороне: оболочка, звук, сам RadialMenu, туннель
const std::vector<std::wstring> kNever = {L"explorer",      L"dwm",
                                          L"audiodg",       L"RadialMenu",
                                          L"RadialTools",   L"ShellHost",
                                          L"SearchHost",    L"StartMenuExperienceHost",
                                          L"TextInputHost", L"ApplicationFrameHost",
                                          L"Happ",          L"happd",
                                          L"xray",          L"sing-box",
                                          L"Start11_64",    L"wallpaper64",
                                          L"wallpaper32"};

int EcoWindows(const std::set<DWORD>& skipPids, const std::vector<std::wstring>& skipNames,
               const std::vector<std::wstring>& guarded, wchar_t tag) {
    std::map<DWORD, std::wstring> names = ProcessNames();
    DWORD self = GetCurrentProcessId();
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    int n = 0;
    for (DWORD pid : WindowPids()) {
        if (pid == self || pid <= 4 || skipPids.count(pid)) continue;
        auto it = names.find(pid);
        if (it == names.end()) continue;
        const std::wstring& nm = it->second;
        if (InList(nm, kNever) || InList(nm, skipNames) || InList(nm, guarded)) continue;
        // окно на переднем плане при компиляции - то, с чем человек работает
        if (tag == L'c' && pid == fgPid) continue;
        if (Tame(pid, tag == L'c' ? 0 : BELOW_NORMAL_PRIORITY_CLASS, true, tag)) ++n;
    }
    return n;
}

// ---------------- PDH ----------------

// Счётчик с * вместо экземпляра: значения всех экземпляров.
struct Multi {
    PDH_HCOUNTER c = nullptr;
    std::vector<std::pair<std::wstring, double>> Read() const {
        std::vector<std::pair<std::wstring, double>> out;
        if (!c) return out;
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) !=
            (PDH_STATUS)PDH_MORE_DATA)
            return out;
        std::vector<BYTE> buf(size);
        auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS)
            return out;
        for (DWORD i = 0; i < count; ++i)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                out.push_back({items[i].szName, items[i].FmtValue.doubleValue});
        return out;
    }
};

double One(PDH_HCOUNTER c) {
    PDH_FMT_COUNTERVALUE v{};
    if (!c || PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS) return 0;
    return v.doubleValue;
}

// «luid_0x00000000_0x0000D1C5» внутри имени экземпляра
bool ParseLuid(const std::wstring& inst, LUID& out) {
    size_t p = inst.find(L"luid_0x");
    if (p == std::wstring::npos) return false;
    unsigned long hi = 0, lo = 0;
    if (swscanf(inst.c_str() + p, L"luid_0x%lx_0x%lx", &hi, &lo) != 2) return false;
    out.HighPart = (LONG)hi;
    out.LowPart = lo;
    return true;
}

bool SameLuid(const LUID& a, const LUID& b) { return a.HighPart == b.HighPart && a.LowPart == b.LowPart; }

// На какой видеокарте рисует процесс: сумма загрузки движков 3D по видеокартам за секунду.
bool GpuOfPids(const std::set<DWORD>& pids, LUID& out, double& load) {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return false;
    Multi eng;
    PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &eng.c);
    PdhCollectQueryData(q);
    Sleep(1200);
    PdhCollectQueryData(q);
    std::vector<std::pair<LUID, double>> sums;
    for (auto& [inst, v] : eng.Read()) {
        if (inst.find(L"engtype_3D") == std::wstring::npos) continue;
        DWORD pid = (DWORD)_wtoi(inst.c_str() + (inst.rfind(L"pid_", 0) == 0 ? 4 : 0));
        if (!pids.count(pid)) continue;
        LUID l;
        if (!ParseLuid(inst, l)) continue;
        auto it = std::find_if(sums.begin(), sums.end(), [&](auto& s) { return SameLuid(s.first, l); });
        if (it == sums.end())
            sums.push_back({l, v});
        else
            it->second += v;
    }
    PdhCloseQuery(q);
    if (sums.empty()) return false;
    auto best = std::max_element(sums.begin(), sums.end(), [](auto& a, auto& b) { return a.second < b.second; });
    out = best->first;
    load = best->second;
    return true;
}

// ---------------- слежка ----------------

HWND g_notify = nullptr;
HANDLE g_stop = nullptr, g_thread = nullptr;
// от главного окна (под g_lock)
bool g_game = false, g_dev = false, g_islc = false;
std::vector<DWORD> g_gamePids;
std::wstring g_gameExe;
ULONGLONG g_gameAt = 0;
int g_gpuChecks = 0;
Session g_sess[2];  // 0 - игра, 1 - редактор
bool g_sessOn[2] = {};
double g_pageStart[2] = {};

void Post(Event ev, double a = 0, double b = 0, const std::wstring& text = L"", const std::wstring& sub = L"",
          const std::wstring& go = L"") {
    if (!g_notify) return;
    auto* n = new Note{ev, a, b, text, sub, go};
    if (!PostMessageW(g_notify, WM_GBOOST, ev, (LPARAM)n)) delete n;
}

std::wstring UbtLog() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\UnrealBuildTool\\Log.txt", p, MAX_PATH);
    return p;
}

ULONGLONG WriteTime(const std::wstring& file) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
}

// Итог сборки из лога UBT: «Result: Succeeded», «Total execution time: 124.13 seconds», первая ошибка.
void ParseBuild(bool dev) {
    std::wstring t = Widen(ReadAll(UbtLog()));
    size_t r = t.rfind(L"Result: ");
    if (r == std::wstring::npos) return;
    bool ok = t.compare(r + 8, 9, L"Succeeded") == 0;
    double secs = 0;
    size_t e = t.rfind(L"Total execution time: ");
    if (e != std::wstring::npos) secs = _wtof(t.c_str() + e + 22);
    if (!dev && secs < 20) return;  // мелкая сборка из IDE без редактора - не новость
    std::wstring err, target, go;
    for (std::wstring& line : Split(t, L'\n')) {
        if (target.empty() && line.rfind(L"Building ", 0) == 0) {
            target = line.substr(9);
            if (!target.empty() && target.back() == L'.') target.erase(target.find_last_not_of(L'.') + 1);
        }
    }
    // первая настоящая ошибка понятным языком (Л31): файл и строка, что обычно помогает, место для VS Code
    builderr::Error be;
    if (!ok && builderr::First(t, be)) {
        err = builderr::Plain(be);
        std::wstring hint = builderr::Hint(be);
        if (!hint.empty()) err += L"\n" + hint;
        go = builderr::GotoArg(be, L"");
    }
    Post(EV_BUILD_DONE, secs, ok ? 1 : 0, err, target, go);
}

int CountWorkers() {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"ShaderCompileWorker.exe")) ++n;
    CloseHandle(snap);
    return n;
}

DWORD WINAPI Loop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER cFree = nullptr, cStbN = nullptr, cStbR = nullptr, cStbC = nullptr, cFaults = nullptr;
    Multi vram;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        PdhAddEnglishCounterW(q, L"\\Memory\\Free & Zero Page List Bytes", 0, &cFree);
        PdhAddEnglishCounterW(q, L"\\Memory\\Standby Cache Normal Priority Bytes", 0, &cStbN);
        PdhAddEnglishCounterW(q, L"\\Memory\\Standby Cache Reserve Bytes", 0, &cStbR);
        PdhAddEnglishCounterW(q, L"\\Memory\\Standby Cache Core Bytes", 0, &cStbC);
        PdhAddEnglishCounterW(q, L"\\Memory\\Pages Input/sec", 0, &cFaults);
        PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vram.c);
        PdhCollectQueryData(q);
    }
    // видеокарта, у которой больше всего своей памяти - та, за чьей памятью следим
    Adapter big;
    for (const Adapter& a : Adapters())
        if (a.vramMb > big.vramMb) big = a;

    ULONGLONG ubtSeen = WriteTime(UbtLog()), ubtPending = 0, ubtChangedAt = 0;
    ULONGLONG lastPurge = 0, vramWarned = 0, shaderFrom = 0;
    int workersSeen = 0, workersPeak = 0, zeroRuns = 0;
    bool compiling = false;
    while (WaitForSingleObject(g_stop, 5000) == WAIT_TIMEOUT) {
        ULONGLONG now = GetTickCount64();
        AcquireSRWLockShared(&g_lock);
        bool game = g_game, dev = g_dev, islc = g_islc;
        std::vector<DWORD> pids = g_gamePids;
        std::wstring exe = g_gameExe;
        ULONGLONG gameAt = g_gameAt;
        int gpuChecks = g_gpuChecks;
        ReleaseSRWLockShared(&g_lock);

        // сборка UBT: лог поменялся и 4 секунды не меняется - сборка кончилась
        ULONGLONG wt = WriteTime(UbtLog());
        if (wt && wt != ubtSeen) {
            if (wt != ubtPending)
                ubtPending = wt, ubtChangedAt = now;
            else if (now - ubtChangedAt >= 4000) {
                ubtSeen = wt;
                ubtPending = 0;
                ParseBuild(dev);
            }
        }
        if (!game && !dev) continue;

        if (q) PdhCollectQueryData(q);
        double freeMb = One(cFree) / 1048576, standbyMb = (One(cStbN) + One(cStbR) + One(cStbC)) / 1048576;
        double faults = One(cFaults);
        double vramUsed = 0;
        for (auto& [inst, v] : vram.Read()) {
            LUID l;
            if (ParseLuid(inst, l) && SameLuid(l, big.luid)) vramUsed += v / 1048576;
        }
        MEMORYSTATUSEX ms{sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        double totalMb = ms.ullTotalPhys / 1048576.0, usedMb = totalMb - ms.ullAvailPhys / 1048576.0;
        double commitMb = (ms.ullTotalPageFile - ms.ullAvailPageFile) / 1048576.0;

        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < 2; ++i) {
            if (!g_sessOn[i]) continue;
            Session& s = g_sess[i];
            s.samples++;
            s.totalMb = totalMb;
            s.limitMb = ms.ullTotalPageFile / 1048576.0;
            s.peakUsedMb = std::max(s.peakUsedMb, usedMb);
            s.peakCommitMb = std::max(s.peakCommitMb, commitMb);
            s.maxFaults = std::max(s.maxFaults, faults);
            if (faults > 800) s.stallSamples++;
            s.peakVramMb = std::max(s.peakVramMb, vramUsed);
            s.vramMb = big.vramMb;
        }
        ReleaseSRWLockExclusive(&g_lock);

        // кэш ожидания раздут, а свободной памяти почти нет: игра будет ждать, пока Windows отдаст страницы
        if (game && islc && freeMb < 1024 && standbyMb > 1024 && now - lastPurge > 120000 && boost::PurgeTaskExists()) {
            lastPurge = now;
            if (boost::RunPurgeAreas({L"StandbyList"})) {
                Sleep(1500);
                AcquireSRWLockExclusive(&g_lock);
                if (g_sessOn[0]) g_sess[0].purges++;
                ReleaseSRWLockExclusive(&g_lock);
                MEMORYSTATUSEX m2{sizeof(m2)};
                GlobalMemoryStatusEx(&m2);
                Post(EV_PURGED, m2.ullAvailPhys / 1048576.0);
            }
        }

        // на какой видеокарте игра: через 25 с после старта (меню и заставки - на чём попало), потом ещё раз
        if (game && !pids.empty() && gpuChecks < 2 && now - gameAt > (gpuChecks ? 90000ull : 25000ull)) {
            AcquireSRWLockExclusive(&g_lock);
            g_gpuChecks++;
            ReleaseSRWLockExclusive(&g_lock);
            LUID l;
            double load = 0;
            std::vector<Adapter> ads = Adapters();
            bool hasDiscrete = std::any_of(ads.begin(), ads.end(), [](const Adapter& a) { return !a.integrated; });
            if (hasDiscrete && GpuOfPids(std::set<DWORD>(pids.begin(), pids.end()), l, load) && load > 3) {
                auto it = std::find_if(ads.begin(), ads.end(), [&](const Adapter& a) { return SameLuid(a.luid, l); });
                if (it != ads.end() && it->integrated) {
                    std::wstring discrete;
                    for (const Adapter& a : ads)
                        if (!a.integrated) discrete = a.name;
                    Post(EV_GPU_WRONG, load, 0, exe, it->name + L"|" + discrete);
                    AcquireSRWLockExclusive(&g_lock);
                    g_gpuChecks = 2;
                    ReleaseSRWLockExclusive(&g_lock);
                }
            }
        }

        if (dev) {
            // видеопамять почти кончилась: Unreal упадёт с «Out of video memory», лучше сказать заранее
            if (big.vramMb > 0 && vramUsed > big.vramMb * 0.92 && (!vramWarned || now - vramWarned > 10 * 60000)) {
                vramWarned = now;
                Post(EV_VRAM_HIGH, vramUsed, big.vramMb);
            }
            // шейдеры: рабочие ShaderCompileWorker держатся два замера подряд - компиляция идёт
            int w = CountWorkers();
            if (w > 0) {
                zeroRuns = 0;
                workersPeak = std::max(workersPeak, w);
                if (!compiling && ++workersSeen >= 2) {
                    compiling = true;
                    shaderFrom = now - 5000;
                    Post(EV_SHADERS_START, w);
                }
            } else {
                workersSeen = 0;
                if (compiling && ++zeroRuns >= 2) {
                    compiling = false;
                    Post(EV_SHADERS_DONE, (now - shaderFrom) / 1000.0 - 5, workersPeak);
                    workersPeak = 0;
                }
            }
        } else if (compiling) {
            compiling = false;  // редактор закрыли посреди компиляции
            workersPeak = workersSeen = zeroRuns = 0;
            Post(EV_SHADERS_DONE, -1, 0);
        }
    }
    if (q) PdhCloseQuery(q);
    CoUninitialize();
    return 0;
}

}  // namespace

// ---------------- процессы ----------------

int GameFirst(const std::vector<DWORD>& game, const std::vector<std::wstring>& keepFast,
              const std::vector<std::wstring>& guarded) {
    AcquireSRWLockExclusive(&g_lock);
    for (DWORD pid : game) Tame(pid, HIGH_PRIORITY_CLASS, false, L'g');
    int n = EcoWindows(std::set<DWORD>(game.begin(), game.end()), keepFast, guarded, L'g');
    SaveState();
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

int BackgroundEco(const std::vector<std::wstring>& exclude, const std::vector<std::wstring>& guarded) {
    AcquireSRWLockExclusive(&g_lock);
    int n = EcoWindows({}, exclude, guarded, L'c');
    SaveState();
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

double Soften(const std::vector<DWORD>& pids) {
    double freed = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (DWORD pid : pids) {
        HANDLE h =
            OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) continue;
        PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
        double before = GetProcessMemoryInfo(h, &pmc, sizeof(pmc)) ? pmc.WorkingSetSize / 1048576.0 : 0;
        EmptyWorkingSet(h);
        double after = GetProcessMemoryInfo(h, &pmc, sizeof(pmc)) ? pmc.WorkingSetSize / 1048576.0 : before;
        freed += std::max(0.0, before - after);
        CloseHandle(h);
        // уже приглушён как фон - переделать в «усыплён»
        for (size_t i = 0; i < g_changes.size(); ++i)
            if (g_changes[i].pid == pid) {
                Untame(g_changes[i]);
                g_changes.erase(g_changes.begin() + i);
                break;
            }
        Tame(pid, IDLE_PRIORITY_CLASS, true, L's');
    }
    SaveState();
    ReleaseSRWLockExclusive(&g_lock);
    return freed;
}

void Restore(wchar_t tag) {
    AcquireSRWLockExclusive(&g_lock);
    std::vector<Change> keep;
    for (const Change& c : g_changes)
        if (!tag || c.tag == tag)
            Untame(c);
        else
            keep.push_back(c);
    g_changes = std::move(keep);
    SaveState();
    ReleaseSRWLockExclusive(&g_lock);
}

void RestoreAll() { Restore(0); }

int Tamed() {
    AcquireSRWLockShared(&g_lock);
    int n = (int)g_changes.size();
    ReleaseSRWLockShared(&g_lock);
    return n;
}

// ---------------- тишина ----------------
// «Не беспокоить» Windows живёт в WNF: WNF_SHEL_QUIETHOURS_ACTIVE_PROFILE_CHANGED (0 - выключено,
// 1 - только важные, 2 - только будильники). Так его переключают и сторонние утилиты; права не нужны.
// Не вышло - запасной путь: общий выключатель всплывающих уведомлений в HKCU.

namespace {
constexpr ULONG64 kQuietWnf = 0x0D83063EA3BF1C75ull;
using NtUpdateWnf = LONG(NTAPI*)(const ULONG64*, const void*, ULONG, const void*, const void*, ULONG, ULONG);
using NtQueryWnf = LONG(NTAPI*)(const ULONG64*, const void*, const void*, ULONG*, void*, ULONG*);
const wchar_t kNoc[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings";

bool WnfGet(DWORD& v) {
    static auto q = (NtQueryWnf)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryWnfStateData");
    if (!q) return false;
    ULONG stamp = 0, size = sizeof(v);
    v = 0;
    return q(&kQuietWnf, nullptr, nullptr, &stamp, &v, &size) >= 0;
}

bool WnfSet(DWORD v) {
    static auto u = (NtUpdateWnf)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtUpdateWnfStateData");
    return u && u(&kQuietWnf, &v, sizeof(v), nullptr, nullptr, 0, 0) >= 0;
}

void QuietBack(const std::wstring& was) {
    if (was.rfind(L"wnf:", 0) == 0) {
        WnfSet((DWORD)_wtoi(was.c_str() + 4));
    } else if (was.rfind(L"reg:", 0) == 0) {
        int v = _wtoi(was.c_str() + 4);
        if (v < 0)
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kNoc, L"NOC_GLOBAL_SETTING_TOASTS_ENABLED");
        else
            RegSetKeyValueW(HKEY_CURRENT_USER, kNoc, L"NOC_GLOBAL_SETTING_TOASTS_ENABLED", REG_DWORD, &v, 4);
    }
}
}  // namespace

void Quiet(bool on) {
    AcquireSRWLockExclusive(&g_lock);
    if (on && g_quietWas.empty()) {
        DWORD cur = 0;
        if (WnfGet(cur)) {
            if (cur != 2 && WnfSet(2)) g_quietWas = L"wnf:" + std::to_wstring(cur);
        } else {
            DWORD v = 0, size = 4;
            int old = RegGetValueW(HKEY_CURRENT_USER, kNoc, L"NOC_GLOBAL_SETTING_TOASTS_ENABLED", RRF_RT_REG_DWORD,
                                   nullptr, &v, &size) == ERROR_SUCCESS
                          ? (int)v
                          : -1;
            DWORD zero = 0;
            if (RegSetKeyValueW(HKEY_CURRENT_USER, kNoc, L"NOC_GLOBAL_SETTING_TOASTS_ENABLED", REG_DWORD, &zero, 4) ==
                ERROR_SUCCESS)
                g_quietWas = L"reg:" + std::to_wstring(old);
        }
        SaveState();
    } else if (!on && !g_quietWas.empty()) {
        QuietBack(g_quietWas);
        g_quietWas.clear();
        SaveState();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool QuietOn() {
    AcquireSRWLockShared(&g_lock);
    bool on = !g_quietWas.empty();
    ReleaseSRWLockShared(&g_lock);
    return on;
}

void Recover() {
    std::vector<std::wstring> lines = Lines(StateFile());
    if (lines.empty()) return;
    for (const std::wstring& l : lines) {
        std::vector<std::wstring> c = Cells(Trim(l));
        if (c.size() >= 2 && c[0] == L"quiet") QuietBack(c[1]);
        if (c.size() >= 6 && c[0] == L"proc")
            Untame({(DWORD)_wtoi(c[1].c_str()), _wcstoui64(c[2].c_str(), nullptr, 10), (DWORD)_wtoi(c[3].c_str()),
                    c[4] == L"1", c[5].empty() ? L'g' : c[5][0]});
    }
    DeleteFileW(StateFile().c_str());
}

// ---------------- видеокарта ----------------

std::vector<Adapter> Adapters() {
    std::vector<Adapter> out;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return out;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            Adapter x;
            x.luid = d.AdapterLuid;
            x.name = Trim(d.Description);
            x.vramMb = d.DedicatedVideoMemory / 1048576.0;
            // встроенная: своей памяти почти нет (Intel UHD, Radeon Graphics в процессоре)
            x.integrated = x.vramMb < 1536;
            out.push_back(x);
        }
        a->Release();
    }
    f->Release();
    return out;
}

bool PreferDiscrete(const std::wstring& exe) {
    if (exe.empty()) return false;
    const wchar_t v[] = L"GpuPreference=2;";
    return RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", exe.c_str(), REG_SZ,
                           v, sizeof(v)) == ERROR_SUCCESS;
}

// ---------------- профили ----------------

std::map<std::wstring, int> Profile(const std::wstring& app) {
    std::map<std::wstring, int> out;
    std::wstring key = Lower(app);
    for (const std::wstring& l : Lines(DataDir() + L"\\boost_profiles.tsv")) {
        std::vector<std::wstring> c = Cells(Trim(l));
        if (c.size() >= 3 && c[0] == key) out[c[1]] = std::clamp(_wtoi(c[2].c_str()), 0, 2);
    }
    return out;
}

void SaveProfile(const std::wstring& app, const std::map<std::wstring, int>& acts) {
    std::wstring key = Lower(app), file = DataDir() + L"\\boost_profiles.tsv", s;
    for (const std::wstring& l : Lines(file)) {
        std::vector<std::wstring> c = Cells(Trim(l));
        if (c.size() >= 3 && !(c[0] == key && acts.count(c[1]))) s += Trim(l) + L"\n";
    }
    for (auto& [prog, act] : acts) s += key + L"\t" + Lower(prog) + L"\t" + std::to_wstring(act) + L"\n";
    WriteAll(file, Narrow(s));
}

// ---------------- слежка и сессия ----------------

void Start(HWND notify) {
    if (g_thread) return;
    g_notify = notify;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
}

void Stop() {
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    g_thread = g_stop = nullptr;
}

namespace {
void BeginSession(int i, const std::wstring& name) {
    g_sess[i] = Session{};
    g_sess[i].name = name;
    g_sess[i].game = i == 0;
    g_sess[i].ms = GetTickCount64();
    g_sessOn[i] = true;
    g_pageStart[i] = tools::PagefileUsed() / 1048576.0;
}
}  // namespace

void SetGame(bool on, const std::vector<DWORD>& pids, const std::wstring& exe, const std::wstring& name, bool islc) {
    double page = on && !g_sessOn[0] ? tools::PagefileUsed() / 1048576.0 : 0;  // PDH - вне замка
    AcquireSRWLockExclusive(&g_lock);
    g_game = on;
    g_islc = islc;
    g_gamePids = pids;
    g_gameExe = exe;
    if (on && !g_sessOn[0]) {
        g_gameAt = GetTickCount64();
        g_gpuChecks = 0;
        BeginSession(0, name);
        g_pageStart[0] = page;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void SetDev(bool on, const std::wstring& name) {
    double page = on && !g_sessOn[1] ? tools::PagefileUsed() / 1048576.0 : 0;
    AcquireSRWLockExclusive(&g_lock);
    g_dev = on;
    if (on && !g_sessOn[1]) {
        BeginSession(1, name);
        g_pageStart[1] = page;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

Session TakeSession(bool game) {
    int i = game ? 0 : 1;
    double page = tools::PagefileUsed() / 1048576.0;
    AcquireSRWLockExclusive(&g_lock);
    Session s = g_sess[i];
    if (g_sessOn[i]) {
        s.ms = GetTickCount64() - s.ms;
        s.pageGrowMb = std::max(0.0, page - g_pageStart[i]);
    } else {
        s = Session{};
    }
    g_sessOn[i] = false;
    ReleaseSRWLockExclusive(&g_lock);
    return s;
}

void SaveSession(const Session& s) {
    if (s.name.empty() || s.ms < 60000) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t day[32];
    swprintf(day, 32, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    wchar_t row[512];
    swprintf(row, 512, L"%ls\t%ls\t%d\t%llu\t%.0f\t%.0f\t%.0f\t%.0f\t%d\t%d\t%d\t%.0f\t%.0f\n", day, s.name.c_str(),
             s.game ? 1 : 0, s.ms, s.peakUsedMb, s.totalMb, s.pageGrowMb, s.maxFaults, s.stallSamples, s.samples,
             s.purges, s.peakVramMb, s.vramMb);
    WriteAll(DataDir() + L"\\boost_sessions.tsv", Narrow(row), true);
}

std::vector<Session> History(size_t max) {
    std::vector<Session> out;
    std::vector<std::wstring> lines = Lines(DataDir() + L"\\boost_sessions.tsv");
    for (size_t i = lines.size(); i-- > 0 && out.size() < max;) {
        std::vector<std::wstring> c = Cells(Trim(lines[i]));
        if (c.size() < 13) continue;
        Session s;
        s.name = c[1];
        s.game = c[2] == L"1";
        s.ms = _wcstoui64(c[3].c_str(), nullptr, 10);
        s.peakUsedMb = _wtof(c[4].c_str());
        s.totalMb = _wtof(c[5].c_str());
        s.pageGrowMb = _wtof(c[6].c_str());
        s.maxFaults = _wtof(c[7].c_str());
        s.stallSamples = _wtoi(c[8].c_str());
        s.samples = _wtoi(c[9].c_str());
        s.purges = _wtoi(c[10].c_str());
        s.peakVramMb = _wtof(c[11].c_str());
        s.vramMb = _wtof(c[12].c_str());
        out.push_back(s);
    }
    return out;
}

// ---------------- Unreal ----------------

std::wstring UnrealExclusions(const std::wstring& editorExe) {
    std::vector<std::wstring> dirs;
    auto add = [&](std::wstring d) {
        if (d.empty() || !IsFolder(d)) return;
        for (const std::wstring& x : dirs)
            if (!_wcsicmp(x.c_str(), d.c_str())) return;
        dirs.push_back(d);
    };
    // движок: ...\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe -> ...\UE_5.8
    std::wstring root = editorExe;
    for (int i = 0; i < 4 && !root.empty(); ++i) root = ParentDir(root);
    add(root);
    // проекты из «недавних» редактора, кроме временных
    wchar_t la[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\UnrealEngine", la, MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(la) + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring ini =
                std::wstring(la) + L"\\" + fd.cFileName + L"\\Saved\\Config\\WindowsEditor\\EditorSettings.ini";
            for (const std::wstring& line : Split(Widen(ReadAll(ini)), L'\n')) {
                size_t p = line.find(L"ProjectName=\"");
                if (p == std::wstring::npos) continue;
                std::wstring proj = line.substr(p + 13, line.find(L'"', p + 13) - p - 13);
                std::replace(proj.begin(), proj.end(), L'/', L'\\');
                if (Lower(proj).find(L"\\temp\\") != std::wstring::npos) continue;
                add(ParentDir(proj));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    add(tools::DdcDir());
    add(la);
    std::wstring out = L"Папки:\r\n";
    for (const std::wstring& d : dirs) out += L"  " + d + L"\r\n";
    out +=
        L"\r\nПроцессы:\r\n  UnrealEditor.exe\r\n  ShaderCompileWorker.exe\r\n  UnrealBuildTool.exe\r\n"
        L"  UbaHost.exe\r\n  cl.exe\r\n  link.exe\r\n  dotnet.exe\r\n";
    return out;
}

}  // namespace gboost
