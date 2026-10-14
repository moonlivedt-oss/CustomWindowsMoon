#pragma once
#include <functional>
#include <string>

#include <windows.h>

#include "core/themes.h"

// Карточка Boost справа снизу: «Освободить ~1.2 ГБ перед игрой?», ход освобождения, итог,
// после игры - «Вернуть закрытые программы?». Не забирает фокус у игры, клики работают.
class BoostCard {
public:
    std::function<void()> onYes;  // «Освободить» или «Вернуть»

    void Create(HINSTANCE inst);
    void Destroy();
    void Tick();  // из общего опроса: выезд, наведение, само скрытие
    enum Mode { OFFER, WORKING, DONE, RESTORE };
    void Show(Mode mode, const std::wstring& title, const std::wstring& sub, const ThemeInfo& theme);
    void Hide();
    bool Visible() const { return open_; }
    Mode CurrentMode() const { return mode_; }

private:
    enum Hit { H_NONE, H_YES, H_NO, H_CLOSE, H_BODY, kHits };
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    Gdiplus::RectF Rect(Hit h) const;
    Hit HitAt(POINT p) const;
    void Render();

    HWND wnd_ = nullptr;
    ThemeInfo theme_{};
    Mode mode_ = OFFER;
    std::wstring title_, sub_;
    bool open_ = false, tracking_ = false, dirty_ = false;
    float slide_ = 0, hover_[kHits] = {};
    Hit hot_ = H_NONE, down_ = H_NONE;
    ULONGLONG until_ = 0;
    double k_ = 1;
    RECT work_{};
};
