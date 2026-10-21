#pragma once
#include <map>
#include <string>

#include <windows.h>

#include "core/themes.h"

struct Config;

// Прозрачность и «поверх всех» для любых окон.
// Alt + колесо над верхней полосой окна - прозрачность, Alt + средняя кнопка там же - закрепить
// поверх остальных. Закреплённое окно получает рамку цвета темы. Рядом с окном всплывает
// подсказка с уровнем. При выходе программы все окна возвращаются как были.
// Alt + левая кнопка в любом месте окна - тащить окно, Alt + правая - менять размер (тянется ближний край или угол,
// как в KDE). Не в играх, не в окнах на весь экран и не в программах, где Alt + мышь - своё управление камерой
// (Unreal, Blender, Maya, 3ds Max, Houdini, Substance, ZBrush, Krita, Photoshop).
class WindowFx {
public:
    void Create(HINSTANCE inst);
    void Apply(const Config& cfg);
    void Tick();
    bool Pinned(HWND w) const {
        auto it = states_.find(w);
        return it != states_.end() && it->second.pinned;
    }
    void Destroy();   // вернуть все окна как было
    void ResetAll();  // то же, но программа продолжает работать
    void TogglePin(HWND w);
    void SetBlocked(bool on);  // кольцо открыто или игра: Alt + мышь не трогаем
    bool Dragging() const;     // идёт Alt + тяга окна: кольцо у края не открываем

private:
    struct State {
        BYTE alpha = 255;
        bool addedLayered = false;  // WS_EX_LAYERED поставили мы
        bool pinned = false;
    };
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static bool OnMouse(WPARAM msg, const MSLLHOOKSTRUCT& m);  // в потоке перехвата (core/llmouse)
    static HWND TargetAt(POINT pt);
    void Wheel(HWND w, int delta);
    void SetAlpha(HWND w, BYTE a);
    void Unpin(HWND w, State& s);
    void Show(HWND w, int kind, int value, const std::wstring& title, const std::wstring& sub);
    void Paint();

    HWND wnd_ = nullptr;
    bool enabled_ = true;
    // Alt + мышь: окно, рамка в начале, где нажали, какие края тянуть (zones::ResizeEdges)
    HWND dragWin_ = nullptr;
    RECT dragFrom_{};
    POINT dragAt_{};
    bool dragSize_ = false;
    int dragEx_ = 2, dragEy_ = 2;
    void DragBegin(HWND w, POINT at, bool size);
    void DragMove(POINT pt);
    ThemeInfo theme_{};
    std::map<HWND, State> states_;
    // подсказка
    enum { K_ALPHA, K_PIN, K_UNPIN, K_DENIED };
    int kind_ = K_ALPHA, value_ = 100;
    std::wstring title_, sub_;
    RECT anchor_{};  // рамка окна, над которым подсказка
    ULONGLONG until_ = 0, lastPrune_ = 0;
    float show_ = 0;
};

// Закрепить активное окно (для горячих углов).
void PinForegroundWindow();
