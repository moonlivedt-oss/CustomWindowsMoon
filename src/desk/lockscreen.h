#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <vector>

#include <windows.h>

#include "core/themes.h"

struct Config;

// Экран блокировки в цветах темы и плавный переход между ним и рабочим столом.
namespace lockscreen {

// Сборка картинки идёт в своём потоке; студия смотрит на done и забирает сообщение.
struct Job {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<bool> done{false};
    bool ok = false;
    std::wstring message;
    HWND notify = nullptr;  // кому InvalidateRect по готовности
};

// Картинка (lock_source или обои) подгоняется под экран и красится под тему: тени уходят в графит темы,
// цвета, далёкие от акцента, притихают, края темнеют, сверху вуаль под часы, снизу - под подписи,
// у нижнего края тонкий световой шов цвета акцента, как у колец. Результат ставится на экран блокировки.
std::shared_ptr<Job> ApplyAsync(const Config& cfg, HWND notify);
std::wstring OutputFile();  // готовая картинка (%APPDATA%\MoonDesk\lockscreen-theme.jpg)

// Аватар над полем пароля в стиле колец: графитовый диск, кольцо секторов с одним «наведённым»
// сектором цвета акцента, в середине серебряный полумесяц или своя картинка (avatar_source).
// Кладёт PNG 448x448 в %APPDATA%\MoonDesk\avatar.png; поставить его Windows даёт только через Параметры.
bool BuildAvatar(const Config& cfg, std::wstring& out, std::wstring& msg);

// Переход: при блокировке за экраном блокировки заранее встаёт копия его картинки (размытая, как у
// экрана входа, или чёткая). После входа - «портал»: на месте аватара появляется кольцо секторов и
// раскрывается на весь экран, внутри круга открывается стол, по краю идёт светящаяся кромка цвета
// акцента. Или картинка просто тает. Блокировка из горячего угла проигрывает то же в обратную сторону.
class Reveal {
public:
    void Create(HINSTANCE inst, HWND sessionWnd);  // sessionWnd получает WM_WTSSESSION_CHANGE
    void Destroy();
    void Apply(const Config& cfg);
    void OnSession(WPARAM code);  // из WM_WTSSESSION_CHANGE
    void LockWithCurtain();       // для горячего угла
    void Demo();                  // показать переход из студии: затемнение в картинку и обратно, без блокировки
    void Intro();                 // при входе после включения: картинка проступает и раскрывается порталом
    bool ScreenOff();             // угол «выключить экран»: схлопнуть портал и погасить экран; false - без перехода
    void OnDisplay(bool on);      // экран включился или погас (GUID_CONSOLE_DISPLAY_STATE)
    void OnPower(WPARAM event);   // сон и пробуждение (WM_POWERBROADCAST)
    void Guard();                 // раз в секунду: окно не висит над столом, если переход оборвался

private:
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    bool Prepare();         // собрать кадр под весь основной монитор
    void Plain();           // в кадре вся картинка
    void Portal(double t);  // 0 - картинка целиком, 1 - портал раскрыт на весь экран
    BYTE PortalAlpha(double t) const;
    void BuildCells();   // узор стекла: таблицы ячеек под весь экран
    void AimAtCursor();  // портал раскрывается оттуда, где курсор
    void Push(BYTE alpha);
    void Tick();
    void Hide();
    void Open();   // начать раскрытие портала от курсора
    void Await();  // картинка стоит, ждём пробуждения: экран включится или придёт ввод

    HWND wnd_ = nullptr, session_ = nullptr;
    bool enabled_ = true;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_ = nullptr;
    RECT mon_{};
    enum { IDLE, INTRO, CURTAIN, HOLD, FADE } state_ = IDLE;
    bool offAfter_ = false;  // после схлопывания погасить экран, а не блокировать
    bool waitWake_ = false;  // картинка стоит до пробуждения (сон, выключенный экран)
    bool locked_ = false;    // сеанс заблокирован: раскрываться только после входа
    bool bootPortal_ = true, wakePortal_ = true;
    ULONGLONG holdAt_ = 0;  // когда картинка встала (ввод позже - значит проснулись)
    ULONGLONG t0_ = 0;
    ULONGLONG pushAt_ = 0;  // когда выведен последний кадр
    bool demo_ = false;
    bool portal_ = true, sounds_ = true, sounded_ = false;
    ThemeInfo theme_{};
    std::vector<uint32_t> src_;                   // картинка без выреза
    int pattern_ = 1;                             // узор стекла: 0 - нет, 1 - соты, 2 - осколки
    POINT origin_{};                              // точка раскрытия в координатах монитора
    std::vector<uint16_t> cell_;                  // на пиксель: номер ячейки
    std::vector<uint8_t> edge_;                   // на пиксель: 0 - середина ячейки, 255 - граница
    std::vector<int8_t> tex_;                     // на пиксель: фактура стекла (грани, линии, блики)
    std::vector<float> cellX_, cellY_, cellJit_;  // на ячейку: середина и случайный сдвиг
    // на ячейку в кадре: сколько осталось, прозрачность, накал, насколько приподнята
    std::vector<uint16_t> keep_, fade_, heat_, lift_;
    std::vector<float> lightX_, lightY_;  // направление на курсор (откуда свет)
    uint32_t* bits_ = nullptr;
};

}  // namespace lockscreen
