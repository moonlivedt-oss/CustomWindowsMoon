// Время в режимах: запись в modes.tsv и итог за неделю с идущими сейчас.
#include "app/modestats.h"

#include "core/text.h"

namespace modestats {

namespace {

LONGLONG g_since[COUNT]{};      // местные секунды включения; 0 - режим не идёт
SRWLOCK g_lock = SRWLOCK_INIT;  // студия читает из своего потока

std::wstring File() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", ad, MAX_PATH);
    CreateDirectoryW(ad, nullptr);
    return std::wstring(ad) + L"\\modes.tsv";
}

std::string ReadFile(const std::wstring& path) {
    std::string out;
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!::ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr)) got = 0;
        out.resize(got);
    }
    CloseHandle(f);
    return out;
}

}  // namespace

LONGLONG LocalNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (LONGLONG)(t / 10000000ull) - 11644473600ll;
}

void Begin(int mode) {
    if (mode < 0 || mode >= COUNT) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_since[mode]) g_since[mode] = LocalNow();
    ReleaseSRWLockExclusive(&g_lock);
}

void End(int mode) {
    if (mode < 0 || mode >= COUNT) return;
    AcquireSRWLockExclusive(&g_lock);
    LONGLONG from = g_since[mode];
    g_since[mode] = 0;
    ReleaseSRWLockExclusive(&g_lock);
    LONGLONG to = LocalNow();
    if (!from || to - from < 30) return;  // дёрнулся на миг - не считаем
    std::string add;
    for (const Row& r : Split(from, to, mode)) add += Narrow(RowLine(r) + L"\n");
    HANDLE f = CreateFileW(File().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, add.data(), (DWORD)add.size(), &put, nullptr);
    CloseHandle(f);
}

Week Now() {
    std::vector<Row> rows;
    LONGLONG now = LocalNow();
    int today = DayOf(now);
    for (const std::wstring& l : ::Split(Widen(ReadFile(File())), L'\n')) {
        Row r;
        if (ParseRow(l, r) && r.day > today - 7) rows.push_back(r);
    }
    AcquireSRWLockShared(&g_lock);
    for (int m = 0; m < COUNT; ++m)
        if (g_since[m])
            for (const Row& r : Split(g_since[m], now, m)) rows.push_back(r);
    ReleaseSRWLockShared(&g_lock);
    return Sum(rows, today);
}

Week Demo() {
    Week w;
    w.firstDay = DayOf(LocalNow()) - 6;
    const int h[7][COUNT] = {{120, 300, 50, 0}, {0, 420, 75, 0}, {200, 180, 0, 30}, {60, 360, 100, 0},
                             {0, 240, 50, 0},   {340, 0, 0, 60}, {280, 90, 25, 0}};
    for (int d = 0; d < 7; ++d)
        for (int m = 0; m < COUNT; ++m) {
            w.sec[d][m] = h[d][m] * 60ll;
            w.total[m] += w.sec[d][m];
        }
    return w;
}

}  // namespace modestats
