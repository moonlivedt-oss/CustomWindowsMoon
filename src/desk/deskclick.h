#pragma once
#include <functional>

#include <windows.h>

// Двойной клик по пустому рабочему столу. Клики не перехватываются: проводник получает их как обычно,
// мы только смотрим, что два нажатия пришлись на стол быстро и рядом.
class DeskClick {
public:
    std::function<void(POINT)> onDouble;
    bool enabled = true;

    void Create(HINSTANCE inst);
    void Destroy();
    void Hook(bool on);  // перехват мыши (снимается в игровом режиме)

private:
    static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static bool IsDesktop(POINT pt);

    HWND wnd_ = nullptr;
    HHOOK hook_ = nullptr;
    DWORD lastTime_ = 0;
    POINT lastPt_{};
};
