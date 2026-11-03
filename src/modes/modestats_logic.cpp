// Время в режимах: деление по дням, разбор строк, итог за неделю.
#include <algorithm>
#include <cwchar>

#include "modes/modestats.h"

namespace modestats {

namespace {

// дни от 1970-01-01 <-> год, месяц, день (по Говарду Хиннанту, без часовых поясов)
int DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void CivilFromDays(int z, int& y, int& m, int& d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}

}  // namespace

int DayOf(LONGLONG localSec) { return (int)(localSec >= 0 ? localSec / 86400 : (localSec - 86399) / 86400); }

std::wstring DayText(int day) {
    int y, m, d;
    CivilFromDays(day, y, m, d);
    wchar_t b[16];
    swprintf(b, 16, L"%04d-%02d-%02d", y, m, d);
    return b;
}

int ParseDay(const std::wstring& s) {
    int y = 0, m = 0, d = 0;
    if (s.size() != 10 || swscanf(s.c_str(), L"%4d-%2d-%2d", &y, &m, &d) != 3) return -1;
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    return DaysFromCivil(y, m, d);
}

std::vector<Row> Split(LONGLONG from, LONGLONG to, int mode) {
    std::vector<Row> out;
    while (from < to) {
        int day = DayOf(from);
        LONGLONG end = std::min(to, (LONGLONG)(day + 1) * 86400);
        out.push_back({day, mode, end - from});
        from = end;
    }
    return out;
}

std::wstring RowLine(const Row& r) {
    return DayText(r.day) + L"\t" + std::to_wstring(r.mode) + L"\t" + std::to_wstring(r.sec);
}

bool ParseRow(const std::wstring& line, Row& r) {
    size_t a = line.find(L'\t'), b = a == std::wstring::npos ? a : line.find(L'\t', a + 1);
    if (b == std::wstring::npos) return false;
    int day = ParseDay(line.substr(0, a));
    int mode = _wtoi(line.substr(a + 1, b - a - 1).c_str());
    LONGLONG sec = _wtoi64(line.c_str() + b + 1);
    if (day < 0 || mode < 0 || mode >= COUNT || sec <= 0) return false;
    r = {day, mode, sec};
    return true;
}

Week Sum(const std::vector<Row>& rows, int today) {
    Week w;
    w.firstDay = today - 6;
    for (const Row& r : rows) {
        int i = r.day - w.firstDay;
        if (i < 0 || i > 6 || r.mode < 0 || r.mode >= COUNT) continue;
        w.sec[i][r.mode] += r.sec;
        w.total[r.mode] += r.sec;
    }
    return w;
}

const wchar_t* Name(int mode) {
    static const wchar_t* k[COUNT] = {L"игра", L"разработка", L"фокус", L"показ"};
    return mode >= 0 && mode < COUNT ? k[mode] : L"";
}

std::wstring Hours(LONGLONG sec) {
    LONGLONG min = sec / 60, h = min / 60;
    min %= 60;
    if (!h && !min) return L"меньше минуты";
    if (!h) return std::to_wstring(min) + L" мин";
    if (h >= 10 || !min) return std::to_wstring(h + (h >= 10 && min >= 30)) + L" ч";
    return std::to_wstring(h) + L" ч " + std::to_wstring(min) + L" мин";
}

std::wstring Line(const Week& w) {
    std::wstring s;
    for (int m = 0; m < COUNT; ++m)
        if (w.total[m] >= 60) s += (s.empty() ? L"" : L", ") + std::wstring(Name(m)) + L" " + Hours(w.total[m]);
    return s;
}

}  // namespace modestats
