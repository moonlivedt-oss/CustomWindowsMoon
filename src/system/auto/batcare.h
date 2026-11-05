#pragma once
// Забота о батарее (Ж1, вместе с Л37): раз в 5 минут считается, сколько часов в день ноутбук стоит от сети с
// зарядом 95% и выше (%APPDATA%\MoonDesk\batcare.tsv, 14 дней). Три дня из последних пяти по 6 часов и больше -
// карточка «Весь день на зарядке - держать 80%?». У ASUS предел ставится задачей с правами (JOB_CHARGE_LIMIT, ATK
// WMI; первый раз Windows спросит разрешение) и после каждого запуска ставится заново - ASUS сбрасывает его при
// перезагрузке. Не ASUS - карточка открывает настройки питания. Выбор - %APPDATA%\MoonDesk\batcare.ini (limit=80,
// limit=0 - снять); отказ - вопрос не раньше чем через 30 дней. Сбережённое - «часов у предела вместо 100%» за
// неделю - Status() (--batcare-diag).
// batcare_logic.cpp - учёт по дням и решение (под тестами).
#include <windows.h>

#include <map>
#include <string>

struct Config;

namespace batcare {

void Init(HINSTANCE inst, const Config* cfg);
void Tick();
void Shutdown();
std::wstring Status();  // «на 100% от сети: 9 ч в день; предел 80%»

// --- чистое ---
struct Day {
    double fullHours = 0;   // от сети, заряд от 95%
    double limitHours = 0;  // от сети, у предела (предел -5..+1) при включённом пределе
};
std::map<std::wstring, Day> Parse(const std::wstring& tsv);  // «2026-10-25\t9.5\t0»
std::wstring Save(const std::map<std::wstring, Day>& days, size_t keep = 14);
bool ShouldOffer(const std::map<std::wstring, Day>& days);  // 3 из последних 5 дней - от 6 часов
double SavedHours(const std::map<std::wstring, Day>& days, size_t lastDays = 7);

}  // namespace batcare
