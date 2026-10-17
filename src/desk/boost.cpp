#include "desk/boost.h"

#include <psapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <set>

#include "core/elevate.h"
#include "core/files.h"
#include "core/text.h"

namespace boost {

namespace {

std::wstring ReadText(const std::wstring& file) {
    HANDLE f =
        CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(GetFileSize(f, nullptr), '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) s.erase(0, 3);
    return Widen(s);
}

// Маленький разбор JSON ровно под boost.config.json: значение по первому ключу с таким именем.
size_t ValueAt(const std::wstring& t, const wchar_t* key) {
    std::wstring k = L"\"" + std::wstring(key) + L"\"";
    size_t p = t.find(k);
    if (p == std::wstring::npos) return p;
    p = t.find(L':', p + k.size());
    if (p == std::wstring::npos) return p;
    ++p;
    while (p < t.size() && iswspace(t[p])) ++p;
    return p;
}

bool GetBool(const std::wstring& t, const wchar_t* key, bool def) {
    size_t p = ValueAt(t, key);
    if (p == std::wstring::npos) return def;
    return t.compare(p, 4, L"true") == 0 ? true : t.compare(p, 5, L"false") == 0 ? false : def;
}

int GetInt(const std::wstring& t, const wchar_t* key, int def) {
    size_t p = ValueAt(t, key);
    return p == std::wstring::npos ? def : _wtoi(t.c_str() + p);
}

std::wstring ParseString(const std::wstring& t, size_t& p) {
    std::wstring out;
    for (++p; p < t.size() && t[p] != L'"'; ++p) {
        if (t[p] == L'\\' && p + 1 < t.size()) ++p;
        out += t[p];
    }
    ++p;
    return out;
}

std::vector<std::wstring> GetStrings(const std::wstring& t, const wchar_t* key) {
    std::vector<std::wstring> out;
    size_t p = ValueAt(t, key);
    if (p == std::wstring::npos || t[p] != L'[') return out;
    for (++p; p < t.size() && t[p] != L']';) {
        if (t[p] == L'"')
            out.push_back(ParseString(t, p));
        else
            ++p;
    }
    return out;
}

std::wstring JsonQuote(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'"') o += L'\\';
        o += c;
    }
    return o + L"\"";
}

// Имя совпадает точно или по маске с * и ? (без учёта регистра), как Test-NameInList в Boost.
bool InList(const std::wstring& name, const std::vector<std::wstring>& list) {
    for (const std::wstring& m : list)
        if (!_wcsicmp(name.c_str(), m.c_str()) ||
            ((m.find(L'*') != std::wstring::npos || m.find(L'?') != std::wstring::npos) &&
             PathMatchSpecW(name.c_str(), m.c_str())))
            return true;
    return false;
}

std::wstring StripExe(const std::wstring& n) { return EndsWithI(n, L".exe") ? n.substr(0, n.size() - 4) : n; }

std::wstring ProcessPath(HANDLE h) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    return QueryFullProcessImageNameW(h, 0, buf, &n) ? std::wstring(buf, n) : L"";
}

// Описание программы из её файла (FileDescription), как в Диспетчере задач.
std::wstring Description(const std::wstring& path) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Lang {
        WORD lang, cp;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &len) || len < sizeof(Lang))
        return {};
    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t* val = nullptr;
    if (!VerQueryValueW(data.data(), key, (void**)&val, &len) || !val || !len) return {};
    return Trim(val);
}

bool EnablePrivilege(const wchar_t* name) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{1};
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = ok && AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

// во время игры: таймер и приоритеты, которые потом вернуть
bool g_timer = false;
// номер процесса -> прежний приоритет и время создания: номер может достаться новому процессу, пока идёт игра
std::map<DWORD, std::pair<DWORD, ULONGLONG>> g_priorities;

ULONGLONG Created(HANDLE h) {
    FILETIME c{}, e{}, k{}, u{};
    return GetProcessTimes(h, &c, &e, &k, &u) ? ((ULONGLONG)c.dwHighDateTime << 32 | c.dwLowDateTime) : 0;
}

}  // namespace

std::wstring Dir(const std::wstring& configured) {
    if (!configured.empty()) return ExpandPath(configured);
    std::wstring side = ParentDir(ExeDir()) + L"\\Boost";  // near - макрос windows.h
    return Exists(side + L"\\boost.config.json") ? side : L"";
}

Settings Load(const std::wstring& dir) {
    Settings s;
    std::wstring t = dir.empty() ? L"" : ReadText(dir + L"\\boost.config.json");
    if (t.empty()) return s;
    s.whitelist = GetStrings(t, L"whitelist");
    s.guarded = GetStrings(t, L"protected");
    s.deprioritize = GetStrings(t, L"deprioritize");
    s.areas = GetStrings(t, L"memoryAreas");
    s.gracefulMs = std::clamp(GetInt(t, L"gracefulTimeoutMs", 3000), 500, 15000);
    s.minFreePercent = GetInt(t, L"minFreePercent", 0);
    s.purge = GetBool(t, L"purgeStandby", false);
    s.trim = GetBool(t, L"trimWorkingSet", false);
    s.timer = GetBool(t, L"timerResolution", false);
    s.gameDvr = GetBool(t, L"disableGameDvr", false);
    s.ok = !s.whitelist.empty();
    return s;
}

std::vector<Group> Scan(const Settings& s) {
    std::vector<Group> out;
    if (!s.ok) return out;
    std::map<std::wstring, Group> by;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    std::map<DWORD, DWORD> parent;
    std::map<DWORD, std::wstring> names;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        parent[pe.th32ProcessID] = pe.th32ParentProcessID;
        names[pe.th32ProcessID] = StripExe(pe.szExeFile);
    }
    CloseHandle(snap);
    DWORD self = GetCurrentProcessId();
    for (auto& [pid, name] : names) {
        if (pid == self || pid <= 4 || !InList(name, s.whitelist) || InList(name, s.guarded)) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;
        PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
        double mb = GetProcessMemoryInfo(h, &pmc, sizeof(pmc)) ? pmc.WorkingSetSize / 1048576.0 : 0;
        Group& g = by[Lower(name)];
        if (g.name.empty()) g.name = name;
        ++g.count;
        g.mb += mb;
        g.pids.push_back(pid);
        // вернуть потом стоит главный процесс (его родитель - не он же), и не апдейтер или webview
        auto pn = names.find(parent[pid]);
        bool root = pn == names.end() || _wcsicmp(pn->second.c_str(), name.c_str()) != 0;
        if (root && g.path.empty()) g.path = ProcessPath(h);
        if (g.title.empty()) {
            std::wstring any = g.path.empty() ? ProcessPath(h) : g.path;
            g.title = any.empty() ? L"" : Description(any);
            if (g.title.empty() || g.title.size() > 40) g.title = name;
        }
        CloseHandle(h);
    }
    for (auto& [k, g] : by) {
        std::wstring low = Lower(g.name);
        g.reopen = !g.path.empty() && low.find(L"update") == std::wstring::npos &&
                   low.find(L"webview") == std::wstring::npos && low.find(L"service") == std::wstring::npos &&
                   low.find(L"crash") == std::wstring::npos;
        out.push_back(std::move(g));
    }
    std::sort(out.begin(), out.end(), [](const Group& a, const Group& b) { return a.mb > b.mb; });
    return out;
}

double TotalMb(const std::vector<Group>& g) {
    double t = 0;
    for (const Group& x : g) t += x.mb;
    return t;
}

double AvailMb(int* usedPercent) {
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    if (usedPercent) *usedPercent = (int)ms.dwMemoryLoad;
    return ms.ullAvailPhys / 1048576.0;
}

Result Free(const Settings& s, const std::wstring& dir, const std::vector<Group>& groups) {
    Result r;
    double before = AvailMb();
    std::set<DWORD> pids;
    for (const Group& g : groups) {
        pids.insert(g.pids.begin(), g.pids.end());
        if (g.reopen) r.reopen.push_back(g.path);
    }
    // мягко: всем окнам этих процессов WM_CLOSE, как крестик
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (reinterpret_cast<std::set<DWORD>*>(lp)->count(pid)) PostMessageW(w, WM_CLOSE, 0, 0);
            return TRUE;
        },
        (LPARAM)&pids);
    ULONGLONG until = GetTickCount64() + s.gracefulMs;
    std::vector<HANDLE> hs;
    for (DWORD pid : pids)
        if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)) hs.push_back(h);
    for (HANDLE h : hs) {
        ULONGLONG now = GetTickCount64();
        WaitForSingleObject(h, now < until ? (DWORD)(until - now) : 0);
    }
    // кто не закрылся сам (свернулся в трей) - добить
    for (HANDLE h : hs) {
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) TerminateProcess(h, 1);
        WaitForSingleObject(h, 1000);
        CloseHandle(h);
    }
    r.closed = (int)groups.size();  // программ, а не процессов: у браузера их десятки
    // сжать рабочие наборы своих процессов, если так в конфиге
    if (s.trim) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (HANDLE h =
                    OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                SetProcessWorkingSetSize(h, (SIZE_T)-1, (SIZE_T)-1);
                CloseHandle(h);
            }
        CloseHandle(snap);
    }
    // сессия в формате Boost: GameBoost -Restore тоже сможет вернуть
    if (!dir.empty() && !r.reopen.empty()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t at[32];
        swprintf(at, 32, L"%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
        std::wstring j = L"{\r\n    \"closedAt\":  \"" + std::wstring(at) + L"\",\r\n    \"paths\":  [\r\n";
        for (size_t i = 0; i < r.reopen.size(); ++i)
            j += L"                  " + JsonQuote(r.reopen[i]) + (i + 1 < r.reopen.size() ? L",\r\n" : L"\r\n");
        j += L"              ],\r\n    \"priorities\":  [\r\n\r\n                   ]\r\n}";
        std::string bytes = "\xEF\xBB\xBF" + Narrow(j);
        HANDLE f =
            CreateFileW((dir + L"\\boost.session.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
            CloseHandle(f);
        }
    }
    if (s.purge) RunPurge(s);
    Sleep(600);  // память освобождается не мгновенно
    r.freedMb = std::max(0.0, AvailMb() - before);
    return r;
}

void ClearSession(const std::wstring& dir) {
    if (!dir.empty()) DeleteFileW((dir + L"\\boost.session.json").c_str());
}

void Reopen(const std::vector<std::wstring>& paths) {
    for (const std::wstring& p : paths)
        if (Exists(p)) ShellExecuteW(nullptr, nullptr, p.c_str(), nullptr, ParentDir(p).c_str(), SW_SHOWMINNOACTIVE);
}

void GameTweaks(const Settings& s, bool on) {
    using SetRes = LONG(WINAPI*)(ULONG, BOOLEAN, PULONG);
    static SetRes setRes = (SetRes)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution");
    ULONG cur = 0;
    if (on) {
        if (s.timer && setRes && !g_timer) g_timer = setRes(5000, TRUE, &cur) >= 0;  // 0.5 мс
        if (s.gameDvr) {
            DWORD zero = 0;
            RegSetKeyValueW(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled", REG_DWORD, &zero, 4);
            RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
                            L"AppCaptureEnabled", REG_DWORD, &zero, 4);
        }
        if (!s.deprioritize.empty()) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W pe{sizeof(pe)};
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                if (!InList(StripExe(pe.szExeFile), s.deprioritize)) continue;
                if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                           pe.th32ProcessID)) {
                    DWORD old = GetPriorityClass(h);
                    if (old && SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS))
                        g_priorities[pe.th32ProcessID] = {old, Created(h)};
                    CloseHandle(h);
                }
            }
            CloseHandle(snap);
        }
    } else {
        if (g_timer && setRes) setRes(5000, FALSE, &cur);
        g_timer = false;
        for (auto& [pid, was] : g_priorities)
            if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                if (Created(h) == was.second) SetPriorityClass(h, was.first);  // тот же процесс, а не тёзка по номеру
                CloseHandle(h);
            }
        g_priorities.clear();
    }
}

// Задача с наивысшими правами (core/elevate): запускает помощника из Program Files, а не exe из папки пользователя.
// Готова - только если помощник свежий: после пересборки задача ждёт «Пересоздать», а не запускает старую копию.
bool PurgeTaskExists() { return !elevate::Blocked(elevate::TASK_PURGE) && elevate::TaskReady(elevate::TASK_PURGE); }

bool SetupPurgeTask() { return elevate::Setup(elevate::TASK_PURGE); }

bool RunPurge(const Settings& s) { return RunPurgeAreas(s.areas); }

bool RunPurgeAreas(const std::vector<std::wstring>& list) {
    if (!PurgeTaskExists()) return false;  // сама по себе (после игры) не спрашивает UAC - пропускает
    std::wstring areas;
    for (const std::wstring& a : list) areas += (areas.empty() ? L"" : L",") + a;
    elevate::Put(L"PurgeAreas", areas);  // помощник не читает настроек из папки пользователя
    if (elevate::Run(elevate::TASK_PURGE)) return true;
    // антивирус не пускает задачу: после игры UAC не спрашиваем, а в студии карточка снова предложит «Настроить»
    elevate::Take(L"PurgeAreas");
    elevate::SetBlocked(elevate::TASK_PURGE, true);
    return false;
}

int PurgeMain(const Settings& s) {
    // NtSetSystemInformation(SystemMemoryListInformation) - то же, что делает Boost (Clear-Memory)
    using SetInfo = LONG(WINAPI*)(int, PVOID, ULONG);
    auto set = (SetInfo)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetSystemInformation");
    if (!set) return 1;
    EnablePrivilege(L"SeProfileSingleProcessPrivilege");
    EnablePrivilege(L"SeIncreaseQuotaPrivilege");
    std::vector<std::wstring> areas = s.areas.empty() ? std::vector<std::wstring>{L"StandbyList"} : s.areas;
    int fails = 0;
    for (const std::wstring& a : areas) {
        int cmd = !_wcsicmp(a.c_str(), L"WorkingSets")              ? 2
                  : !_wcsicmp(a.c_str(), L"ModifiedPageList")       ? 3
                  : !_wcsicmp(a.c_str(), L"StandbyList")            ? 4
                  : !_wcsicmp(a.c_str(), L"StandbyListLowPriority") ? 5
                                                                    : 0;
        if (cmd) {
            if (set(80, &cmd, sizeof(cmd)) < 0) ++fails;  // 80 - SystemMemoryListInformation
        } else if (!_wcsicmp(a.c_str(), L"SystemFileCache")) {
            if (!SetSystemFileCacheSize((SIZE_T)-1, (SIZE_T)-1, 0)) ++fails;
        }
    }
    return fails ? 2 : 0;
}

}  // namespace boost
