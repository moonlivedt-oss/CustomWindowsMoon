// Кольцо выделенного, чистое: раскладка, калькулятор, регистр, пробелы, адрес для поиска.
#include <windows.h>

#include <cmath>
#include <cwchar>
#include <cwctype>

#include "app/selring.h"
#include "core/text.h"

namespace selring {

namespace {

// одни и те же клавиши в английской и русской раскладке (верхний ряд, средний, нижний, ё; потом с Shift; цифры
// с Shift, где знаки разные: в русской «?» - на Shift+7, «№» - на Shift+3)
const wchar_t kEn[] = L"qwertyuiop[]asdfghjkl;'zxcvbnm,./`QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?~@#$^&";
const wchar_t kRu[] = L"йцукенгшщзхъфывапролджэячсмитьбю.ёЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ,Ё\"№;:?";

bool IsCyr(wchar_t c) { return (c >= 0x0410 && c <= 0x044F) || c == 0x0401 || c == 0x0451; }
bool IsLat(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

// ---- разбор выражения ----
struct Parser {
    const std::wstring& s;
    size_t i = 0;
    bool ok = true;
    int ops = 0;
    void Skip() {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == 0xA0)) ++i;
    }
    bool Eat(wchar_t c) {
        Skip();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    double Number() {
        Skip();
        size_t a = i;
        std::wstring n;
        while (i < s.size() && (iswdigit(s[i]) || s[i] == L'.' || s[i] == L',')) n += s[i] == L',' ? L'.' : s[i], ++i;
        if (i == a || n == L".") {
            ok = false;
            return 0;
        }
        wchar_t* end = nullptr;
        double v = wcstod(n.c_str(), &end);
        if (*end) ok = false;
        return v;
    }
    double Primary() {
        Skip();
        if (Eat(L'(')) {
            double v = Expr();
            if (!Eat(L')')) ok = false;
            return v;
        }
        return Number();
    }
    double Unary() {
        if (Eat(L'-')) return ++ops, -Unary();
        if (Eat(L'+')) return Unary();
        return Primary();
    }
    double Power() {
        double v = Unary();
        if (Eat(L'^')) {
            ++ops;
            v = std::pow(v, Power());  // справа налево: 2^3^2 = 2^9
        }
        return v;
    }
    double Term() {
        double v = Power();
        for (;;) {
            if (Eat(L'*') || Eat(0x00D7))
                ++ops, v *= Power();
            else if (Eat(L'/') || Eat(0x00F7)) {
                ++ops;
                double d = Power();
                if (d == 0) ok = false;
                v = ok ? v / d : 0;
            } else if (Eat(L'%')) {
                ++ops;
                double d = Power();
                if (d == 0) ok = false;
                v = ok ? std::fmod(v, d) : 0;
            } else {
                return v;
            }
        }
    }
    double Expr() {
        double v = Term();
        for (;;) {
            if (Eat(L'+'))
                ++ops, v += Term();
            else if (Eat(L'-') || Eat(0x2212))
                ++ops, v -= Term();
            else
                return v;
        }
    }
};

}  // namespace

bool HasCyrillic(const std::wstring& s) {
    for (wchar_t c : s)
        if (IsCyr(c)) return true;
    return false;
}

std::wstring FixLayout(const std::wstring& s) {
    int cyr = 0, lat = 0;
    for (wchar_t c : s) cyr += IsCyr(c), lat += IsLat(c);
    bool toRu = lat >= cyr;
    const wchar_t* from = toRu ? kEn : kRu;
    const wchar_t* to = toRu ? kRu : kEn;
    std::wstring out = s;
    for (wchar_t& c : out)
        if (const wchar_t* p = wcschr(from, c); p && c) c = to[p - from];
    return out;
}

bool Calc(const std::wstring& s, double& out) {
    if (s.size() > 200) return false;
    Parser p{s};
    double v = p.Expr();
    p.Skip();
    if (p.i == s.size() - 1 && s.back() == L'=') ++p.i;  // «2+2=»
    if (!p.ok || p.i != s.size() || p.ops == 0 || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::wstring FormatNumber(double v, bool comma) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.10g", v);
    std::wstring r = buf;
    if (r == L"-0") r = L"0";
    if (comma)
        for (wchar_t& c : r)
            if (c == L'.') c = L',';
    return r;
}

std::wstring Sentence(const std::wstring& s) {
    std::wstring r = s;
    bool start = true;
    for (wchar_t& c : r) {
        wchar_t lo = (wchar_t)(UINT_PTR)CharLowerW((LPWSTR)(UINT_PTR)c);
        wchar_t up = (wchar_t)(UINT_PTR)CharUpperW((LPWSTR)(UINT_PTR)c);
        if (lo != up) {  // буква
            c = start ? up : lo;
            start = false;
        } else if (c == L'.' || c == L'!' || c == L'?' || c == L'\n') {
            start = true;
        }
    }
    return r;
}

std::wstring Squeeze(const std::wstring& s) {
    std::wstring r;
    int newlines = 0;
    bool space = false;
    for (wchar_t c : s) {
        if (c == L'\r') continue;
        if (c == L'\n') {
            while (!r.empty() && r.back() == L' ') r.pop_back();
            if (++newlines <= 2 && !r.empty()) r += L'\n';
            space = false;
            continue;
        }
        if (c == L' ' || c == L'\t' || c == 0xA0) {
            space = true;
            continue;
        }
        if (space && !r.empty() && r.back() != L'\n') r += L' ';
        space = false;
        newlines = 0;
        r += c;
    }
    while (!r.empty() && (r.back() == L'\n' || r.back() == L' ')) r.pop_back();
    return r;
}

std::wstring UrlEncode(const std::wstring& s) {
    std::string u = Narrow(s);
    std::wstring r;
    static const wchar_t hex[] = L"0123456789ABCDEF";
    for (unsigned char c : u) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            r += (wchar_t)c;
        else
            r += {L'%', hex[c >> 4], hex[c & 15]};
    }
    return r;
}

int Words(const std::wstring& s) {
    int n = 0;
    bool in = false;
    for (wchar_t c : s) {
        bool w = iswalnum(c) || IsCyr(c);
        if (w && !in) ++n;
        in = w;
    }
    return n;
}

}  // namespace selring
