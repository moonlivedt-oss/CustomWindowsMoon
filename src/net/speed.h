#pragma once
// Скорость интернета (И26): загрузка и отдача через speed.cloudflare.com, задержка - повтором HTTPS (TUN подделывает
// пинг). Каждый замер помнит, шёл ли он через туннель и через какую страну, - история в %APPDATA%\MoonDesk\net\
// speed.tsv, и сравнение «через туннель / напрямую» по ней.
#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "net/net.h"

namespace net {

struct Speed {
    ULONGLONG when = 0;       // FILETIME одним числом
    double down = 0, up = 0;  // Мбит/с; 0 - не измерилось
    int pingMs = -1;
    bool tunnel = false;
    std::wstring via;  // адаптер туннеля или физический; страна выхода через запятую
};

// Ход замера для живого показа в студии (Н1): пишет поток замера, читает кадр.
struct SpeedLive {
    std::atomic<int> phase{0};    // 0 - ещё нет, 1 - задержка, 2 - загрузка, 3 - отдача, 4 - готово
    std::atomic<double> mbps{0};  // скорость сейчас, Мбит/с
    std::atomic<double> part{0};  // сколько прошло от фазы, 0..1
};

// Замер целиком (~20 с): задержка, загрузка (до 10 с), отдача (до 8 с). Блокирующий - из рабочего потока.
Speed MeasureSpeed(const Log& log, std::atomic<bool>* cancel = nullptr, SpeedLive* live = nullptr);

// --- история и сравнение (без сети, под тестами) ---
std::wstring SpeedRow(const Speed& s);  // строка speed.tsv
bool ParseSpeedRow(const std::wstring& line, Speed& s);
std::vector<Speed> SpeedHistory();       // старые сначала
void SaveSpeed(const Speed& s);          // в конец истории, держим последние 200
std::wstring SpeedText(const Speed& s);  // «↓ 87 Мбит/с, ↑ 21, задержка 45 мс, через туннель (Финляндия)»
// «Через туннель обычно ↓ 80 / ↑ 20, напрямую ↓ 140 / ↑ 35 (медиана последних замеров)»; пусто - сравнивать не с чем
std::wstring SpeedCompare(const std::vector<Speed>& history);
struct SpeedMedian {
    double down = 0, up = 0;
    int n = 0;  // сколько замеров вошло (последние 20 этого вида)
};
SpeedMedian SpeedMedianOf(const std::vector<Speed>& history, bool tunnel);
// Замеры по дням (местное время), в каждом дне отдельно через туннель и напрямую - медианы дня; последние days
// дней, где замеры были, старые сначала
struct SpeedDay {
    int year = 0, month = 0, day = 0;
    bool tunnel = false;
    double down = 0, up = 0;
    int pingMs = -1, n = 0;
};
std::vector<SpeedDay> SpeedDays(const std::vector<Speed>& history, size_t days = 14);
// Выходы туннеля (О3): страна из via («happ-xray, Швеция» - после последней запятой), медианы последних 20 замеров
// каждой; быстрые первыми. Без страны и не через туннель - не в счёт
struct SpeedExit {
    std::wstring country;
    double down = 0, up = 0;
    int pingMs = -1, n = 0;
};
std::vector<SpeedExit> SpeedByExit(const std::vector<Speed>& history);
// «Быстрее всего через Швецию: ↓ 61 против ↓ 45 у Нидерландов»; пусто - сравнивать не с чем (меньше двух стран)
std::wstring SpeedExitAdvice(const std::vector<SpeedExit>& exits);

}  // namespace net
