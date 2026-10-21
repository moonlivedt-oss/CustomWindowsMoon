#include "desk/gamepad.h"

#include <xinput.h>

#include <atomic>
#include <initializer_list>
#include <cmath>

namespace gamepad {

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

HANDLE g_thread = nullptr, g_stop = nullptr;
std::atomic<HWND> g_target{nullptr};
std::atomic<bool> g_enabled{false}, g_connected{false};

GetStateFn Load() {
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            if (auto fn = (GetStateFn)(void*)GetProcAddress(m, "XInputGetState")) return fn;
    return nullptr;
}

void Post(Action a, LPARAM lp = 0) {
    if (HWND w = g_target) PostMessageW(w, WM_PAD, a, lp);
}

DWORD WINAPI Loop(void*) {
    GetStateFn get = Load();
    if (!get) return 0;
    WORD was = 0;
    ULONGLONG viewSince = 0;
    bool opened = false;
    int lastX = 0, lastY = 0;
    for (;;) {
        bool any = false;
        XINPUT_STATE st{};
        if (g_enabled) {
            for (DWORD i = 0; i < XUSER_MAX_COUNT && !any; ++i)
                if (get(i, &st) == ERROR_SUCCESS) any = true;
        }
        g_connected = any;
        if (WaitForSingleObject(g_stop, any ? 16 : 2000) == WAIT_OBJECT_0) break;
        if (!any) {
            was = 0;
            viewSince = 0;
            continue;
        }
        Edge e{was, st.Gamepad.wButtons};
        was = e.now;
        ULONGLONG now = GetTickCount64();
        // View зажата 0,6 с - открыть (один раз за нажатие)
        if (e.now & XINPUT_GAMEPAD_BACK) {
            if (!viewSince) viewSince = now, opened = false;
            if (!opened && now - viewSince >= 600) {
                opened = true;
                Post(OPEN);
            }
        } else {
            viewSince = 0;
        }
        if (e.Pressed(XINPUT_GAMEPAD_A)) Post(ACCEPT);
        if (e.Pressed(XINPUT_GAMEPAD_B)) Post(CANCEL);
        if (e.Pressed(XINPUT_GAMEPAD_LEFT_SHOULDER)) Post(BACK);
        if (e.Pressed(XINPUT_GAMEPAD_RIGHT_SHOULDER)) Post(DRILL);
        // стик: только заметный наклон и только когда он поменялся - иначе мышь бы «прилипала» к кольцу
        int x = st.Gamepad.sThumbLX, y = st.Gamepad.sThumbLY;
        double mag = std::sqrt((double)x * x + (double)y * y);
        if (mag < XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 1.4) x = y = 0;
        if (std::abs(x - lastX) + std::abs(y - lastY) > 900 || ((x || y) == 0 && (lastX || lastY))) {
            lastX = x, lastY = y;
            if (x || y) Post(MOVE, MAKELPARAM((WORD)(x + 32768), (WORD)(y + 32768)));
        }
    }
    return 0;
}

}  // namespace

void Start(HWND target, bool enabled) {
    g_target = target;
    g_enabled = enabled;
    if (g_thread || !enabled) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void Stop() {
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    g_thread = g_stop = nullptr;
}

bool Connected() { return g_connected; }

}  // namespace gamepad
