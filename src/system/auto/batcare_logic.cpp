// Забота о батарее: учёт часов на 100% по дням и решение, пора ли предлагать предел (под тестами).
#include "core/text.h"
#include "system/auto/batcare.h"

namespace batcare {

std::map<std::wstring, Day> Parse(const std::wstring& tsv) {
    std::map<std::wstring, Day> out;
    for (const std::wstring& line : Split(tsv, L'\n')) {
        std::vector<std::wstring> f = Split(line, L'\t');
        if (f.size() < 2 || f[0].size() != 10) continue;
        Day d;
        d.fullHours = _wtof(f[1].c_str());
        if (f.size() > 2) d.limitHours = _wtof(f[2].c_str());
        out[f[0]] = d;
    }
    return out;
}

std::wstring Save(const std::map<std::wstring, Day>& days, size_t keep) {
    std::wstring s;
    size_t skip = days.size() > keep ? days.size() - keep : 0;
    for (const auto& [day, d] : days) {
        if (skip) {
            --skip;
            continue;
        }
        wchar_t b[96];
        swprintf(b, 96, L"%ls\t%.2f\t%.2f\n", day.c_str(), d.fullHours, d.limitHours);
        s += b;
    }
    return s;
}

bool ShouldOffer(const std::map<std::wstring, Day>& days) {
    int seen = 0, heavy = 0;
    for (auto it = days.rbegin(); it != days.rend() && seen < 5; ++it, ++seen) heavy += it->second.fullHours >= 6;
    return heavy >= 3;
}

double SavedHours(const std::map<std::wstring, Day>& days, size_t lastDays) {
    double h = 0;
    size_t n = 0;
    for (auto it = days.rbegin(); it != days.rend() && n < lastDays; ++it, ++n) h += it->second.limitHours;
    return h;
}

}  // namespace batcare
