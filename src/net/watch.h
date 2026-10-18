#pragma once
// Сторож связи: в своём потоке раз в interval секунд лёгкий HTTPS-запрос; пропал интернет -
// короткая диагностика и событие окну (карточка «Починить?»), вернулся - событие «вернулся».
// Раз в несколько минут смотрит выходной узел и сообщает, когда страна сменилась.
#include <atomic>
#include <string>
#include <vector>

#include <windows.h>

#include "net/net.h"

namespace net {

struct Event {
    enum Kind { DOWN, UP, EXIT, GAME } kind = DOWN;
    Report report;      // для DOWN: что нашла диагностика
    std::wstring text;  // готовая строка: «Япония -> Германия», «Сервер ... 48 мс через happ-tun»
    bool worth = true;  // для GAME: есть ли что сказать (туннель или высокий пинг)
};

// лёгкое состояние для кольца и трея: обновляется каждый цикл сторожа
struct Light {
    bool known = false, online = false;
    std::wstring country, route;
    bool tunnel = false;
    int pingMs = -1;
};

class Watch {
public:
    // события приходят окну notify сообщением msg: lParam - Event*, удаляет получатель
    void Start(HWND notify, UINT msg);
    void Stop();
    void Configure(bool on, bool exitWatch, int intervalSec);
    void Kick(int graceMs = 25000);  // проверить сейчас; graceMs - сколько не считать обрывом (после сна сеть встаёт)
    Light Now() const;

private:
    static DWORD WINAPI Thread(void* self);
    void Loop();
    void Post(Event* e);

    HWND notify_ = nullptr;
    UINT msg_ = 0;
    HANDLE thread_ = nullptr, wake_ = nullptr;
    std::atomic<bool> stop_{false}, on_{true}, exit_{true};
    std::atomic<int> interval_{20};
    std::atomic<ULONGLONG> graceUntil_{0};
    std::atomic<bool> fresh_{false};  // замерить задержку туннеля сейчас (открыли кольцо сети)
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    Light light_;
};

// Игра запустилась: через ~20 с найти её сервер (TCP-соединения процесса), маршрут и пинг.
// Результат - событие GAME тому же окну.
void CheckGame(const std::vector<DWORD>& pids, const std::wstring& name, HWND notify, UINT msg);

}  // namespace net
