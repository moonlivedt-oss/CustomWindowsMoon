#include "core/mem.h"

#include <windows.h>

namespace mem {

namespace {
UINT_PTR g_timer = 0;
std::function<bool()> g_busy;

void Trim() {
    HANDLE heaps[64];
    DWORD n = GetProcessHeaps(64, heaps);
    for (DWORD i = 0; i < n && i < 64; ++i) HeapCompact(heaps[i], 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

void CALLBACK OnTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (g_busy && g_busy()) return;  // таймер остаётся: попробуем на следующем тике
    KillTimer(nullptr, g_timer);
    g_timer = 0;
    Trim();
}
}  // namespace

void TrimSoon(unsigned delayMs) {
    // с hWnd = nullptr тот же id перезапускает таймер: серия скрытий даёт одну обрезку
    g_timer = SetTimer(nullptr, g_timer, delayMs, OnTimer);
}

void SetBusy(std::function<bool()> busy) { g_busy = std::move(busy); }

}  // namespace mem
