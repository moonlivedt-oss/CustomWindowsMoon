#pragma once
#include <functional>
#include <set>

#include <windows.h>

#include "core/themes.h"

struct Config;

// Рамки и заголовки чужих окон через DWM (Windows 11), без внедрения в процессы.
// Рамка активного окна при переключении вспыхивает светлым акцентом и оседает в цвет темы,
// у прежнего окна плавно гаснет до стали. Полоса заголовка и её текст - в цветах темы,
// у неактивных окон текст приглушён. При выходе и в игровом режиме окна возвращаются к своим цветам.
class WindowFrame {
public:
    std::function<bool(HWND)> skip;  // окна, рамку которых держит кто-то ещё (закреплённые поверх всех)

    void Create(HINSTANCE inst);
    void Apply(const Config& cfg);
    void Suspend(bool on);  // игровой режим: всё вернуть и не трогать окна
    void Destroy();

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND w, LONG obj, LONG child, DWORD, DWORD);
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    bool Active() const { return (frame_ || captions_) && !suspended_; }
    bool Eligible(HWND w) const;
    void Hook(bool on);
    void Foreground(HWND w);
    void Sweep();  // новые окна получают цвета темы, закрытые забываются
    void Tick();
    void Paint(HWND w, bool active, COLORREF border);
    void ResetAll();

    HWND wnd_ = nullptr;
    HWINEVENTHOOK hook_ = nullptr;
    bool frame_ = true, captions_ = true, suspended_ = false;
    ThemeInfo theme_{};
    COLORREF accent_ = 0, flash_ = 0, idle_ = 0, caption_ = 0, captionIdle_ = 0, text_ = 0, textIdle_ = 0;
    std::set<HWND> touched_;
    HWND active_ = nullptr, prev_ = nullptr;
    ULONGLONG t0_ = 0;
    bool animating_ = false;
    int retries_ = 0;
};
