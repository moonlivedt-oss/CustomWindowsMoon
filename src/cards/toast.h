#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "core/themes.h"

// Общая основа карточек-сообщений справа снизу (Boost, сеть, утилиты, снимок).
// Карточки не перекрывают друг друга: каждая новая встаёт над уже открытыми, а когда нижняя
// уходит, верхние плавно опускаются. Движение считается по времени, а не по кадрам:
// въезд справа с пружиной, уход вправо с растворением, высота подстраивается под текст.
// Наведение ставит таймер на паузу (полоска внизу показывает, сколько осталось), бросок вправо убирает карточку.
// Правый клик или «...» открывает меню прямо в карточке: отложить, повторять, закрыть (у карточек с HasMenu; у
// остальных правый клик просто убирает). Обычное всплывающее меню отняло бы фокус, а он у игры не отнимается.
class Toast {
public:
    virtual ~Toast() = default;
    void Tick();  // из общего опроса раз в 20 мс
    bool Visible() const { return open_; }
    void Hide() { open_ = false; }
    void Destroy();
    bool SaveFrame(const std::wstring& path) const;  // для разработки: последний кадр на ровном фоне в PNG

    enum Glyph { G_NONE, G_OK, G_FAIL, G_WARN, G_ASK, G_BUSY, G_NET, G_RESTORE, G_INFO };

protected:
    enum {
        H_NONE = 0,
        H_CLOSE = 1,
        H_BODY = 2,
        H_DOTS = 3,      // кнопка меню
        H_M_CANCEL = 4,  // меню: назад к карточке
        H_M_CLOSE = 5,   // меню: закрыть карточку
        H_M_MUTE = 6,    // меню: больше не показывать такие
        H_USER = 10,
        H_M_LATER = 20,   // меню: четыре кнопки «напомнить через»
        H_M_REPEAT = 24,  // меню: четыре кнопки «повторять каждые»
        kMaxHits = 40
    };
    static constexpr float kW = 384, kM = 28;  // ширина карточки и поля под тень, до масштаба
    static constexpr float kLiqL = 100;        // запас слева под брызги у жидкой карточки

    void CreateWnd(HINSTANCE inst, const wchar_t* cls, const wchar_t* title);
    // открыть (или обновить открытую): holdMs - сколько висеть, 0 - пока не закроют; onCursor - на мониторе курсора
    void Open(int holdMs, bool onCursor = false);
    void Invalidate() { dirty_ = true; }
    void Dismiss();  // закрыть, как крестиком (с Closed)
    // открытой карточке - новый таймер на том же месте: 0 - пока идёт работа, иначе показать итог ещё holdMs
    void Hold(int holdMs);

    // содержимое
    virtual float BodyHeight(Gdiplus::Graphics& g) = 0;  // высота карточки в dip под текущее содержимое
    virtual void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) = 0;
    virtual int HitContent(const Gdiplus::PointF&) const { return H_NONE; }
    virtual void Click(int) {}
    virtual void Closed() {}  // ушла по таймеру, крестику, правому клику или броску
    // идёт работа: кадры рисуются, вместо времени бежит блик. На таймер не влияет: чтобы не ушла - Open(0) или Hold(0)
    virtual bool Busy() const { return false; }
    virtual Gdiplus::Color Tone() const;                          // цвет полоски слева и знака
    virtual bool Message(UINT, WPARAM, LPARAM) { return false; }  // свои сообщения окна; true - обработано
    HWND Wnd() const { return wnd_; }

    // Жидкое движение: карточка вытекает из края экрана на перемычке, растягивается по скорости, брызгает при
    // ударе и дрожит, а уходя - затягивается обратно в край. Иначе - обычный въезд с растворением.
    virtual bool LiquidMotion() const { return false; }
    // своя покадровая анимация наследника; true - нужен новый кадр
    virtual bool Animate(double /*dtMs*/) { return false; }
    void Kick(float n) { sv_ += n; }  // толчок: карточка дрожит
    bool calmUpdate_ = false;         // следующий Open - лишь обновление хода работы: не вздрагивать
    virtual Gdiplus::Color ButtonTone() const { return ToColor(theme_.ac); }  // цвет главной кнопки
    // Для появления содержимого: 0..1 за dur секунд после паузы delay от момента, когда карточка почти приехала.
    float Reveal(float delay, float dur) const;

    // Меню карточки
    virtual bool HasMenu() const { return false; }
    virtual bool MenuRepeat() const { return false; }  // есть ли строка «повторять каждые»
    virtual bool MenuMute() const { return false; }    // есть ли «не показывать такие»
    virtual int MenuRepeatActive() const { return 0; }  // каждые сколько минут уже повторяется, 0 - нет
    virtual void MenuChoose(int /*minutes*/, bool /*repeat*/) {}  // 0 минут с repeat - выключить повтор
    virtual void MenuMuteKind() {}
    virtual void Idle(ULONGLONG /*now*/) {}  // каждый такт, даже у спрятанной: отложенная карточка просыпается здесь
    void OpenMenu();
    void CloseMenu();
    Gdiplus::RectF MenuRect(int id) const;

    // помощники рисования для наследников
    float k() const { return (float)k_; }
    float HoverOf(int id) const { return id >= 0 && id < kMaxHits ? hover_[id] : 0; }
    bool Pressed(int id) const { return down_ == id && hot_ == id; }
    bool Hovered() const { return hot_ != H_NONE; }  // мышь над карточкой (над кнопкой - тоже)
    void DrawGlyph(Gdiplus::Graphics& g, float cx, float cy, Glyph glyph, Gdiplus::Color c) const;
    void DrawButton(Gdiplus::Graphics& g, const Gdiplus::RectF& r, const std::wstring& label, bool primary,
                    int id) const;
    float ButtonWidth(Gdiplus::Graphics& g, const std::wstring& label) const;
    float TextHeight(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& f, float width) const;
    Gdiplus::RectF Body() const;  // в координатах окна
    // в Closed: убрали руками (крестик, правый клик, бросок), а не ушла по таймеру
    bool ClosedByHand() const { return byHand_; }

    ThemeInfo theme_{};
    double k_ = 1;

private:
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    int HitAt(POINT p) const;
    void Render();
    void StepLiquid(double dtMs, bool& anim);
    void PaintGoo(int W, int H, int clipX, Gdiplus::Color fill);
    void PaintMenu(Gdiplus::Graphics& g, Gdiplus::Color fill, float radius);
    void MenuClick(int id);
    float MenuHeight() const;
    Gdiplus::RectF DotsRect() const;
    float Presence() const;  // сколько места карточка занимает в стопке (0..1)
    int TargetY() const;

    HWND wnd_ = nullptr;
    RECT work_{};
    bool open_ = false, tracking_ = false, dirty_ = false, dragging_ = false, byHand_ = false;
    float hover_[kMaxHits] = {};
    int hot_ = H_NONE, down_ = H_NONE;
    // движение
    float appear_ = 0, appearV_ = 0;  // 0 - спрятана, 1 - на месте (пружина, может перелететь)
    float y_ = 0;                     // текущая высота над нижним краем, px
    bool yInit_ = false;
    float dragX_ = 0, dragStart_ = 0, fling_ = 0;  // бросок вправо
    float pulse_ = 0;                              // короткий толчок при обновлении открытой карточки
    float h_ = 120, hT_ = 120;                     // высота карточки и её цель, dip
    bool hInit_ = false;
    // жидкое движение; единицы - dip и секунды
    struct Drop {
        float t, life, y, amp, r, dy;
    };
    std::vector<Drop> drops_;
    float liqX_ = 0, liqV_ = 0, s_ = 1, sv_ = 0;  // сдвиг от места, его скорость; растяжение и его скорость
    float tether_ = 0, tetherT_ = 0;              // перемычка до края экрана: 1 - есть, 0 - оборвалась
    bool splashed_ = false, wasDrag_ = false;
    ULONGLONG revealAt_ = 0;
    // меню: открыто ли, плавность появления, когда курсор ушёл; итог выбора показывается мгновение, потом карточка уходит
    bool menuOpen_ = false, menuDismiss_ = false;
    float menuT_ = 0;
    ULONGLONG menuAwaySince_ = 0, menuDoneAt_ = 0;
    std::wstring menuNote_;
    std::vector<uint8_t> goo_, gooTmp_;
    ULONGLONG last_ = 0, order_ = 0, lastTimerFrame_ = 0;
    double left_ = 0, hold_ = 0;  // мс до ухода и полное время
    // кадр
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
};
