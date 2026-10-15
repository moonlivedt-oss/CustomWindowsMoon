#pragma once
#include <functional>

#include <windows.h>

// Двойной клик по пустому рабочему столу. Клики не перехватываются: проводник получает их как обычно,
// мы только смотрим, что два нажатия пришлись на стол быстро и рядом.
class DeskClick {
public:
    std::function<void(POINT)> onDouble;  // задать до Create

    void Create(HINSTANCE inst);
    void SetEnabled(bool on);  // выключено - мышь через нас не ходит вовсе
    void Destroy();

private:
    static bool OnMouse(WPARAM msg, const MSLLHOOKSTRUCT& m);  // в потоке перехвата (core/llmouse)
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static bool IsDesktop(POINT pt);

    HWND wnd_ = nullptr;
    DWORD lastTime_ = 0;  // трогает только поток перехвата
    POINT lastPt_{};
};
