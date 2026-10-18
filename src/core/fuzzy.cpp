#include "core/fuzzy.h"

#include <cwchar>

namespace fuzzy {

namespace {

bool WordStart(const std::wstring& s, size_t at) {
    if (at == 0) return true;
    wchar_t p = s[at - 1];
    return p == L' ' || p == L'-' || p == L'_' || p == L'.' || p == L'(' || p == L'/' || p == L'\\' || p == L':';
}

int Exact(const std::wstring& name, const std::wstring& q) {
    size_t at = name.find(q);
    if (at == std::wstring::npos) return -1;
    if (at == 0) return 0;
    for (; at != std::wstring::npos; at = name.find(q, at + 1))
        if (WordStart(name, at)) return 1;
    return 2;
}

// буквы запроса по порядку; 3 - все на началах слов (аббревиатура), 4 - как попало
int Subseq(const std::wstring& name, const std::wstring& q) {
    if (q.size() < 2) return -1;  // одна буква с пропусками совпадает почти со всем
    size_t at = 0;
    bool starts = true;
    for (wchar_t ch : q) {
        if (ch == L' ') continue;
        // сначала ищем ближайшее начало слова с этой буквой, иначе любую
        size_t best = std::wstring::npos;
        for (size_t i = at; i < name.size(); ++i)
            if (name[i] == ch && WordStart(name, i)) {
                best = i;
                break;
            }
        if (best == std::wstring::npos) {
            best = name.find(ch, at);
            starts = false;
        }
        if (best == std::wstring::npos) return -1;
        at = best + 1;
    }
    return starts ? 3 : 4;
}

int One(const std::wstring& name, const std::wstring& q) {
    int s = Exact(name, q);
    return s >= 0 ? s : Subseq(name, q);
}

}  // namespace

std::wstring OtherLayout(const std::wstring& q) {
    static const wchar_t en[] = L"qwertyuiop[]asdfghjkl;'zxcvbnm,.`";
    static const wchar_t ru[] =
        L"йцукенгшщзхъфывап"
        L"ролджэячсмитьбюё";
    std::wstring out = q;
    for (wchar_t& c : out) {
        if (!c) continue;
        if (const wchar_t* p = wcschr(en, c))
            c = ru[p - en];
        else if (const wchar_t* r = wcschr(ru, c))
            c = en[r - ru];
    }
    return out;
}

int Score(const std::wstring& name, const std::wstring& q) {
    if (q.empty()) return -1;
    int a = One(name, q), b = One(name, OtherLayout(q));
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}

}  // namespace fuzzy
