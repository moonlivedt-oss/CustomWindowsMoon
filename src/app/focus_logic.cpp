// Режим фокуса: совпадение заголовков окон и строка отсчёта (без окон, под тестами).
#include <cwctype>

#include "app/focus.h"

namespace focus {

namespace {

std::wstring Low(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

}  // namespace

std::wstring AutoMatch(const std::vector<std::wstring>& titles, const std::wstring& masks) {
    size_t at = 0;
    while (at < masks.size()) {
        size_t bar = masks.find(L'|', at);
        std::wstring m = Low(masks.substr(at, bar == std::wstring::npos ? std::wstring::npos : bar - at));
        while (!m.empty() && m.back() == L' ') m.pop_back();
        while (!m.empty() && m.front() == L' ') m.erase(0, 1);
        if (m.size() >= 3)
            for (const std::wstring& t : titles)
                if (Low(t).find(m) != std::wstring::npos) return m;
        if (bar == std::wstring::npos) break;
        at = bar + 1;
    }
    return L"";
}

std::wstring LeftText(ULONGLONG msLeft) {
    if (msLeft < 60000) return L"Фокус: меньше минуты";
    return L"Фокус: ещё " + std::to_wstring((msLeft + 59999) / 60000) + L" мин";
}

}  // namespace focus
