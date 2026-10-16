// Тяжёлые программы по памяти (процессы одной программы складываются), мягкое завершение и корзина.
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

// Системное и то, без чего Windows или защита не работают: не показываем и не завершаем.
bool Protected(const std::wstring& exe) {
    static const wchar_t* kSkip[] = {L"system",
                                     L"registry",
                                     L"smss.exe",
                                     L"csrss.exe",
                                     L"wininit.exe",
                                     L"winlogon.exe",
                                     L"services.exe",
                                     L"lsass.exe",
                                     L"svchost.exe",
                                     L"dwm.exe",
                                     L"fontdrvhost.exe",
                                     L"memory compression",
                                     L"msmpeng.exe",
                                     L"avp.exe",
                                     L"avpui.exe",
                                     L"secure system",
                                     L"radialmenu.exe",
                                     L"sihost.exe",
                                     L"ctfmon.exe",
                                     L"explorer.exe",
                                     L"searchhost.exe",
                                     L"startmenuexperiencehost.exe",
                                     L"audiodg.exe",
                                     L"lsaiso.exe",
                                     L"vmmem",
                                     L"vmmemwsl",
                                     L"textinputhost.exe",
                                     L"shellexperiencehost.exe",
                                     L"runtimebroker.exe",
                                     L"conhost.exe"};
    std::wstring n = Lower(exe);
    for (const wchar_t* s : kSkip)
        if (n == s) return true;
    return false;
}

// PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 1809+): в заголовках MinGW её нет
struct MemEx2 {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
        QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage, PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};

// Личная память процесса, как столбец «Память» в Диспетчере задач: общие библиотеки не считаются
// по разу на каждый процесс (у браузера их десятки, сумма рабочих наборов врёт в разы).
ULONGLONG PrivateBytes(HANDLE h) {
    MemEx2 m{sizeof(m)};
    if (K32GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m)) && m.PrivateWorkingSetSize)
        return m.PrivateWorkingSetSize;
    PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
    return K32GetProcessMemoryInfo(h, &pmc, sizeof(pmc)) ? pmc.WorkingSetSize : 0;
}

std::wstring ExeName(HANDLE h) {
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameW(h, 0, buf, &n)) return L"";
    std::wstring p(buf, n);
    return p.substr(p.find_last_of(L'\\') + 1);
}

struct CloseCtx {
    const std::vector<DWORD>* pids;
    int posted = 0;
};

BOOL CALLBACK CloseTop(HWND w, LPARAM lp) {
    auto* c = (CloseCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (std::find(c->pids->begin(), c->pids->end(), pid) == c->pids->end()) return TRUE;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;  // только главные окна
    PostMessageW(w, WM_CLOSE, 0, 0);
    ++c->posted;
    return TRUE;
}

}  // namespace

bool KillPid(DWORD pid, const std::wstring& expect) {
    if (pid <= 4 || pid == GetCurrentProcessId()) return false;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    // номер мог уже достаться другому процессу, пока кольцо было открыто
    bool same = expect.empty() || expect == L"?" || _wcsicmp(ExeName(h).c_str(), expect.c_str()) == 0;
    bool ok = same && !Protected(ExeName(h)) && TerminateProcess(h, 1);
    if (ok) WaitForSingleObject(h, 1500);  // порт и файлы освобождаются, когда процесс действительно ушёл
    CloseHandle(h);
    return ok;
}

std::vector<HeavyApp> HeavyApps(int count) {
    std::map<std::wstring, HeavyApp> byName;
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID <= 4 || Protected(pe.szExeFile)) continue;
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

AppClose CloseApp(const HeavyApp& app, int waitMs, const Log& log) {
    AppClose r;
    if (Protected(app.name)) return r;
    // сначала по-хорошему: главным окнам WM_CLOSE (браузер сохранит вкладки, редактор спросит про файлы)
    std::vector<HANDLE> hs;
    std::vector<DWORD> alive;
    for (DWORD pid : app.pids) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;
        if (_wcsicmp(ExeName(h).c_str(), app.name.c_str()) != 0) {  // номер уже чужой
            CloseHandle(h);
            continue;
        }
        hs.push_back(h);
        alive.push_back(pid);
    }
    CloseCtx ctx{&alive};
    EnumWindows(CloseTop, (LPARAM)&ctx);
    if (ctx.posted && log) log(L"Прошу " + app.name + L" закрыться...");
    ULONGLONG until = GetTickCount64() + (ctx.posted ? waitMs : 0);
    for (HANDLE h : hs) {
        ULONGLONG now = GetTickCount64();
        DWORD wait = now < until ? (DWORD)(until - now) : 0;
        if (WaitForSingleObject(h, wait) == WAIT_OBJECT_0)
            ++r.closed;
        else if (TerminateProcess(h, 1) && WaitForSingleObject(h, 1500) == WAIT_OBJECT_0)
            ++r.killed;
        else
            ++r.failed;
        CloseHandle(h);
    }
    return r;
}

ULONGLONG RecycleBinBytes(ULONGLONG* items) {
    SHQUERYRBINFO rb{sizeof(rb)};
    if (FAILED(SHQueryRecycleBinW(nullptr, &rb))) return 0;
    if (items) *items = (ULONGLONG)rb.i64NumItems;
    return (ULONGLONG)rb.i64Size;
}

bool EmptyRecycleBin() {
    return SUCCEEDED(SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND));
}

}  // namespace tools
