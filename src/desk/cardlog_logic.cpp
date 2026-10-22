// Память карточек-вопросов: вид по заголовку, счёт закрытий без действия, файл заглушённых (без окон, под тестами).
#include <cwctype>

#include "desk/cardlog.h"

namespace cardlog {

std::wstring KindOf(const std::wstring& title) {
    size_t end = 0;
    while (end < title.size() && !iswdigit(title[end]) && title[end] != L'~' && title[end] != L'\n') ++end;
    std::wstring k = title.substr(0, end);
    while (!k.empty() && (k.back() == L' ' || k.back() == L':' || k.back() == L'-' || k.back() == L'«' ||
                          k.back() == L'(' || k.back() == L','))
        k.pop_back();
    return k.empty() ? title : k;
}

bool Strikes::Closed(const std::wstring& kind, bool acted) {
    if (acted) {
        n.erase(kind);
        return false;
    }
    return ++n[kind] == 3;
}

std::wstring MutesText(const std::set<std::wstring>& kinds) {
    std::wstring s;
    for (const std::wstring& k : kinds) s += k + L"\n";
    return s;
}

std::set<std::wstring> ParseMutes(const std::wstring& text) {
    std::set<std::wstring> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring k = text.substr(at, end - at);
        if (!k.empty() && k.back() == L'\r') k.pop_back();
        if (!k.empty()) out.insert(k);
        at = end + 1;
    }
    return out;
}

}  // namespace cardlog
