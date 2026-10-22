#include "core/procsnap.h"

#include <algorithm>
#include <atomic>

namespace procsnap {

namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
std::shared_ptr<const List> g_list;
ULONGLONG g_at = 0;

// SYSTEM_PROCESS_INFORMATION целиком (в winternl.h нужные поля спрятаны в Reserved): раскладка не меняется с Vista
struct UnicodeStr {
    USHORT len, max;
    PWSTR buf;
};
struct SysProc {
    ULONG next, threads;
    LARGE_INTEGER wsPrivate;
    ULONG hardFaults, threadsPeak;
    ULONGLONG cycles;
    LARGE_INTEGER create, user, kernel;
    UnicodeStr name;
    LONG basePriority;
    HANDLE pid, parent;
    ULONG handles, session;
    ULONG_PTR key;
    SIZE_T vmPeak, vm;
    ULONG faults;
    SIZE_T wsPeak, ws, pagedPeak, paged, nonPagedPeak, nonPaged, pagefile, pagefilePeak, privatePages;
};
using QueryFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);

// Все процессы одним вызовом ядра. CreateToolhelp32Snapshot делает то же самое и потом ещё копирует всё в общую
// память: на основном потоке игровой режим платил за него ~12 мс каждые 2 с, здесь - в несколько раз меньше.
template <class Fn>
bool ForEach(Fn fn) {
    static QueryFn query = (QueryFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!query) return false;
    // буфер - на вызов (память программы в покое - пара МБ), размер - по прошлому разу
    static std::atomic<size_t> hint{256 * 1024};
    std::vector<BYTE> buf(hint);
    ULONG need = 0;
    LONG st;
    // список растёт между вызовами: STATUS_INFO_LENGTH_MISMATCH - взять больше с запасом
    while ((st = query(5 /* SystemProcessInformation */, buf.data(), (ULONG)buf.size(), &need)) == (LONG)0xC0000004L)
        buf.resize(std::max<size_t>(need, buf.size()) + 64 * 1024);
    if (st < 0) return false;
    hint = buf.size();
    for (size_t at = 0;;) {
        const SysProc& p = *(const SysProc*)(buf.data() + at);
        fn(p);
        if (!p.next) break;
        at += p.next;
    }
    return true;
}

}  // namespace

std::shared_ptr<const List> Get(DWORD maxAgeMs) {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_lock);
    std::shared_ptr<const List> cur = g_list;
    bool fresh = cur && now - g_at <= maxAgeMs;
    ReleaseSRWLockShared(&g_lock);
    if (fresh) return cur;
    auto list = std::make_shared<List>();
    bool ok = ForEach([&](const SysProc& p) {
        PROCESSENTRY32W pe{sizeof(pe)};
        pe.th32ProcessID = (DWORD)(ULONG_PTR)p.pid;
        pe.th32ParentProcessID = (DWORD)(ULONG_PTR)p.parent;
        pe.cntThreads = p.threads;
        pe.pcPriClassBase = p.basePriority;
        size_t n = std::min<size_t>(p.name.buf ? p.name.len / sizeof(wchar_t) : 0, MAX_PATH - 1);
        if (n) wmemcpy(pe.szExeFile, p.name.buf, n);
        pe.szExeFile[n] = 0;
        list->push_back(pe);
    });
    if (!ok) {  // запасной путь
        list->clear();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{sizeof(pe)};
            for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) list->push_back(pe);
            CloseHandle(snap);
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_list = list;
    g_at = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
    return list;
}

std::vector<Info> Session() {
    std::vector<Info> out;
    DWORD mine = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mine);
    ForEach([&](const SysProc& p) {
        DWORD pid = (DWORD)(ULONG_PTR)p.pid;
        if (pid <= 4 || p.session != mine) return;
        Info i;
        i.pid = pid;
        i.parent = (DWORD)(ULONG_PTR)p.parent;
        if (p.name.buf) i.name.assign(p.name.buf, p.name.len / sizeof(wchar_t));
        i.created = (ULONGLONG)p.create.QuadPart;
        i.cpu = (ULONGLONG)p.user.QuadPart + (ULONGLONG)p.kernel.QuadPart;
        i.priv = p.privatePages;
        out.push_back(std::move(i));
    });
    return out;
}

}  // namespace procsnap
