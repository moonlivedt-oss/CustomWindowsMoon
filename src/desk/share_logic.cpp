// Показ экрана: что прятать в тексте и как узнать окно-полоску показа (без окон, под тестами).
#include <cwctype>

#include "core/text.h"
#include "desk/share.h"

namespace share {

namespace {

// заменить все вхождения без учёта регистра
void ReplaceI(std::wstring& s, const std::wstring& what, const std::wstring& with) {
    if (what.empty()) return;
    std::wstring low = Lower(s), w = Lower(what);
    std::wstring out;
    size_t from = 0;
    for (size_t at; (at = low.find(w, from)) != std::wstring::npos; from = at + w.size())
        out += s.substr(from, at - from) + with;
    if (!from) return;
    s = out + s.substr(from);
}

// IPv4 «a.b.c.d» на этом месте: длина; 0 - не адрес
size_t Ipv4At(const std::wstring& s, size_t i) {
    if (i > 0 && (iswdigit(s[i - 1]) || s[i - 1] == L'.')) return 0;
    size_t p = i;
    for (int part = 0; part < 4; ++part) {
        size_t d = 0;
        int v = 0;
        while (p < s.size() && iswdigit(s[p]) && d < 3) v = v * 10 + (s[p++] - L'0'), ++d;
        if (!d || v > 255) return 0;
        if (part < 3) {
            if (p >= s.size() || s[p] != L'.') return 0;
            ++p;
        }
    }
    if (p < s.size() && (iswdigit(s[p]) || (s[p] == L'.' && p + 1 < s.size() && iswdigit(s[p + 1])))) return 0;
    return p - i;
}

bool EmailChar(wchar_t c) { return iswalnum(c) || c == L'.' || c == L'_' || c == L'-' || c == L'+'; }

}  // namespace

std::wstring MaskText(std::wstring s, const std::vector<std::wstring>& profiles,
                      const std::vector<std::wstring>& words) {
    for (const std::wstring& p : profiles)
        if (p.size() > 3) ReplaceI(s, p, L"%USERPROFILE%");
    for (const std::wstring& w : words)
        if (w.size() >= 3) ReplaceI(s, w, L"***");
    // адреса IPv4, кроме своего компьютера (127.0.0.1) и «любого» (0.0.0.0)
    for (size_t i = 0; i < s.size(); ++i) {
        size_t n = iswdigit(s[i]) ? Ipv4At(s, i) : 0;
        if (!n) continue;
        std::wstring ip = s.substr(i, n);
        if (ip.rfind(L"127.", 0) == 0 || ip == L"0.0.0.0") {
            i += n - 1;
            continue;
        }
        s.replace(i, n, L"x.x.x.x");
        i += 6;
    }
    // почта: имя@домен.зона
    for (size_t at = s.find(L'@'); at != std::wstring::npos; at = s.find(L'@', at + 1)) {
        size_t a = at, b = at + 1;
        while (a > 0 && EmailChar(s[a - 1])) --a;
        while (b < s.size() && EmailChar(s[b])) ++b;
        std::wstring dom = s.substr(at + 1, b - at - 1);
        if (a == at || dom.find(L'.') == std::wstring::npos) continue;
        s.replace(a, b - a, L"***@***");
        at = a + 6;
    }
    return s;
}

bool TitleHit(const std::wstring& title, const std::wstring& masks) {
    if (title.empty()) return false;
    std::wstring low = Lower(title);
    for (const std::wstring& m : Split(masks, L'|'))
        if (!m.empty() && low.find(Lower(m)) != std::wstring::npos) return true;
    return false;
}

}  // namespace share
