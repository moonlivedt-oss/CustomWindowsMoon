// Тяжёлые программы по памяти (процессы одной программы складываются), память, размер корзины.
// Завершение и очистка корзины - в RadialTools.exe (src/act).
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

// PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 1809+): в заголовках MinGW её нет
struct MemEx2 {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
        QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage, PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};

}  // namespace

// Личная память процесса, как столбец «Память» в Диспетчере задач: общие библиотеки не считаются
// по разу на каждый процесс (у браузера их десятки, сумма рабочих наборов врёт в разы).
ULONGLONG PrivateBytes(HANDLE h) {
    MemEx2 m{sizeof(m)};
    if (K32GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m)) && m.PrivateWorkingSetSize)
        return m.PrivateWorkingSetSize;
    PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
    return K32GetProcessMemoryInfo(h, &pmc, sizeof(pmc)) ? pmc.WorkingSetSize : 0;
}

std::vector<HeavyApp> HeavyApps(int count) {
    std::map<std::wstring, HeavyApp> byName;
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID <= 4 || ProtectedExe(pe.szExeFile)) continue;
        // только программы этого входа: службы и чужие сеансы без прав всё равно не завершить
        DWORD s = 0;
        if (!ProcessIdToSessionId(pe.th32ProcessID, &s) || s != mySession) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        ULONGLONG b = PrivateBytes(h);
        CloseHandle(h);
        if (!b) continue;
        HeavyApp& a = byName[Lower(pe.szExeFile)];
        a.name = pe.szExeFile;
        a.pids.push_back(pe.th32ProcessID);
        a.bytes += b;
    }
    CloseHandle(snap);
    std::vector<HeavyApp> out;
    for (auto& [k, a] : byName) out.push_back(std::move(a));
    std::sort(out.begin(), out.end(), [](const HeavyApp& a, const HeavyApp& b) { return a.bytes > b.bytes; });
    if ((int)out.size() > count) out.resize(count);
    return out;
}

ULONGLONG RecycleBinBytes(ULONGLONG* items) {
    SHQUERYRBINFO rb{sizeof(rb)};
    if (FAILED(SHQueryRecycleBinW(nullptr, &rb))) return 0;
    if (items) *items = (ULONGLONG)rb.i64NumItems;
    return (ULONGLONG)rb.i64Size;
}

MemoryState Memory() {
    MEMORYSTATUSEX m{sizeof(m)};
    MemoryState r;
    if (GlobalMemoryStatusEx(&m))
        r.total = m.ullTotalPhys, r.avail = m.ullAvailPhys, r.loadPercent = (int)m.dwMemoryLoad;
    return r;
}

// Какой exe запускает строка автозагрузки: путь в кавычках или до «.exe»; ярлык - по его цели;
// «Update.exe --processStart Discord.exe» (Squirrel) - на деле работает Discord.exe.
std::wstring StartExe(const StartItem& it) {
    std::wstring c = Trim(it.command);
    wchar_t buf[4096];
    if (ExpandEnvironmentStringsW(c.c_str(), buf, 4096)) c = buf;
    std::wstring l = Lower(c);
    size_t ps = l.find(L"--processstart");
    if (ps != std::wstring::npos) {
        std::wstring rest = Trim(c.substr(ps + 14));
        if (!rest.empty() && rest[0] == L'"') rest = rest.substr(1, rest.find(L'"', 1) - 1);
        return rest.substr(0, rest.find(L' '));
    }
    if (EndsWithI(c, L".lnk")) {
        IShellLinkW* sl = nullptr;
        std::wstring target;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
                wchar_t t[MAX_PATH];
                if (SUCCEEDED(pf->Load(c.c_str(), STGM_READ)) && SUCCEEDED(sl->GetPath(t, MAX_PATH, nullptr, 0)))
                    target = t;
                pf->Release();
            }
            sl->Release();
        }
        return target;
    }
    if (!c.empty() && c[0] == L'"') return c.substr(1, c.find(L'"', 1) - 1);
    size_t e = l.find(L".exe");
    return e == std::wstring::npos ? c : c.substr(0, e + 4);
}

void StartupWeight(std::vector<StartItem>& items) {
    // запущенные программы этого входа: полный путь и имя -> личная память
    std::map<std::wstring, ULONGLONG> byPath, byName;
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        DWORD s = 0;
        if (pe.th32ProcessID <= 4 || !ProcessIdToSessionId(pe.th32ProcessID, &s) || s != mySession) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        ULONGLONG b = PrivateBytes(h);
        wchar_t p[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, p, &n)) byPath[Lower(std::wstring(p, n))] += b;
        byName[Lower(pe.szExeFile)] += b;
        CloseHandle(h);
    }
    CloseHandle(snap);
    for (StartItem& it : items) {
        std::wstring exe = Lower(StartExe(it));
        if (exe.empty()) continue;
        auto p = byPath.find(exe);
        if (p != byPath.end()) {
            it.running = true, it.memory = p->second;
            continue;
        }
        auto n = byName.find(exe.substr(exe.find_last_of(L'\\') + 1));
        if (n != byName.end()) it.running = true, it.memory = n->second;
    }
}

}  // namespace tools
