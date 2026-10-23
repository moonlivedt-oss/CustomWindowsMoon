// Разбор вывода «winget upgrade» (без процессов, под тестами).
#include <cwctype>

#include "tools/winget.h"

namespace tools {

namespace {

std::vector<std::wstring> Words(const std::wstring& l) {
    std::vector<std::wstring> w;
    std::wstring cur;
    for (wchar_t c : l) {
        if (iswspace(c)) {
            if (!cur.empty()) w.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

}  // namespace

std::vector<Upgrade> ParseWingetUpgrade(const std::wstring& out) {
    std::vector<Upgrade> v;
    bool table = false;
    size_t at = 0;
    while (at < out.size()) {
        // строки делятся и по \r: ход загрузки источника перерисовывается на месте
        size_t end = out.find_first_of(L"\r\n", at);
        if (end == std::wstring::npos) end = out.size();
        std::wstring line = out.substr(at, end - at);
        at = end + 1;
        if (line.find(L"-----") != std::wstring::npos && line.find_first_not_of(L"- ") == std::wstring::npos) {
            table = true;  // под шапкой таблицы (их бывает две: вторая - пакеты, что обновляются только явно)
            continue;
        }
        std::vector<std::wstring> w = Words(line);
        if (!table || w.size() < 4) continue;
        // с конца: источник (winget, msstore), «доступно», версия (бывает «< 1.2»), ИД без пробелов, остальное - имя
        if (w.back() == L"winget" || w.back() == L"msstore") w.pop_back();
        if (w.size() < 4) continue;
        Upgrade u;
        u.avail = w.back(), w.pop_back();
        u.have = w.back(), w.pop_back();
        if (w.size() >= 3 && (w.back() == L"<" || w.back() == L">")) u.have = w.back() + L" " + u.have, w.pop_back();
        u.id = w.back(), w.pop_back();
        if (u.id.find(L'.') == std::wstring::npos || !iswdigit(u.avail[0])) continue;  // не строка пакета
        for (const std::wstring& x : w) u.name += (u.name.empty() ? L"" : L" ") + x;
        v.push_back(u);
    }
    return v;
}

}  // namespace tools
