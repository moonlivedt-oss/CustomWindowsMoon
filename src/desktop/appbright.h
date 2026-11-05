#pragma once
// Яркость по программе (Ж7): впереди программа из правил - яркость экрана сдвигается (Figma и видео ярче, чтение
// темнее), ушли в другую - возвращается, как была. Правила - bright_apps в radial.cfg: «figma.exe=+20|vlc.exe=+25|
// sumatrapdf.exe=-15» (+N/-N - от вашей нынешней яркости, N - ровно столько). Поправили яркость руками, пока
// программа впереди, - при уходе поправка запоминается для неё (%APPDATA%\MoonDesk\appbright.tsv). Выключатель -
// bright_by_app. Экран ноутбука - WMI, внешний - DDC/CI (desktop/brightness), всё в своём потоке. В игре не трогается.
// appbright_logic.cpp - правила и решения (под тестами).
#include <windows.h>

#include <map>
#include <string>

struct Config;

namespace appbright {

void Init(const Config* cfg, bool (*paused)());
void Tick();
void Shutdown();

// --- чистое ---
struct Rule {
    int value = 0;
    bool relative = true;  // +N/-N от исходной яркости
};
std::map<std::wstring, Rule> ParseRules(const std::wstring& text);  // ключи - exe строчными
std::wstring SaveRules(const std::map<std::wstring, Rule>& rules);
int Target(int base, const Rule& r);  // 0..100

// Решения: какую яркость ставить при смене программы впереди. Без потоков и WMI.
class Planner {
public:
    std::map<std::wstring, Rule> rules;
    // впереди exe, сейчас яркость cur; -1 - ничего не делать, иначе поставить столько
    int OnApp(const std::wstring& exe, int cur);
    // поправку руками запомнить: уходим из active, а яркость не та, что ставили; true - правило поменялось
    bool Learn(int cur);
    const std::wstring& Active() const { return active_; }

private:
    std::wstring active_;
    int base_ = -1, set_ = -1;
};

}  // namespace appbright
