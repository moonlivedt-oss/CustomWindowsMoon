#pragma once
// Правила «если - то» (И28): строки rule = условие -> действие, действие в [settings] radial.cfg.
// Условия: app:маска (запущена программа, маски через +), ac (на зарядке), battery (от батареи), battery<30 (заряд
// ниже), time:23:00-07:00 (по часам, через полночь можно), weekend; несколько через «&» - все сразу.
// Действия: power:eco|balanced|perf (режим питания), volume:30 (общая громкость, %), mute, theme:<id> (тема колец),
// run:<действие угла> (один раз при входе в правило). Пока правило держится - его режим, тема и громкость; правило
// кончилось - возвращается то, что было до первого сработавшего. Первое подходящее правило в списке главнее.
#include <string>
#include <vector>

struct Config;

namespace rules {

struct Rule {
    std::wstring text;               // как написано (для журнала)
    std::vector<std::wstring> when;  // условия
    std::wstring power, theme, run;  // пусто - не трогать
    int volume = -1;                 // 0..100, -1 - не трогать
    bool mute = false;
};

struct Facts {
    std::vector<std::wstring> procs;  // имена exe
    bool ac = true;
    int battery = 100;  // %
    int minutes = 0;    // местное время, минуты от полуночи
    bool weekend = false;
};

std::vector<Rule> Parse(const std::vector<std::wstring>& lines, std::wstring* bad = nullptr);
bool Holds(const Rule& r, const Facts& f);

// Основная программа: раз в 3 с (из общего опроса). onTheme - сменить тему колец на время (пусто - вернуть свою).
void Tick(const Config& cfg, void (*onTheme)(const std::wstring& theme));
std::wstring ActiveText();  // какое правило держится сейчас (для студии), пусто - никакое
void Release();             // при выходе и «Вернуть всё»: вернуть режим и громкость

}  // namespace rules
