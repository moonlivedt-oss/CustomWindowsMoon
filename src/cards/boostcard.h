#pragma once
#include <functional>
#include <string>
#include <vector>

#include "cards/toast.h"

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
    // Вид для памяти карточек (cards/cardlog, Л10/Л11): пусто - по заголовку, «-» - не помнить (вечерний дайджест
    // сам себя не глушит). Сбрасывается каждым Show.
    std::wstring kind;
    // «Подробнее» под текстом: что именно произойдёт и что можно потерять (system/explain). Задаётся перед Show,
    // сбрасывается каждым Show - как kind.
    std::wstring more;
    // Тайна в тексте (код из SMS): пока мышь не над карточкой, вместо неё точки - код не прочтёт тот, кто смотрит на
    // экран или его запись. Задаётся перед Show, сбрасывается каждым Show.
    std::wstring secret;
    // Уровень 0..1: под текстом наливается жидкая шкала («Память на пределе»); -1 - без неё. Задаётся перед Show,
    // сбрасывается каждым Show.
    float meter = -1;

    void Create(HINSTANCE inst);
    enum Mode { OFFER, WORKING, DONE, RESTORE };
    // holdMs - сколько висеть (0 - по виду карточки: вопрос дольше, итог коротко);
    // glyph - знак слева (G_NONE - по виду и словам: «не получилось» -> крестик и т.п.)
    bool Show(Mode mode, const std::wstring& title, const std::wstring& sub, const ThemeInfo& theme, int holdMs = 0,
              Glyph glyph = G_NONE);
    Mode CurrentMode() const { return mode_; }
    // для проверки меню настоящими сообщениями окна (--card-snap later-sim)
    void DemoRightClick();
    void DemoClickMenu(int row, int index);
    void DemoClickYes();
    void DemoExpire() { Hold(1); }
    void DemoFastForward();
    int LaterCount() const { return (int)later_.size(); }
    int RepeatMinutes() const { return repeatMin_; }
    void ExpandMore() {  // раскрыть «Подробнее» (снимки --card-snap)
        moreOpen_ = !more_.empty();
        Invalidate();
    }

private:
    enum { H_YES = H_USER, H_NO, H_MORE };
    // Отложенное сообщение: всё, чтобы показать его снова с теми же кнопками. Живёт до выхода из программы.
    struct Later {
        ULONGLONG due = 0;
        int repeatMin = 0;
        Mode mode = DONE;
        std::wstring kind, title, sub, subHidden, more, yes, no;
        std::function<void()> onYes, onNo;
        int hold = 0;
        Glyph glyph = G_NONE;
        float meter = -1;
    };
    void Enqueue(Later l);
    bool HasMenu() const override { return mode_ != WORKING; }
    bool MenuRepeat() const override { return true; }
    bool MenuMute() const override { return infoKind_ != L"-"; }
    int MenuRepeatActive() const override { return repeatMin_; }
    void MenuChoose(int minutes, bool repeat) override;
    void MenuMuteKind() override;
    void Idle(ULONGLONG now) override;
    float BodyHeight(Gdiplus::Graphics& g) override;
    void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) override;
    int HitContent(const Gdiplus::PointF& p) const override;
    void Click(int id) override;
    bool Busy() const override { return mode_ == WORKING; }
    bool LiquidMotion() const override { return true; }
    bool Animate(double dtMs) override;
    Gdiplus::Color ButtonTone() const override;
    void DrawLevel(Gdiplus::Graphics& g, const Gdiplus::RectF& track, float level) const;
    void Closed() override;
    void Unanswered();  // вопрос ушёл без нажатия: в пропущенные и в счёт закрытий
    void InfoGone();    // сообщение без кнопок ушло: не читали (мышь не подходила) - в счёт закрытий, читали - с нуля
    Gdiplus::Color Tone() const override;
    bool Buttons() const { return mode_ == OFFER || mode_ == RESTORE || StopButton(); }
    bool StopButton() const { return mode_ == WORKING && onStop; }
    std::wstring Label(bool yes) const;

    Mode mode_ = OFFER;
    bool asked_ = false;  // на экране вопрос с кнопкой действия, ответа ещё не было
    // на экране сообщение без кнопок, которое пришло само (не итог только что нажатого): его тоже можно заглушить,
    // если его раз за разом не читают
    bool info_ = false, seen_ = false;
    std::wstring infoKind_;
    std::wstring askKind_, askYes_, askNo_;
    std::function<void()> askOnYes_, askOnNo_;
    Glyph glyph_ = G_NONE;
    std::wstring title_, sub_, more_;
    std::wstring subHidden_;  // sub_ с тайной под точками (пусто - тайны нет)
    bool moreOpen_ = false;
    std::vector<Later> later_;
    Later cur_;              // показанное повторяющееся: снимок, а не живые поля (их мог переписать следующий вопрос)
    int repeatMin_ = 0;      // показанное повторяется каждые столько минут
    bool refiring_ = false;  // Show вызван из Idle
    int holdUsed_ = 0;
    float subH_ = 0, moreH_ = 0, meter_ = -1;
    ULONGLONG lastPulse_ = 0;
    Gdiplus::RectF yes_, no_, link_;  // кнопки и ссылка «Подробнее» в координатах окна (считаются при раскладке)
};
