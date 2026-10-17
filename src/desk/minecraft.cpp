#include "desk/minecraft.h"

#include <tlhelp32.h>

#include <algorithm>

#include "core/files.h"
#include "core/text.h"

namespace mc {

namespace {

std::string ReadAll(const std::wstring& file, size_t max = 4u << 20) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s;
    char buf[16384];
    DWORD got = 0;
    while (s.size() < max && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) s.append(buf, got);
    CloseHandle(f);
    return s;
}

ULONGLONG WriteTime(const std::wstring& file) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
}

ULONGLONG NowWall() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

std::wstring Unquote(std::wstring v) {
    v = Trim(v);
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
    // Qt пишет в ini \" и \\ внутри кавычек
    std::wstring o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == L'\\' && i + 1 < v.size()) ++i;
        o += v[i];
    }
    return o;
}

// Ключи ini Prism (секция не важна: имена уникальны).
std::wstring IniGet(const std::wstring& text, const wchar_t* key) {
    std::wstring k = std::wstring(key) + L"=";
    for (const std::wstring& line : Split(text, L'\n'))
        if (line.rfind(k, 0) == 0) return Unquote(line.substr(k.size()));
    return L"";
}

int CountJars(const std::wstring& dir) {
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.jar").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do ++n;
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// Командная строка чужого процесса (того же пользователя): ProcessCommandLineInformation, права не нужны.
std::wstring CommandLine(DWORD pid) {
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto q = (Query)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!q) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    std::vector<BYTE> buf(32768);
    ULONG len = 0;
    LONG st = q(h, 60, buf.data(), (ULONG)buf.size(), &len);
    if (st < 0 && len > buf.size()) {
        buf.resize(len);
        st = q(h, 60, buf.data(), (ULONG)buf.size(), &len);
    }
    CloseHandle(h);
    if (st < 0) return L"";
    struct US {
        USHORT Length, MaximumLength;
        PWSTR Buffer;
    }* us = (US*)buf.data();
    return us->Buffer ? std::wstring(us->Buffer, us->Length / 2) : L"";
}

// -Xmx8096m / -Xmx8G -> МБ
int ParseXmx(const std::wstring& cl) {
    size_t p = cl.find(L"-Xmx");
    if (p == std::wstring::npos) return 0;
    double v = _wtof(cl.c_str() + p + 4);
    size_t u = p + 4;
    while (u < cl.size() && (iswdigit(cl[u]) || cl[u] == L'.')) ++u;
    wchar_t unit = u < cl.size() ? towlower(cl[u]) : L'm';
    return (int)(unit == L'g' ? v * 1024 : unit == L'k' ? v / 1024 : v);
}

// «...instances/super2/natives» -> super2
std::wstring InstanceFromCmd(const std::wstring& cl) {
    std::wstring low = Lower(cl);
    for (const wchar_t* key : {L"instances/", L"instances\\"}) {
        size_t p = low.find(key);
        if (p == std::wstring::npos) continue;
        p += wcslen(key);
        size_t e = cl.find_first_of(L"/\\", p);
        if (e != std::wstring::npos) return cl.substr(p, e - p);
    }
    return L"";
}

std::wstring FileName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// ---------------- слежка ----------------

SRWLOCK g_lock = SRWLOCK_INIT;
Stats g_stats;
HANDLE g_stop = nullptr, g_thread = nullptr;
DWORD g_pid = 0;
std::wstring g_jcmd, g_log;

// Число с единицей рядом со словом: «used 3456789K» или «3456789K used», в МБ; -1 - нет.
double NumNear(const std::string& s, size_t at, size_t wordLen) {
    auto parse = [&](size_t p) -> double {
        while (p < s.size() && s[p] == ' ') ++p;
        size_t b = p;
        while (p < s.size() && isdigit((unsigned char)s[p])) ++p;
        if (p == b || p >= s.size()) return -1;
        double v = atof(s.c_str() + b);
        char u = (char)tolower(s[p]);
        return u == 'k' ? v / 1024 : u == 'm' ? v : u == 'g' ? v * 1024 : u == 'b' ? v / 1048576 : -1;
    };
    double after = parse(at + wordLen);
    if (after >= 0) return after;
    // перед словом: «...3456789K used»
    if (at < 2) return -1;
    size_t e = at - 1;
    while (e > 0 && s[e] == ' ') --e;
    size_t b = e;
    while (b > 0 && isdigit((unsigned char)s[b - 1])) --b;
    return b < e ? parse(b) : -1;
}

// jcmd <pid> GC.heap_info: G1 «total 8290304K, used 3456789K», Shenandoah «8388608K committed, 3456789K used»,
// ZGC «used 1234M, capacity 8096M».
bool HeapInfo(double& used, double& committed) {
    if (g_jcmd.empty()) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + g_jcmd + L"\" " + std::to_wstring(g_pid) + L" GC.heap_info";
    bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    std::string out;
    if (ok) {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got && out.size() < 65536) out.append(buf, got);
        if (WaitForSingleObject(pi.hProcess, 8000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    used = committed = -1;
    size_t u = out.find("used");
    if (u != std::string::npos) used = NumNear(out, u, 4);
    for (const char* w : {"total", "committed", "capacity"}) {
        size_t c = out.find(w);
        if (c != std::string::npos && committed < 0) committed = NumNear(out, c, strlen(w));
    }
    return used >= 0;
}

DWORD WINAPI WatchLoop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    ULONGLONG offset = 0, heapAt = GetTickCount64() + 60000;  // первый замер - когда мир загрузится
    std::string carry;
    while (WaitForSingleObject(g_stop, 5000) == WAIT_TIMEOUT) {
        // лог: только то, что дописалось
        HANDLE f = CreateFileW(g_log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            GetFileSizeEx(f, &size);
            if ((ULONGLONG)size.QuadPart < offset) offset = 0, carry.clear();  // новый лог
            ULONGLONG want = std::min<ULONGLONG>(size.QuadPart - offset, 4u << 20);
            if (want) {
                LARGE_INTEGER pos;
                pos.QuadPart = (LONGLONG)((ULONGLONG)size.QuadPart - want);
                SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
                std::string chunk(want, '\0');
                DWORD got = 0;
                ReadFile(f, chunk.data(), (DWORD)want, &got, nullptr);
                chunk.resize(got);
                offset = size.QuadPart;
                chunk = carry + chunk;
                size_t last = chunk.rfind('\n');
                carry = last == std::string::npos ? chunk : chunk.substr(last + 1);
                if (last != std::string::npos) chunk.resize(last);
                int lags = 0;
                double ms = 0;
                bool oom = false;
                for (size_t p = 0; (p = chunk.find("Can't keep up!", p)) != std::string::npos; ++p) {
                    ++lags;
                    size_t r = chunk.find("Running ", p);
                    if (r != std::string::npos && r - p < 120) ms += atof(chunk.c_str() + r + 8);
                }
                if (chunk.find("java.lang.OutOfMemoryError") != std::string::npos) oom = true;
                AcquireSRWLockExclusive(&g_lock);
                g_stats.lags += lags;
                g_stats.lagMs += ms;
                g_stats.oom = g_stats.oom || oom;
                ReleaseSRWLockExclusive(&g_lock);
            }
            CloseHandle(f);
        }
        // куча: раз в 2 минуты, jcmd - это ещё одна Java на полсекунды
        if (GetTickCount64() >= heapAt) {
            heapAt = GetTickCount64() + 120000;
            double used = 0, committed = 0;
            if (HeapInfo(used, committed)) {
                AcquireSRWLockExclusive(&g_lock);
                g_stats.peakHeapMb = std::max(g_stats.peakHeapMb, used);
                if (committed > 0) g_stats.heapMb = committed;
                g_stats.heapSamples++;
                ReleaseSRWLockExclusive(&g_lock);
            }
        }
    }
    return 0;
}

}  // namespace

std::wstring PrismDir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\PrismLauncher", p, MAX_PATH);
    return p;
}

std::wstring PrismExe() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Programs\\PrismLauncher\\prismlauncher.exe", p, MAX_PATH);
    if (Exists(p)) return p;
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\PrismLauncher\\prismlauncher.exe", p, MAX_PATH);
    return Exists(p) ? p : L"";
}

std::vector<Instance> Instances() {
    std::vector<Instance> out;
    std::wstring root = PrismDir(), global = Widen(ReadAll(root + L"\\prismlauncher.cfg"));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\instances\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring dir = root + L"\\instances\\" + fd.cFileName;
        std::wstring cfg = Widen(ReadAll(dir + L"\\instance.cfg"));
        if (cfg.empty()) continue;
        Instance in;
        in.id = fd.cFileName;
        in.name = IniGet(cfg, L"name");
        if (in.name.empty()) in.name = in.id;
        in.game = IsFolder(dir + L"\\minecraft") ? dir + L"\\minecraft" : dir + L"\\.minecraft";
        bool ownMem = IniGet(cfg, L"OverrideMemory") == L"true";
        in.ownArgs = IniGet(cfg, L"OverrideJavaArgs") == L"true";
        in.maxMb = _wtoi(IniGet(ownMem ? cfg : global, L"MaxMemAlloc").c_str());
        std::wstring args = IniGet(in.ownArgs ? cfg : global, L"JvmArgs");
        in.preTouch = args.find(L"-XX:+AlwaysPreTouch") != std::wstring::npos;
        in.mods = CountJars(in.game + L"\\mods");
        in.lastLaunch = _wcstoui64(IniGet(cfg, L"lastLaunchTime").c_str(), nullptr, 10);
        std::wstring key = IniGet(cfg, L"iconKey");
        for (const wchar_t* ext : {L".png", L".gif", L".jpg"}) {
            std::wstring icon = root + L"\\icons\\" + key + ext;
            if (!key.empty() && Exists(icon)) {
                in.icon = icon;
                break;
            }
        }
        out.push_back(in);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const Instance& a, const Instance& b) { return a.lastLaunch > b.lastLaunch; });
    return out;
}

bool FindInstance(const std::wstring& id, Instance& out) {
    for (const Instance& in : Instances())
        if (!_wcsicmp(in.id.c_str(), id.c_str())) {
            out = in;
            return true;
        }
    return false;
}

std::vector<DWORD> PrismPids() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"prismlauncher.exe")) out.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

bool PrismRunning() { return !PrismPids().empty(); }

bool Launch(const std::wstring& id) {
    std::wstring exe = PrismExe();
    if (exe.empty()) return false;
    std::wstring args = L"-l \"" + id + L"\"";
    return (INT_PTR)ShellExecuteW(nullptr, nullptr, exe.c_str(), args.c_str(), ParentDir(exe).c_str(),
                                  SW_SHOWMINNOACTIVE) > 32;
}

bool IsMinecraft(DWORD pid, const std::wstring& path, std::wstring& id, int& xmxMb) {
    std::wstring exe = Lower(FileName(path));
    if (exe != L"javaw.exe" && exe != L"java.exe") return false;
    std::wstring cl = CommandLine(pid);
    if (cl.empty()) return false;
    std::wstring low = Lower(cl);
    id = InstanceFromCmd(cl);
    bool prism = !id.empty() && low.find(L"prismlauncher") != std::wstring::npos;
    bool vanilla = low.find(L"net.minecraft") != std::wstring::npos || low.find(L"--gamedir") != std::wstring::npos ||
                   low.find(L"minecraft") != std::wstring::npos;
    if (!prism && !vanilla) return false;
    if (!prism) id.clear();
    xmxMb = ParseXmx(cl);
    return true;
}

std::wstring GameName(DWORD pid, const std::wstring& path) {
    std::wstring id;
    int xmx = 0;
    if (!IsMinecraft(pid, path, id, xmx)) return L"";
    Instance in;
    if (!id.empty() && FindInstance(id, in)) return in.name;
    return L"Minecraft";
}

bool RemovePreTouch(const std::wstring& id, std::wstring& err) {
    if (PrismRunning()) {
        err = L"Prism открыт: он перезапишет настройки сборки при выходе. Закрой его и нажми ещё раз.";
        return false;
    }
    std::wstring root = PrismDir(), file = root + L"\\instances\\" + id + L"\\instance.cfg";
    std::string raw = ReadAll(file);
    if (raw.empty()) {
        err = L"Не нашёл instance.cfg сборки.";
        return false;
    }
    std::wstring cfg = Widen(raw);
    bool own = IniGet(cfg, L"OverrideJavaArgs") == L"true";
    std::wstring args = IniGet(own ? cfg : Widen(ReadAll(root + L"\\prismlauncher.cfg")), L"JvmArgs");
    size_t p = args.find(L"-XX:+AlwaysPreTouch");
    if (p == std::wstring::npos) {
        err = L"Флага уже нет.";
        return false;
    }
    args.erase(p, wcslen(L"-XX:+AlwaysPreTouch"));
    for (size_t d; (d = args.find(L"  ")) != std::wstring::npos;) args.erase(d, 1);
    args = Trim(args);
    // копия на всякий случай
    CopyFileW(file.c_str(), (file + L".moon-backup").c_str(), FALSE);
    // строки ini переписываем целиком: JvmArgs и (если аргументы были общие) OverrideJavaArgs
    std::wstring quoted = L"\"";
    for (wchar_t c : args) {
        if (c == L'"' || c == L'\\') quoted += L'\\';
        quoted += c;
    }
    quoted += L"\"";
    std::vector<std::wstring> lines = Split(cfg, L'\n');
    bool setArgs = false, setOverride = false;
    for (std::wstring& l : lines) {
        if (l.rfind(L"JvmArgs=", 0) == 0) l = L"JvmArgs=" + quoted, setArgs = true;
        if (l.rfind(L"OverrideJavaArgs=", 0) == 0) l = L"OverrideJavaArgs=true", setOverride = true;
    }
    std::wstring outText;
    bool general = false;
    for (const std::wstring& l : lines) {
        outText += l + L"\r\n";
        if (l == L"[General]" && !general) {
            general = true;
            if (!setArgs) outText += L"JvmArgs=" + quoted + L"\r\n";
            if (!setOverride) outText += L"OverrideJavaArgs=true\r\n";
        }
    }
    if (!general) {
        if (!setArgs) outText += L"JvmArgs=" + quoted + L"\r\n";
        if (!setOverride) outText += L"OverrideJavaArgs=true\r\n";
    }
    std::string bytes = Narrow(outText);
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = L"Не получилось записать instance.cfg.";
        return false;
    }
    DWORD put = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    CloseHandle(f);
    return put == bytes.size();
}

void StartWatch(DWORD pid, const std::wstring& javaExe, const Instance& inst) {
    if (g_thread) StopWatch();
    g_pid = pid;
    std::wstring jcmd = ParentDir(javaExe) + L"\\jcmd.exe";
    g_jcmd = Exists(jcmd) ? jcmd : L"";
    g_log = inst.game + L"\\logs\\latest.log";
    AcquireSRWLockExclusive(&g_lock);
    g_stats = Stats{};
    g_stats.id = inst.id;
    g_stats.name = inst.name;
    g_stats.xmxMb = inst.maxMb;
    g_stats.startWall = NowWall();
    ReleaseSRWLockExclusive(&g_lock);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, WatchLoop, nullptr, 0, nullptr);
}

Stats StopWatch() {
    if (g_thread) {
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, 10000);
        CloseHandle(g_thread);
        CloseHandle(g_stop);
        g_thread = g_stop = nullptr;
    }
    AcquireSRWLockShared(&g_lock);
    Stats s = g_stats;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

bool Watching() { return g_thread != nullptr; }

bool NewCrash(const std::wstring& gameDir, ULONGLONG sinceWall, Crash& out) {
    std::wstring best;
    ULONGLONG bestT = sinceWall;
    bool jvm = false;
    auto scan = [&](const std::wstring& pattern, const std::wstring& dir, bool isJvm) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            ULONGLONG t = (ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
            if (t > bestT) bestT = t, best = dir + L"\\" + fd.cFileName, jvm = isJvm;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    scan(L"crash-*.txt", gameDir + L"\\crash-reports", false);
    scan(L"hs_err_pid*.log", gameDir, true);
    if (best.empty()) return false;
    out = Crash{};
    out.file = best;
    out.jvm = jvm;
    std::wstring t = Widen(ReadAll(best, 256 * 1024));
    std::vector<std::wstring> lines = Split(t, L'\n');
    if (jvm) {
        // «# EXCEPTION_ACCESS_VIOLATION ...», «# Problematic frame: # C [nvoglv64.dll+0x...]»
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::wstring& l = lines[i];
            if (out.what.empty() &&
                (l.find(L"EXCEPTION_") != std::wstring::npos || l.find(L"Out of Memory") != std::wstring::npos ||
                 l.find(L"insufficient memory") != std::wstring::npos))
                out.what = Trim(l.substr(l.find_first_not_of(L"# ")));
            if (l.find(L"Problematic frame:") != std::wstring::npos && i + 1 < lines.size())
                out.mods = Trim(lines[i + 1].substr(std::min(lines[i + 1].size(), (size_t)1)));
        }
        if (out.what.empty()) out.what = L"Java завершилась с фатальной ошибкой";
    } else {
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::wstring& l = lines[i];
            if (out.what.empty() && l.rfind(L"Description: ", 0) == 0) {
                out.what = Trim(l.substr(13));
                // следующая непустая строка - сама ошибка
                for (size_t j = i + 1; j < lines.size() && j < i + 4; ++j)
                    if (!lines[j].empty()) {
                        std::wstring e = lines[j];
                        if (e.size() > 120) e = e.substr(0, 117) + L"...";
                        out.what += L": " + e;
                        break;
                    }
            }
            if (out.mods.empty() && l.find(L"Suspected Mod") != std::wstring::npos) {
                size_t c = l.find(L':');
                out.mods = c == std::wstring::npos ? L"" : Trim(l.substr(c + 1));
                // Forge перечисляет моды на следующих строках
                if (out.mods.empty() || out.mods == L"NONE")
                    for (size_t j = i + 1; j < lines.size() && j < i + 4; ++j)
                        if (lines[j].find(L"(") != std::wstring::npos) {
                            out.mods += (out.mods.empty() || out.mods == L"NONE" ? L"" : L", ") + Trim(lines[j]);
                            if (out.mods.rfind(L"NONE", 0) == 0) out.mods = Trim(lines[j]);
                        }
                if (out.mods == L"NONE") out.mods.clear();
            }
        }
        if (out.what.empty()) out.what = L"отчёт о падении без описания";
    }
    if (out.mods.size() > 140) out.mods = out.mods.substr(0, 137) + L"...";
    return true;
}

bool PagefileAuto() {
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                     L"PagingFiles", RRF_RT_REG_MULTI_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    std::wstring first = buf;
    // «?:\pagefile.sys» - всё решает система; «C:\pagefile.sys 0 0» - размер по выбору системы
    return first.rfind(L"?:", 0) == 0 || EndsWithI(Trim(first), L" 0 0");
}

}  // namespace mc
