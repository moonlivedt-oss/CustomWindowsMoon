#include "core/procsnap.h"

namespace procsnap {

namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
std::shared_ptr<const List> g_list;
ULONGLONG g_at = 0;
}  // namespace

std::shared_ptr<const List> Get(DWORD maxAgeMs) {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_lock);
    std::shared_ptr<const List> cur = g_list;
    bool fresh = cur && now - g_at <= maxAgeMs;
    ReleaseSRWLockShared(&g_lock);
    if (fresh) return cur;
    auto list = std::make_shared<List>();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) list->push_back(pe);
        CloseHandle(snap);
    }
    AcquireSRWLockExclusive(&g_lock);
    g_list = list;
    g_at = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
    return list;
}

}  // namespace procsnap
