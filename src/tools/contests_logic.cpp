// Сроки из ContestReminder: разбор contests.json и выбор ближайших (без файлов, под тестами).
#include <algorithm>
#include <cwctype>
#include <ctime>

#include "tools/contests.h"

namespace tools {

namespace {

// строковое значение поля "key" в объекте [from, to): с экранированием \" \\ \n \uXXXX
bool Str(const std::wstring& j, const wchar_t* key, size_t from, size_t to, std::wstring& out) {
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t at = j.find(k, from);
    if (at == std::wstring::npos || at >= to) return false;
    size_t p = j.find(L':', at + k.size());
    while (p != std::wstring::npos && p + 1 < to && iswspace(j[p + 1])) ++p;
    if (p == std::wstring::npos || p + 1 >= to || j[p + 1] != L'"') return false;  // null или не строка
    out.clear();
    for (size_t i = p + 2; i < to; ++i) {
        wchar_t c = j[i];
        if (c == L'"') return true;
        if (c != L'\\' || i + 1 >= to) {
            out += c;
            continue;
        }
        wchar_t e = j[++i];
        if (e == L'u' && i + 4 < to) {
            out += (wchar_t)wcstol(j.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
        } else {
            out += e == L'n' ? L'\n' : e == L't' ? L'\t' : e;
        }
    }
    return false;
}

bool Bool(const std::wstring& j, const wchar_t* key, size_t from, size_t to) {
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t at = j.find(k, from);
    if (at == std::wstring::npos || at >= to) return false;
    size_t p = j.find_first_not_of(L" :\t\r\n", at + k.size());
    return p != std::wstring::npos && p < to && j.compare(p, 4, L"true") == 0;
}

// «2026-10-25T23:59:00» - местное время
LONGLONG Iso(const std::wstring& s) {
    tm t{};
    if (swscanf(s.c_str(), L"%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) <
        5)
        return 0;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_isdst = -1;
    time_t v = mktime(&t);
    return v == (time_t)-1 ? 0 : (LONGLONG)v;
}

}  // namespace

std::vector<Contest> ParseContests(const std::wstring& j) {
    std::vector<Contest> out;
    size_t at = j.find(L'[');
    if (at == std::wstring::npos) return out;
    // объекты верхнего уровня массива: по скобкам, кавычки учитываем
    int depth = 0;
    bool str = false;
    size_t start = 0;
    for (size_t i = at + 1; i < j.size(); ++i) {
        wchar_t c = j[i];
        if (str) {
            if (c == L'\\')
                ++i;
            else if (c == L'"')
                str = false;
            continue;
        }
        if (c == L'"')
            str = true;
        else if (c == L'{' && depth++ == 0)
            start = i;
        else if (c == L'}' && --depth == 0) {
            Contest k;
            std::wstring dl;
            Str(j, L"title", start, i, k.title);
            Str(j, L"url", start, i, k.url);
            if (Str(j, L"deadline", start, i, dl)) k.deadline = Iso(dl);
            k.done = Bool(j, L"done", start, i) || Bool(j, L"auto_closed", start, i);
            if (!k.title.empty() && k.deadline) out.push_back(k);
        } else if (c == L']' && depth == 0) {
            break;
        }
    }
    return out;
}

std::vector<Contest> SoonContests(const std::vector<Contest>& all, LONGLONG now, int days) {
    std::vector<Contest> v;
    for (const Contest& c : all)
        if (!c.done && c.deadline > now && c.deadline - now < (LONGLONG)days * 86400) v.push_back(c);
    std::sort(v.begin(), v.end(), [](const Contest& a, const Contest& b) { return a.deadline < b.deadline; });
    return v;
}

std::wstring DeadlineLeft(LONGLONG deadline, LONGLONG now) {
    LONGLONG s = deadline - now;
    if (s <= 0) return L"срок вышел";
    if (s < 3600) return L"через " + std::to_wstring(std::max<LONGLONG>(1, s / 60)) + L" мин";
    if (s < 86400) return L"через " + std::to_wstring(s / 3600) + L" ч";
    return L"через " + std::to_wstring(s / 86400) + L" дн." +
           (s % 86400 >= 3600 ? L" " + std::to_wstring(s % 86400 / 3600) + L" ч" : L"");
}

}  // namespace tools
