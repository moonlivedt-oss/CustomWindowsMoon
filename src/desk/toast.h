#pragma once
#include <string>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "core/themes.h"

// Общая основа карточек-сообщений справа снизу (Boost, сеть, утилиты, снимок).
// Карточки не перекрывают друг друга: каждая новая встаёт над уже открытыми, а когда нижняя
// уходит, верхние плавно опускаются. Движение считается по времени, а не по кадрам:
// въезд справа с пружиной, уход вправо с растворением, высота подстраивается под текст.
// Наведение ставит таймер на паузу (полоска внизу показывает, сколько осталось),
// правый клик или бросок вправо убирает карточку. Фокус у игры не отнимается.
class Toast {
public:
    virtual ~Toast() = default;
    void Tick();  // из общего опроса раз в 20 мс
    bool Visible() const { return open_; }
    void Hide() { open_ = false; }
    void Destroy();

    enum Glyph { G_NONE, G_OK, G_FAIL, G_WARN, G_ASK, G_BUSY, G_NET, G_RESTORE, G_INFO };

protected:
    enum { H_NONE = 0, H_CLOSE = 1, H_BODY = 2, H_USER = 10, kMaxHits = 24 };
    static constexpr float kW = 384, kM = 28;  // ширина карточки и поля под тень, до масштаба

    void CreateWnd(HINSTANCE inst, const wchar_t* cls, const wchar_t* title);
    // открыть (или обновить открытую): holdMs - сколько висеть, 0 - пока не закроют; onCursor - на мониторе курсора
    void Open(int holdMs, bool onCursor = false);
    void Invalidate() { dirty_ = true; }
    void Dismiss();  // закрыть, как крестиком (с Closed)

    // содержимое
    virtual float BodyHeight(Gdiplus::Graphics& g) = 0;  // высота карточки в dip под текущее содержимое
    virtual void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) = 0;
    virtual int HitContent(const Gdiplus::PointF&) const { return H_NONE; }
    virtual void Click(int) {}
    virtual void Closed() {}                     // ушла по таймеру, крестику, правому клику или броску
    virtual bool Busy() const { return false; }  // идёт работа: не уходит, кадры рисуются
    virtual Gdiplus::Color Tone() const;         // цвет полоски слева и знака
    virtual bool Message(UINT, WPARAM, LPARAM) { return false; }  // свои сообщения окна; true - обработано
    HWND Wnd() const { return wnd_; }

    // помощники рисования для наследников
    float k() const { return (float)k_; }
    float HoverOf(int id) const { return id >= 0 && id < kMaxHits ? hover_[id] : 0; }
    bool Pressed(int id) const { return down_ == id && hot_ == id; }
    void DrawGlyph(Gdiplus::Graphics& g, float cx, float cy, Glyph glyph, Gdiplus::Color c) const;
    void DrawButton(Gdiplus::Graphics& g, const Gdiplus::RectF& r, const std::wstring& label, bool primary,
                    int id) const;
    float ButtonWidth(Gdiplus::Graphics& g, const std::wstring& label) const;
    float TextHeight(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& f, float width) const;
    Gdiplus::RectF Body() const;  // в координатах окна

    ThemeInfo theme_{};
    double k_ = 1;

private:
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    int HitAt(POINT p) const;
    void Render();
    float Presence() const;  // сколько места карточка занимает в стопке (0..1)
    int TargetY() const;

    HWND wnd_ = nullptr;
    RECT work_{};
    bool open_ = false, tracking_ = false, dirty_ = false, dragging_ = false;
    float hover_[kMaxHits] = {};
    int hot_ = H_NONE, down_ = H_NONE;
    // движение
    float appear_ = 0, appearV_ = 0;  // 0 - спрятана, 1 - на месте (пружина, может перелететь)
    float y_ = 0;                     // текущая высота над нижним краем, px
    bool yInit_ = false;
    float dragX_ = 0, dragStart_ = 0, fling_ = 0;  // бросок вправо
    float pulse_ = 0;                              // короткий толчок при обновлении открытой карточки
    float h_ = 120;                                // высота карточки, dip
    ULONGLONG last_ = 0, order_ = 0, lastTimerFrame_ = 0;
    double left_ = 0, hold_ = 0;  // мс до ухода и полное время
    // кадр
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
};
