#pragma once
#include <string>

#include <windows.h>

#include "core/themes.h"

// Волна сот по краям экрана, когда игровой режим включается или выключается: от середины нижнего
// края в обе стороны бегут загорающиеся соты и встречаются наверху, внизу на миг появляется
// плашка «Игровой режим: игра» или «Игровой режим выключен». Длится чуть больше секунды
// и сама себе не мешает: в эксклюзивном полноэкранном режиме не показывается.
class HexWave {
public:
    void Create(HINSTANCE inst);
    void Destroy();
    void Play(bool entering, const std::wstring& label, const ThemeInfo& theme);

private:
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    void Frame();
    void Stop();

    HWND wnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    RECT mon_{};
    ThemeInfo theme_{};
    bool entering_ = true;
    std::wstring label_;
    ULONGLONG t0_ = 0;
};
