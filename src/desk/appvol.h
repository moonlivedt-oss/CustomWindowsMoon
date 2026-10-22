#pragma once
// Громкость по программам (Л36): уровень, который вы поставили программе в микшере (Discord тише, игра громче),
// запоминается по имени exe и возвращается, когда она запускается снова. Windows помнит это не для всех: у
// браузеров, игр и программ с новым сеансом на каждый запуск уровень сбрасывается. Свой поток раз в 4 с смотрит
// звуковые сеансы устройства по умолчанию; уровни - %APPDATA%\MoonDesk\appvol.tsv.
// appvol_logic.cpp - решение по снимку сеансов (под тестами), appvol.cpp - сеансы WASAPI и поток.
#include <windows.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace appvol {

struct SessionNow {
    DWORD pid = 0;
    std::wstring exe;  // строчными, без .exe
    float level = 1;
};
struct Plan {
    std::vector<std::pair<DWORD, float>> set;  // кому вернуть запомненный уровень
    std::map<std::wstring, float> save;        // что вы поменяли - запомнить
};
// last - уровень каждого сеанса на прошлом проходе (pid -> уровень): обновляется здесь, ушедшие убираются.
// Новый сеанс - вернуть ему запомненное (first - первый проход после старта: только запомнить, что есть);
// знакомый поменял уровень - это сделали вы, запомнить.
Plan PlanVolumes(const std::vector<SessionNow>& now, std::map<DWORD, float>& last,
                 const std::map<std::wstring, float>& saved, bool first);
std::wstring SavedText(const std::map<std::wstring, float>& saved);  // «exe<TAB>0.35» строками
std::map<std::wstring, float> ParseSaved(const std::wstring& text);

void Start(bool enabled);
void SetEnabled(bool on);
void Stop();
std::map<std::wstring, float> Saved();  // для студии
std::vector<SessionNow> Current();      // звуковые сеансы сейчас (--appvol-diag)
void Forget(const std::wstring& exe);

}  // namespace appvol
