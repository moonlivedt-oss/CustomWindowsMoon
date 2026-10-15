#include "core/llmouse.h"

#include <atomic>

namespace llmouse {

namespace {

constexpr UINT WM_SYNC = WM_APP + 1;

std::atomic<Handler> g_handler[kSlots];
std::atomic<bool> g_need[kSlots];
std::atomic<bool> g_suspend{false};
HANDLE g_thread = nullptr;
DWORD g_tid = 0;

LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto& m = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        for (int i = 0; i < kSlots; ++i) {
            if (!g_need[i].load(std::memory_order_relaxed)) continue;
            Handler h = g_handler[i].load(std::memory_order_relaxed);
            if (h && h(wp, m)) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

bool Wanted() {
    if (g_suspend) return false;
    for (int i = 0; i < kSlots; ++i)
        if (g_need[i] && g_handler[i]) return true;
    return false;
}

DWORD WINAPI Thread(void* ready) {
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // очередь потока: теперь можно слать WM_SYNC
    SetEvent((HANDLE)ready);
    HHOOK hook = nullptr;
    auto sync = [&] {
        bool want = Wanted();
        if (want && !hook) {
            hook = SetWindowsHookExW(WH_MOUSE_LL, Proc, GetModuleHandleW(nullptr), 0);
        } else if (!want && hook) {
            UnhookWindowsHookEx(hook);
            hook = nullptr;
        }
    };
    sync();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (msg.message == WM_SYNC) sync();
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

// Перехват ставится и снимается только в своём потоке: туда уходит просьба пересчитать, нужен ли он.
void Kick() {
    if (!g_thread) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_thread = CreateThread(nullptr, 0, Thread, ready, 0, &g_tid);
        if (!g_thread) {
            CloseHandle(ready);
            return;
        }
        SetThreadPriority(g_thread, THREAD_PRIORITY_HIGHEST);  // мышь не ждёт остальных потоков
        WaitForSingleObject(ready, 2000);
        CloseHandle(ready);
    }
    PostThreadMessageW(g_tid, WM_SYNC, 0, 0);
}

}  // namespace

void Set(Slot s, Handler h) { g_handler[s] = h; }

void Need(Slot s, bool on) {
    if (g_need[s].exchange(on) != on) Kick();
}

void Suspend(bool on) {
    if (g_suspend.exchange(on) != on) Kick();
}

void Shutdown() {
    if (!g_thread) return;
    PostThreadMessageW(g_tid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_tid = 0;
}

}  // namespace llmouse
