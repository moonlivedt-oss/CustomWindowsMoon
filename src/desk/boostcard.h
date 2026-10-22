#pragma once
#include <functional>
#include <string>

#include "desk/toast.h"

// Карточка справа снизу: «Освободить ~1.2 ГБ перед игрой?», ход работы, итог, вопрос после игры.
// Ей же пользуются «Сеть» («Пропал интернет - починить?», «Выход сменился») и «Утилиты».
// Карточек может быть несколько - они складываются стопкой (см. Toast) и не перекрывают друг друга.
class BoostCard : public Toast {
public:
    std::function<void()> onYes;     // «Освободить», «Вернуть», «Починить»
    std::function<void()> onNo;      // вторая кнопка как действие (пусто - просто закрыть); сбрасывается после нажатия
    std::wstring yesLabel, noLabel;  // свои подписи кнопок; пусто - как у Boost
    float progress = -1;             // WORKING: доля 0..1 - полоска под текстом (ход DISM); -1 - без полоски
    std::function<void()> onStop;    // WORKING: кнопка «Стоп» (пусто - без кнопки); сбрасывается любым не-WORKING Show
    const BoostCard* below = nullptr;  // устарело: стопку теперь строит Toast
    // Вид для памяти карточек (desk/cardlog, Л10/Л11): пусто - по заголовку, «-» - не помнить (вечерний дайджест
    // сам себя не глушит). Сбрасывается каждым Show.
    std::wstring kind;

    void Create(HINSTANCE inst);
    enum Mode { OFFER, WORKING, DONE, RESTORE };
    // holdMs - сколько висеть (0 - по виду карточки: вопрос дольше, итог коротко);
    // glyph - знак слева (G_NONE - по виду и словам: «не получилось» -> крестик и т.п.)
    void Show(Mode mode, const std::wstring& title, const std::wstring& sub, const ThemeInfo& theme, int holdMs = 0,
              Glyph glyph = G_NONE);
    Mode CurrentMode() const { return mode_; }

private:
    enum { H_YES = H_USER, H_NO };
    float BodyHeight(Gdiplus::Graphics& g) override;
    void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) override;
    int HitContent(const Gdiplus::PointF& p) const override;
    void Click(int id) override;
    bool Busy() const override { return mode_ == WORKING; }
    void Closed() override;
    void Unanswered();  // вопрос ушёл без нажатия: в пропущенные и в счёт закрытий
    Gdiplus::Color Tone() const override;
    bool Buttons() const { return mode_ == OFFER || mode_ == RESTORE || StopButton(); }
    bool StopButton() const { return mode_ == WORKING && onStop; }
    std::wstring Label(bool yes) const;

    Mode mode_ = OFFER;
    bool asked_ = false;  // на экране вопрос с кнопкой действия, ответа ещё не было
    std::wstring askKind_, askYes_, askNo_;
    std::function<void()> askOnYes_, askOnNo_;
    Glyph glyph_ = G_NONE;
    std::wstring title_, sub_;
    float subH_ = 0;
    Gdiplus::RectF yes_, no_;  // кнопки в координатах окна (считаются при раскладке)
};
