#pragma once
// Выход VPN с лучшим пингом (Ж3) - дополнение проверки игры в «Сети» (net::CheckGame): игра идёт через туннель -
// пинг до её сервера через нынешний выход записывается по стране выхода (%APPDATA%\MoonDesk\vpnexit.tsv); если через
// другую страну для этой игры было заметно быстрее - карточка подскажет её выбрать в Happ. Рядом - сколько было бы
// напрямую через Wi-Fi (соединение с адреса Wi-Fi мимо туннеля, если клиент это пускает) и какие программы Happ уже
// пускает мимо VPN (правила direct по именам процессов в %LOCALAPPDATA%\Happ\config.json). Список серверов
// подписки Happ хранит зашифрованным - перебрать все страны заранее нельзя, учёт копится сам от игры к игре.
// vpnexit_logic.cpp - разбор и выбор (под тестами).
#include <string>
#include <vector>

namespace vpnexit {

struct Sample {
    std::wstring game, country;
    int ms = 0;
    long long when = 0;  // unix
};

// --- в программе (из фонового потока) ---
std::vector<std::wstring> Bypass();                              // мимо VPN в Happ: имена процессов
int DirectRtt(const std::wstring& ip, int port, int timeoutMs);  // TCP с адреса Wi-Fi/провода; -1 - не вышло
// записать замер и вернуть подсказку: «через Германию было 62 мс - выберите её в Happ» / пусто
std::wstring Note(const std::wstring& game, const std::wstring& country, int ms);

// --- чистое ---
std::vector<std::wstring> ParseBypass(const std::string& singboxJson);
std::vector<Sample> Parse(const std::wstring& tsv);
std::wstring Save(const std::vector<Sample>& list, size_t keep = 200);
// лучшая другая страна для игры: медиана её замеров за 30 дней на 25% и 20 мс лучше нынешнего ms; пусто - нет
std::wstring Better(const std::vector<Sample>& list, const std::wstring& game, const std::wstring& country, int ms,
                    long long now, int* betterMs = nullptr);

}  // namespace vpnexit
