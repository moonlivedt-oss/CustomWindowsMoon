#pragma once
// Время в режимах (О3): сколько за неделю шли игровой режим, режим разработки, фокус и показ экрана - по дням.
// Каждое выключение режима дописывает строку «ГГГГ-ММ-ДД<TAB>режим<TAB>секунды» в %APPDATA%\MoonDesk\modes.tsv
// (режим через полночь делится по дням); идущий сейчас режим добавляется к итогу на лету. Видно на «Состоянии» и
// в недельном отчёте. modestats_logic.cpp - деление по дням, разбор и итог (под тестами).
#include <windows.h>

#include <string>
#include <vector>

namespace modestats {

enum Mode { GAME, DEV, FOCUS, SHARE, COUNT };

// --- без окон и файлов (под тестами) ---
struct Row {
    int day = 0;  // дней от 1970-01-01 по местному времени
    int mode = 0;
    LONGLONG sec = 0;
};
int DayOf(LONGLONG localSec);         // местные секунды от 1970 -> номер дня
std::wstring DayText(int day);        // «2026-10-25»
int ParseDay(const std::wstring& s);  // обратно; -1 - не дата
// Промежуток [from, to) местного времени - по дням
std::vector<Row> Split(LONGLONG localFrom, LONGLONG localTo, int mode);
std::wstring RowLine(const Row& r);
bool ParseRow(const std::wstring& line, Row& r);

struct Week {
    int firstDay = 0;          // номер первого из семи дней (сегодня - 6)
    LONGLONG sec[7][COUNT]{};  // по дням
    LONGLONG total[COUNT]{};   // за неделю
};
Week Sum(const std::vector<Row>& rows, int today);
const wchar_t* Name(int mode);     // «игра», «разработка», «фокус», «показ»
std::wstring Hours(LONGLONG sec);  // «9 ч», «1 ч 20 мин», «40 мин», «меньше минуты»
std::wstring Line(const Week& w);  // «игра 9 ч, разработка 14 ч, фокус 6 ч»; пусто - режимы не шли

// --- в программе ---
LONGLONG LocalNow();   // местные секунды от 1970
void Begin(int mode);  // режим включился
void End(int mode);    // выключился: дописать строку(и) в modes.tsv
Week Now();            // за 7 дней, с идущими сейчас
Week Demo();           // для снимков (RADIAL_DEMO)

}  // namespace modestats
