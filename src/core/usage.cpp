// Чем пользуются: счёт в памяти, файл %APPDATA%\MoonDesk\usage.tsv.
#include <windows.h>

#include "core/text.h"
#include "core/usage.h"

namespace usage {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
Data g_data;
bool g_read = false, g_dirty = false;

std::wstring File() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", p, MAX_PATH);
    CreateDirectoryW(p, nullptr);
    return std::wstring(p) + L"\\usage.tsv";
}

void Read() {  // под g_lock
    if (g_read) return;
    g_read = true;
    HANDLE f = CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::string raw(64 * 1024, '\0');
        DWORD got = 0;
        ReadFile(f, raw.data(), (DWORD)raw.size(), &got, nullptr);
        CloseHandle(f);
        raw.resize(got);
        g_data = Parse(Widen(raw));
    }
    if (!g_data.since) {  // первый запуск со счётом: с сегодняшнего дня
        g_data.since = Today();
        g_dirty = true;
    }
}

}  // namespace

int Today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // FILETIME - от 1601 года; 134774 дня до 1970-01-01
    return (int)(t / (24ull * 3600 * 10000000)) - 134774;
}

void Hit(const wchar_t* key) {
    int today = Today();
    AcquireSRWLockExclusive(&g_lock);
    Read();
    Rec& r = g_data.rec[key];
    r.last = today;
    ++r.count;
    g_dirty = true;
    ReleaseSRWLockExclusive(&g_lock);
}

void Flush() {
    AcquireSRWLockExclusive(&g_lock);
    Read();
    if (!g_dirty) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_dirty = false;
    std::string s = Narrow(Text(g_data));
    ReleaseSRWLockExclusive(&g_lock);
    HANDLE f = CreateFileW(File().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &put, nullptr);
    CloseHandle(f);
}

Data Snapshot() {
    AcquireSRWLockExclusive(&g_lock);
    Read();
    Data d = g_data;
    ReleaseSRWLockExclusive(&g_lock);
    return d;
}

}  // namespace usage
