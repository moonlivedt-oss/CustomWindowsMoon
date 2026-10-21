// Разбор и проверка правил «если - то» без окон, звука и процессов - под тестами (tests_more.cpp).
#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "app/rules.h"
#include "core/text.h"
#include "tools/guard.h"

namespace rules {

namespace {

std::wstring T(const std::wstring& s) { return Trim(s); }

std::vector<std::wstring> SplitBy(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        std::wstring part = T(s.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
        if (!part.empty()) out.push_back(part);
        a = b == std::wstring::npos ? s.size() + 1 : b + 1;
    }
    return out;
}

int Minutes(const std::wstring& hhmm) {
    size_t c = hhmm.find(L':');
    if (c == std::wstring::npos || c == 0 || c + 1 >= hhmm.size()) return -1;
    int h = _wtoi(hhmm.c_str()), m = _wtoi(hhmm.c_str() + c + 1);
    return h >= 0 && h < 24 && m >= 0 && m < 60 && iswdigit(hhmm[0]) && iswdigit(hhmm[c + 1]) ? h * 60 + m : -1;
}

bool ValidWhen(const std::wstring& w) {
    std::wstring l = Lower(w);
    if (l == L"ac" || l == L"battery" || l == L"weekend") return true;
    if (l.rfind(L"battery<", 0) == 0) return _wtoi(l.c_str() + 8) > 0;
    if (l.rfind(L"app:", 0) == 0) return l.size() > 4;
    if (l.rfind(L"time:", 0) == 0) {
        size_t d = l.find(L'-', 5);
        return d != std::wstring::npos && Minutes(l.substr(5, d - 5)) >= 0 && Minutes(l.substr(d + 1)) >= 0;
    }
    return false;
}

}  // namespace

std::vector<Rule> Parse(const std::vector<std::wstring>& lines, std::wstring* bad) {
    std::vector<Rule> out;
    for (const std::wstring& line : lines) {
        size_t arrow = line.find(L"->");
        Rule r;
        r.text = T(line);
        bool ok = arrow != std::wstring::npos;
        if (ok) {
            r.when = SplitBy(line.substr(0, arrow), L'&');
            ok = !r.when.empty() && std::all_of(r.when.begin(), r.when.end(), ValidWhen);
            for (const std::wstring& a : SplitBy(line.substr(arrow + 2), L',')) {
                std::wstring l = Lower(a);
                if (l.rfind(L"power:", 0) == 0 && (l == L"power:eco" || l == L"power:balanced" || l == L"power:perf"))
                    r.power = l.substr(6);
                else if (l.rfind(L"volume:", 0) == 0 && iswdigit(l.c_str()[7]))
                    r.volume = std::clamp(_wtoi(l.c_str() + 7), 0, 100);
                else if (l == L"mute")
                    r.mute = true;
                else if (l.rfind(L"theme:", 0) == 0 && l.size() > 6)
                    r.theme = a.substr(6);
                else if (l.rfind(L"run:", 0) == 0 && l.size() > 4)
                    r.run = a.substr(4);
                else
                    ok = false;
            }
            ok = ok && (!r.power.empty() || r.volume >= 0 || r.mute || !r.theme.empty() || !r.run.empty());
        }
        if (ok)
            out.push_back(r);
        else if (bad && !T(line).empty())
            *bad += (bad->empty() ? L"" : L"; ") + T(line);
    }
    return out;
}

bool Holds(const Rule& r, const Facts& f) {
    for (const std::wstring& w : r.when) {
        std::wstring l = Lower(w);
        bool ok = false;
        if (l == L"ac")
            ok = f.ac;
        else if (l == L"battery")
            ok = !f.ac;
        else if (l == L"weekend")
            ok = f.weekend;
        else if (l.rfind(L"battery<", 0) == 0)
            ok = !f.ac && f.battery < _wtoi(l.c_str() + 8);
        else if (l.rfind(L"app:", 0) == 0) {
            std::wstring masks = w.substr(4);
            std::replace(masks.begin(), masks.end(), L'+', L'|');
            ok = std::any_of(f.procs.begin(), f.procs.end(),
                             [&](const std::wstring& p) { return guard::Match(p, masks); });
        } else if (l.rfind(L"time:", 0) == 0) {
            size_t d = l.find(L'-', 5);
            int a = Minutes(l.substr(5, d - 5)), b = Minutes(l.substr(d + 1));
            ok = a <= b ? f.minutes >= a && f.minutes < b : f.minutes >= a || f.minutes < b;  // через полночь
        }
        if (!ok) return false;
    }
    return true;
}

}  // namespace rules
