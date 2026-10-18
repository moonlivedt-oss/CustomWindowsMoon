// Живой рост папок (Загрузки, TEMP): ReadDirectoryChangesW будит поток, когда в папке что-то меняется, а размер
// пересчитывается не чаще раза в минуту и только если были изменения. Спокойная папка не стоит ничего.
#include <windows.h>

#include <deque>
#include <memory>

#include "tools/tools.h"

namespace tools {

namespace {

constexpr ULONGLONG kMinute = 60 * 1000;
constexpr DWORD kQuiet = 2000;  // тишина после изменений, прежде чем мерить

struct Folder {
    std::wstring name, path;
    HANDLE dir = INVALID_HANDLE_VALUE;
    OVERLAPPED ov{};
    alignas(8) BYTE buf[16 * 1024];  // сами изменения не разбираем - важен факт
    bool dirty = true;               // первый замер - сразу
    ULONGLONG measuredAt = 0, changedAt = 0;
    std::deque<std::pair<ULONGLONG, ULONGLONG>> samples;  // время (тик) и размер, за последний час с запасом
    ULONGLONG dayStart = 0;                               // размер на первом замере сегодняшнего дня
    int day = -1;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<LiveSize> g_sizes;
HANDLE g_stop = nullptr, g_thread = nullptr;
std::vector<std::unique_ptr<Folder>> g_folders;  // живут, пока идёт поток

int LocalDay() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 400 + st.wMonth * 32 + st.wDay;
}

bool Arm(Folder& f) {
    DWORD got = 0;
    return ReadDirectoryChangesW(f.dir, f.buf, sizeof(f.buf), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE,
                                 &got, &f.ov, nullptr) != FALSE;
}

void Measure(Folder& f, ULONGLONG now) {
    ULONGLONG bytes = DirBytes(f.path);
    f.dirty = false;
    f.measuredAt = now;
    int day = LocalDay();
    if (day != f.day) f.day = day, f.dayStart = bytes;
    f.samples.push_back({now, bytes});
    while (f.samples.size() > 1 && now - f.samples.front().first > 70 * kMinute) f.samples.pop_front();
    // «за час»: с самого раннего замера, который не старше часа (если программа запущена недавно - за меньшее)
    ULONGLONG hourAgo = bytes;
    for (const auto& [at, b] : f.samples)
        if (now - at <= 60 * kMinute) {
            hourAgo = b;
            break;
        }
    LiveSize s;
    s.name = f.name;
    s.path = f.path;
    s.now = bytes;
    s.today = (LONGLONG)bytes - (LONGLONG)f.dayStart;
    s.hour = (LONGLONG)bytes - (LONGLONG)hourAgo;
    s.ready = true;
    AcquireSRWLockExclusive(&g_lock);
    for (LiveSize& x : g_sizes)
        if (x.path == s.path) x = s;
    ReleaseSRWLockExclusive(&g_lock);
}

DWORD WINAPI Loop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        std::vector<HANDLE> waits{g_stop};
        bool pending = false;
        for (auto& f : g_folders) {
            if (f->dir != INVALID_HANDLE_VALUE) waits.push_back(f->ov.hEvent);
            pending = pending || f->dirty;
        }
        // есть несчитанные изменения - просыпаться чаще, чтобы поймать тишину после них
        DWORD r = WaitForMultipleObjects((DWORD)waits.size(), waits.data(), FALSE, pending ? kQuiet : (DWORD)kMinute);
        if (r == WAIT_OBJECT_0) break;
        ULONGLONG now = GetTickCount64();
        for (auto& f : g_folders) {
            if (f->dir == INVALID_HANDLE_VALUE) continue;
            if (WaitForSingleObject(f->ov.hEvent, 0) == WAIT_OBJECT_0) {
                DWORD got = 0;
                GetOverlappedResult(f->dir, &f->ov, &got, FALSE);  // 0 байт - буфер переполнился, тоже изменение
                f->dirty = true;
                f->changedAt = now;
                ResetEvent(f->ov.hEvent);
                if (!Arm(*f)) {
                    CloseHandle(f->dir);  // папку удалили или отключили - дальше не следим
                    f->dir = INVALID_HANDLE_VALUE;
                }
            }
            // мерить, когда в папке 2 с тихо (файл дописан), и не чаще раза в минуту: пока идёт скачивание,
            // события сыплются сотнями. Первый замер - сразу после тишины.
            if (f->dirty && now - f->changedAt >= kQuiet && (!f->measuredAt || now - f->measuredAt >= kMinute))
                Measure(*f, now);
            if (!f->dirty && f->day != LocalDay()) Measure(*f, now);  // полночь: новый отсчёт «за сегодня»
        }
    }
    for (auto& f : g_folders) {
        if (f->dir == INVALID_HANDLE_VALUE) continue;
        CancelIoEx(f->dir, &f->ov);
        DWORD got = 0;
        GetOverlappedResult(f->dir, &f->ov, &got, TRUE);
        CloseHandle(f->dir);
    }
    for (auto& f : g_folders) CloseHandle(f->ov.hEvent);
    return 0;
}

}  // namespace

void LiveStart(const std::vector<std::pair<std::wstring, std::wstring>>& folders) {
    if (g_thread) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (const auto& [name, path] : folders) {
        if (path.empty()) continue;
        auto f = std::make_unique<Folder>();
        f->name = name;
        f->path = path;
        f->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        f->dir = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (f->dir != INVALID_HANDLE_VALUE && !Arm(*f)) {
            CloseHandle(f->dir);
            f->dir = INVALID_HANDLE_VALUE;
        }
        LiveSize s;
        s.name = name;
        s.path = path;
        g_sizes.push_back(s);
        g_folders.push_back(std::move(f));
    }
    g_thread = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
}

void LiveStop() {
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    g_thread = g_stop = nullptr;
}

std::vector<LiveSize> LiveSizes() {
    AcquireSRWLockShared(&g_lock);
    std::vector<LiveSize> v = g_sizes;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

}  // namespace tools
