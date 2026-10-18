#include "core/secrets.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>

namespace secrets {

namespace {

bool Starts(const std::wstring& s, const wchar_t* p) { return s.rfind(p, 0) == 0; }

bool AllOf(const std::wstring& s, int (*f)(wint_t)) {
    for (wchar_t c : s)
        if (!f(c)) return false;
    return !s.empty();
}

int IsHex(wint_t c) { return iswxdigit(c); }

bool TokenChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' ||
           c == L'.' || c == L'+' || c == L'/' || c == L'=';
}

std::wstring TrimWs(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// номер карты: 13-19 цифр, можно через пробелы и дефисы группами, контрольная сумма Луна
bool Card(const std::wstring& t) {
    std::wstring d;
    for (wchar_t c : t) {
        if (c >= L'0' && c <= L'9')
            d += c;
        else if (c != L' ' && c != L'-')
            return false;
    }
    return d.size() >= 13 && d.size() <= 19 && Luhn(d);
}

// GUID и хэши (sha коммита, sha256 файла) - часто копируют по делу, секретом не считаем
bool HashLike(const std::wstring& t) {
    if (t.size() == 36 && t[8] == L'-' && t[13] == L'-' && t[18] == L'-' && t[23] == L'-') return true;
    return AllOf(t, IsHex) && (t.size() == 32 || t.size() == 40 || t.size() == 64 || t.size() <= 12);
}

}  // namespace

bool Luhn(const std::wstring& digits) {
    int sum = 0;
    bool dbl = false;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (*it < L'0' || *it > L'9') return false;
        int v = *it - L'0';
        if (dbl && (v *= 2) > 9) v -= 9;
        sum += v;
        dbl = !dbl;
    }
    return !digits.empty() && sum % 10 == 0;
}

double Entropy(const std::wstring& s) {
    if (s.empty()) return 0;
    std::map<wchar_t, int> n;
    for (wchar_t c : s) ++n[c];
    double e = 0;
    for (auto& [c, k] : n) {
        double p = (double)k / s.size();
        e -= p * std::log2(p);
    }
    return e;
}

std::wstring Kind(const std::wstring& text) {
    std::wstring t = TrimWs(text);
    if (t.empty() || t.size() > 8000) return L"";
    if (t.find(L"-----BEGIN") != std::wstring::npos && t.find(L"PRIVATE KEY") != std::wstring::npos)
        return L"закрытый ключ";
    if (Card(t)) return L"номер карты";
    // пароль строкой «password: ...», «пароль = ...», «pwd=...»
    std::wstring low;
    for (wchar_t c : t) low += (wchar_t)towlower(c);
    for (const wchar_t* k : {L"password", L"пароль", L"passwd", L"pwd", L"secret", L"api_key", L"apikey", L"token"}) {
        size_t at = low.find(k);
        if (at == std::wstring::npos) continue;
        size_t after = at + wcslen(k);
        while (after < low.size() && (low[after] == L' ' || low[after] == L'"' || low[after] == L'\'')) ++after;
        if (after >= low.size() || (low[after] != L':' && low[after] != L'=')) continue;
        // значение - до конца строки, одним словом от 6 знаков (не вызов функции и не переменная из кода);
        // кусок кода в несколько строк паролем не считаем
        if (std::count(t.begin(), t.end(), L'\n') > 2) continue;
        std::wstring v = t.substr(after + 1);
        v = TrimWs(v.substr(0, v.find_first_of(L"\r\n")));
        while (!v.empty() && (v.back() == L';' || v.back() == L',' || v.back() == L'"' || v.back() == L'\''))
            v.pop_back();
        while (!v.empty() && (v[0] == L'"' || v[0] == L'\'')) v.erase(0, 1);
        if (v.size() >= 6 && v.find_first_of(L" ()[]{}") == std::wstring::npos) return L"пароль или ключ в строке";
    }
    if (t.find_first_of(L" \t\r\n") != std::wstring::npos) return L"";  // дальше - только одиночные токены
    if (Starts(t, L"Bearer")) return L"токен";
    struct Pre {
        const wchar_t* p;
        size_t min;
        const wchar_t* what;
    };
    static const Pre kPre[] = {
        {L"ghp_", 30, L"токен GitHub"},      {L"gho_", 30, L"токен GitHub"},
        {L"ghs_", 30, L"токен GitHub"},      {L"github_pat_", 40, L"токен GitHub"},
        {L"glpat-", 20, L"токен GitLab"},    {L"sk-ant-", 30, L"ключ Anthropic"},
        {L"sk-", 30, L"ключ API"},           {L"sk_live_", 20, L"ключ Stripe"},
        {L"rk_live_", 20, L"ключ Stripe"},   {L"xoxb-", 20, L"токен Slack"},
        {L"xoxp-", 20, L"токен Slack"},      {L"AKIA", 20, L"ключ AWS"},
        {L"AIza", 35, L"ключ Google"},       {L"ya29.", 30, L"токен Google"},
        {L"hf_", 30, L"токен Hugging Face"}, {L"npm_", 30, L"токен npm"},
        {L"pypi-", 40, L"токен PyPI"},       {L"dop_v1_", 40, L"токен DigitalOcean"},
    };
    for (const Pre& p : kPre)
        if (Starts(t, p.p) && t.size() >= p.min) return p.what;
    // JWT: три части base64url через точку, первые две - JSON ({"... -> eyJ)
    if (Starts(t, L"eyJ") && t.find(L'.') != std::wstring::npos &&
        t.find(L'.', t.find(L'.') + 1) != std::wstring::npos && t.size() > 40)
        return L"токен (JWT)";
    // похоже на сгенерированный ключ: 24-200 знаков без пробелов, есть строчные, заглавные и цифры, высокая
    // энтропия; пути, адреса, хэши и обычные слова сюда не попадают
    if (t.size() < 24 || t.size() > 200 || HashLike(t)) return L"";
    if (t.find(L"://") != std::wstring::npos || t.find(L'\\') != std::wstring::npos ||
        t.find(L'@') != std::wstring::npos)
        return L"";
    bool lo = false, up = false, dig = false;
    for (wchar_t c : t) {
        if (!TokenChar(c)) return L"";
        lo |= c >= L'a' && c <= L'z';
        up |= c >= L'A' && c <= L'Z';
        dig |= c >= L'0' && c <= L'9';
    }
    if (lo && up && dig && Entropy(t) >= 4.0) return L"похоже на ключ";
    return L"";
}

}  // namespace secrets
