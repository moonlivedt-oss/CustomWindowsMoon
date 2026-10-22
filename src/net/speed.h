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

// Замер целиком (~20 с): задержка, загрузка (до 10 с), отдача (до 8 с). Блокирующий - из рабочего потока.
Speed MeasureSpeed(const Log& log, std::atomic<bool>* cancel = nullptr);

// --- история и сравнение (без сети, под тестами) ---
std::wstring SpeedRow(const Speed& s);  // строка speed.tsv
bool ParseSpeedRow(const std::wstring& line, Speed& s);
std::vector<Speed> SpeedHistory();       // старые сначала
void SaveSpeed(const Speed& s);          // в конец истории, держим последние 200
std::wstring SpeedText(const Speed& s);  // «↓ 87 Мбит/с, ↑ 21, задержка 45 мс, через туннель (Финляндия)»
// «Через туннель обычно ↓ 80 / ↑ 20, напрямую ↓ 140 / ↑ 35 (медиана последних замеров)»; пусто - сравнивать не с чем
std::wstring SpeedCompare(const std::vector<Speed>& history);

}  // namespace net
