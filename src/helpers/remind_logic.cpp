// Напоминания: файл строками, подпись из буфера, часы для кольца (под тестами).
#include <ctime>

#include "core/text.h"
#include "helpers/remind.h"

namespace remind {

std::wstring Save(const std::vector<Note>& list) {
    std::wstring out;
    for (const Note& n : list) {
        std::wstring t = n.text;
        for (wchar_t& c : t)
            if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        out += std::to_wstring(n.when) + L"\t" + t + L"\n";
    }
    return out;
}

std::vector<Note> Parse(const std::wstring& tsv) {
    std::vector<Note> out;
    size_t at = 0;
    while (at < tsv.size()) {
        size_t e = tsv.find(L'\n', at);
        std::wstring line = tsv.substr(at, e == std::wstring::npos ? std::wstring::npos : e - at);
        at = e == std::wstring::npos ? tsv.size() : e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos || tab == 0) continue;
        Note n;
        n.when = _wtoi64(line.substr(0, tab).c_str());
        n.text = line.substr(tab + 1);
        if (n.when > 0) out.push_back(n);
    }
    return out;
}

std::wstring Caption(const std::wstring& clip) {
    std::wstring t = Trim(clip);
    size_t nl = t.find_first_of(L"\r\n");
    if (nl != std::wstring::npos) t = Trim(t.substr(0, nl));
    for (wchar_t& c : t)
        if (c == L'\t') c = L' ';
    if (t.size() > 80) t = t.substr(0, 79) + L"…";
    return t;
}

std::vector<int> Hours(int hour, int minute) {
    int h1 = (minute >= 30 ? hour + 2 : hour + 1) % 24;  // до ровного часа меньше 30 минут - он уже ни к чему
    // второй - вечер (18:00, 21:00) или утро; после первого, если смотреть от сейчас
    auto ahead = [hour](int h) { return (h - hour + 24) % 24; };
    int h2 = -1;
    for (int c : {18, 21, 9})  // ближайший из них после первого
        if (ahead(c) > ahead(h1) && (h2 < 0 || ahead(c) < ahead(h2))) h2 = c;
    if (h2 < 0) h2 = (h1 + 1) % 24;
    return {h1, h2};
}

std::wstring Clock(long long when, long long now) {
    time_t a = (time_t)when, b = (time_t)now;
    tm w{}, n{};
    localtime_s(&w, &a);
    localtime_s(&n, &b);
    wchar_t s[40];
    bool today = w.tm_year == n.tm_year && w.tm_yday == n.tm_yday;
    swprintf(s, 40, L"%s%02d:%02d", today ? L"" : L"завтра ", w.tm_hour, w.tm_min);
    return s;
}

}  // namespace remind
