#pragma once
#include <atomic>
#include <windows.h>

// Яркость экрана колесом из угла. Внешний монитор - через DDC/CI, экран ноутбука - через WMI.
// Оба способа медленные (десятки миллисекунд), поэтому работают в своём потоке: шаги колеса
// копятся, а когда яркость выставлена, окну приходит сообщение с новым уровнем (wParam, 0..100).
class Brightness {
public:
    ~Brightness() { Stop(); }
    void Start(HWND notify, UINT msg);
    void Stop();
    void Add(HMONITOR mon, int delta);

private:
    static DWORD WINAPI Thread(void* self);
    void Loop();

    HANDLE thread_ = nullptr, wake_ = nullptr;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::atomic<bool> quit_{false};
    SRWLOCK mu_ = SRWLOCK_INIT;
    HMONITOR mon_ = nullptr;
    int pending_ = 0;
};
