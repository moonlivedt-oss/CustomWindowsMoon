#pragma once
// QR-код своими силами (для «Wi-Fi на телефон»): байтовый режим, коррекция M, версии 1-10 (до 213 байт).
// Без сторонних библиотек: Рида-Соломона, маски и служебные узоры - по спецификации ISO/IEC 18004.
#include <string>
#include <vector>

namespace qr {

struct Code {
    int size = 0;  // модулей по стороне (21 + 4 * (версия - 1)); 0 - текст не влез
    int version = 0, mask = 0;
    std::vector<bool> dark;  // size * size, построчно
    bool At(int x, int y) const { return dark[(size_t)y * size + x]; }
};

Code Encode(const std::string& bytes);  // UTF-8 как есть

// Строка для телефона: WIFI:T:WPA;S:<имя>;P:<пароль>;; - спецсимволы \ ; , : " экранируются
std::string WifiText(const std::string& ssid, const std::string& password, bool open);

}  // namespace qr
