#pragma once
// Чем пользуются: для каждой возможности, которую включают руками (кольцо окон, палитра, двойной клик по столу...), -
// день последнего раза и сколько раз всего. Студия (Состояние - «Чем не пользуетесь») по этому предлагает выключить
// то, что давно лежит без дела: программа проще, а вернуть можно тем же переключателем.
// Счёт идёт в памяти (Hit можно звать из любого потока, в том числе из перехвата мыши - он не пишет на диск), в
// %APPDATA%\MoonDesk\usage.tsv его сбрасывает Flush из общего опроса и при выходе.
// Судить начинаем, когда счёт ведётся хотя бы idleDays дней: иначе «ни разу» значит лишь «ещё не успели».
#include <map>
#include <string>
#include <vector>

namespace usage {

// --- без окон и файлов (под тестами) ---
struct Rec {
    int last = 0;   // день последнего раза (дни от 1970-01-01, местное время)
    int count = 0;  // сколько раз всего
};
struct Data {
    int since = 0;  // с какого дня ведётся счёт
    std::map<std::wstring, Rec> rec;
};
std::wstring Text(const Data& d);
Data Parse(const std::wstring& text);

struct Idle {
    std::wstring key;
    int days = -1;  // сколько дней без дела; -1 - ни разу за всё время счёта
};
// Включённые возможности (enabled - их ключи), которыми не пользовались idleDays дней и больше. Пусто, пока счёт
// ведётся меньше idleDays дней. Сначала самые давние и ни разу не тронутые.
std::vector<Idle> Unused(const Data& d, const std::vector<std::wstring>& enabled, int today, int idleDays = 14);
int TrackedDays(const Data& d, int today);  // сколько дней ведётся счёт

// --- в программе ---
int Today();
void Hit(const wchar_t* key);  // возможностью воспользовались
void Flush();                  // записать, если что-то поменялось
Data Snapshot();

}  // namespace usage
