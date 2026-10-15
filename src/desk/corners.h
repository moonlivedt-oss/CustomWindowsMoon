#pragma once
#include <atomic>
#include <string>

#include <windows.h>

#include "desk/brightness.h"

struct Config;

// Горячие углы: курсор задержался в углу экрана - выполняется действие.
// Пока курсор ждёт, из угла растёт свечение; отвёл раньше - ничего не случится.
// Два быстрых толчка в угол - второе действие. Колесо в углу - громкость, яркость или треки,
// уровень показывается дугой прямо в углу.
namespace corners {

enum Corner { TL, TR, BL, BR, kCount };

struct Action {
    const wchar_t* key;   // как пишется в radial.cfg
    const wchar_t* name;  // для студии
};
const Action* Actions(int& count);               // список встроенных действий (первое - «ничего»)
const Action* WheelActions(int& count);          // что делает колесо (первое - «ничего»)
std::wstring ActionName(const std::wstring& v);  // подпись для значения из radial.cfg
void Run(const std::wstring& action);
// действия, которые делает main.cpp
void SetWindowsRing(void (*show)());  // windows: кольцо окон
void SetLock(void (*lock)());         // lock: блокировка с переходом
void SetScreenOff(bool (*off)());     // screen_off: выключить с переходом; false - выключить по-старому
void SetBoost(void (*boost)());       // boost: карточка «освободить память»
void SetNet(void (*net)());           // net: кольцо «Сеть»
void SetTools(void (*tools)());       // tools: кольцо «Утилиты»
// keys:ctrl+shift+esc -> «Ctrl + Shift + Esc»; виртуальная клавиша -> имя для keys:
std::wstring KeyName(UINT vk);
// на этом мониторе открыта игра или видео на весь экран
bool FullscreenOn(HMONITOR mon, const RECT& monRect);

class HotCorners {
public:
    void Create(HINSTANCE inst);
    void Destroy();
    // вызывается из опроса раз в 20 мс; blocked - сейчас нельзя (зажат Alt, открыто меню)
    void Tick(const Config& cfg, bool blocked);

private:
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static bool OnMouse(WPARAM msg, const MSLLHOOKSTRUCT& m);  // в потоке перехвата (core/llmouse)
    int CornerAt(POINT pt, int tol, POINT* at, RECT* mon, HMONITOR* hmon) const;
    void Wheel(int corner, int delta);
    void ShowLevel(int corner, int level, const wchar_t* label);
    void Paint();
    void Hide();

    HWND wnd_ = nullptr;
    const Config* cfg_ = nullptr;
    std::atomic<int> wheelMask_{0};          // в каких углах колесу что-то назначено (снимок для перехвата)
    std::atomic<bool> skipFullscreen_{true};
    Brightness bright_;
    int corner_ = -1;      // в каком углу курсор
    ULONGLONG since_ = 0;  // когда пришёл в угол
    bool fired_ = false;   // уже сработал, ждём, пока курсор уйдёт
    float progress_ = 0;   // 0..1 - сколько осталось ждать
    float flash_ = 0;      // вспышка после срабатывания
    int lastCorner_ = -1;  // для двойного толчка: откуда курсор ушёл и когда
    ULONGLONG lastLeave_ = 0;
    bool lastFired_ = false;
    int wheelCorner_ = -1;  // где недавно крутили колесо
    ULONGLONG wheelAt_ = 0;
    int levelCorner_ = -1, level_ = 0;  // уровень громкости или яркости после колеса
    std::wstring levelLabel_;
    ULONGLONG levelUntil_ = 0;
    float levelShow_ = 0;
    POINT at_{};  // точка угла
    RECT mon_{};
    COLORREF color_ = 0;
};

}  // namespace corners
