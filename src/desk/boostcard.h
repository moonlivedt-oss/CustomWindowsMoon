#pragma once
#include <functional>
#include <string>

#include <windows.h>

#include "core/themes.h"

// Карточка справа снизу: «Освободить ~1.2 ГБ перед игрой?», ход работы, итог, вопрос после игры.
// Ей же пользуется раздел «Сеть»: «Пропал интернет - починить?». Не забирает фокус у игры, клики работают.
// Карточек может быть несколько: below - над какой встать, если та видна.
class BoostCard {
public:
    std::function<void()> onYes;  // «Освободить», «Вернуть», «Починить»
    std::wstring yesLabel, noLabel;  // свои подписи кнопок; пусто - как у Boost
    const BoostCard* below = nullptr;

    void Create(HINSTANCE inst);
    void Destroy();
    void Tick();  // из общего опроса: выезд, наведение, само скрытие
    enum Mode { OFFER, WORKING, DONE, RESTORE };
    // holdMs - сколько висеть (0 - по виду карточки: вопрос дольше, итог коротко)
    void Show(Mode mode, const std::wstring& title, const std::wstring& sub, const ThemeInfo& theme, int holdMs = 0);
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
    float lift_ = 0;  // насколько поднята над нижней карточкой
};
