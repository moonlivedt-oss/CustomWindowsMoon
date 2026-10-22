#pragma once
// Wi-Fi (И27): сигнал и канал текущей сети, соседние сети на том же канале (и перекрывающихся в 2,4 ГГц), пароль
// сохранённого профиля и QR-код для телефона. Native Wifi API (wlanapi), без прав; блокирующее - из рабочего потока.
#include <windows.h>

#include <string>
#include <vector>

namespace net {

struct WifiNet {
    std::wstring ssid;
    int signal = 0;   // качество 0-100
    int rssi = 0;     // дБм
    int channel = 0;  // 0 - не знаем
    bool band5 = false;
    bool open = false;
};

struct WifiInfo {
    bool ok = false;  // есть беспроводной адаптер и он подключён
    // Windows 11 отдаёт сведения о Wi-Fi (сеть, сигнал, соседи) только при включённом определении
    // местоположения: выключено - адаптер есть, а ответ «отказано в доступе»
    bool locationOff = false;
    std::wstring adapter;
    WifiNet now;                // к чему подключены
    std::vector<WifiNet> same;  // соседи на том же канале или перекрывающихся (2,4 ГГц: ближе 5 каналов)
    std::vector<WifiNet> all;   // всё, что слышно (без своей сети), сильные сначала
    std::wstring advice;        // «на 6-м канале ещё 4 сети - на роутере лучше 1 или 11» и т.п.
};

WifiInfo Wifi(bool scan = true);     // scan - сначала попросить свежий список (до ~4 с)
int WifiChannel(ULONG centerKHz);    // частота центра в кГц -> номер канала (2,4 / 5 / 6 ГГц)
bool ChannelsOverlap(int a, int b);  // 2,4 ГГц: каналы ближе 5 мешают друг другу; 5 ГГц - только тот же
// Пароль сохранённой сети (пусто - не отдали или сеть открытая) и QR-картинка «подключиться» в PNG.
// known - такой профиль Wi-Fi вообще есть
std::wstring WifiPassword(const std::wstring& ssid, bool* open = nullptr, bool* known = nullptr);
// Имя подключённой сети без доступа к местоположению: подключённые сети Windows (Network List Manager), у
// которых есть профиль Wi-Fi с тем же именем; пусто - не нашлось
std::wstring WifiProfileNow();
bool WifiQrPng(const std::wstring& ssid, const std::wstring& password, bool open, const std::wstring& path);

}  // namespace net
