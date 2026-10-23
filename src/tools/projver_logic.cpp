// Окружение проекта: разбор требуемых версий и выбор установленной (без файлов и процессов, под тестами).
#include <cwctype>

#include "tools/projver.h"

namespace tools {

namespace {

// первая версия в тексте после позиции from: «>=3.11», «^20.1», «v18»
Ver FirstVer(const std::wstring& s, size_t from = 0) {
    for (size_t i = from; i < s.size(); ++i)
        if (iswdigit(s[i])) return ParseVer(s.substr(i));
    return {};
}

// значение строки "key": "..." (первое вхождение после from)
std::wstring JsonString(const std::wstring& json, const std::wstring& key, size_t from = 0) {
    size_t at = json.find(L"\"" + key + L"\"", from);
    if (at == std::wstring::npos) return L"";
    size_t colon = json.find(L':', at);
    size_t q1 = colon == std::wstring::npos ? std::wstring::npos : json.find(L'"', colon);
    size_t q2 = q1 == std::wstring::npos ? std::wstring::npos : json.find(L'"', q1 + 1);
    return q2 == std::wstring::npos ? L"" : json.substr(q1 + 1, q2 - q1 - 1);
}

std::wstring FirstLine(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    size_t b = s.find_first_of(L"\r\n", a);
    return s.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
}

}  // namespace

Ver ParseVer(const std::wstring& in) {
    std::wstring s = in;
    size_t i = 0;
    while (i < s.size() && (s[i] == L'v' || s[i] == L'V' || s[i] == L' ')) ++i;
    Ver v;
    int n = 0, d = 0;
    while (i < s.size() && iswdigit(s[i]) && d < 4) n = n * 10 + (s[i++] - L'0'), ++d;
    if (!d) return {};
    v.major = n;
    if (i < s.size() && s[i] == L'.' && i + 1 < s.size() && iswdigit(s[i + 1])) {
        ++i;
        n = 0, d = 0;
        while (i < s.size() && iswdigit(s[i]) && d < 4) n = n * 10 + (s[i++] - L'0'), ++d;
        v.minor = n;
    }
    return v;
}

bool Fits(const Need& n, const Ver& h) {
    if (!n.v.Known()) return true;
    if (!h.Known()) return false;
    if (n.atLeast)
        return h.major > n.v.major || (h.major == n.v.major && (n.v.minor < 0 || h.minor < 0 || h.minor >= n.v.minor));
    return h.major == n.v.major && (n.v.minor < 0 || h.minor == n.v.minor);
}

Need NodeNeed(const std::wstring& nvmrc, const std::wstring& nodeVersion, const std::wstring& pkg) {
    Need n;
    for (const auto& [text, from] : {std::pair<const std::wstring&, const wchar_t*>{nvmrc, L".nvmrc"},
                                     std::pair<const std::wstring&, const wchar_t*>{nodeVersion, L".node-version"}}) {
        Ver v = ParseVer(FirstLine(text));
        if (v.Known()) {
            n.v = Ver{v.major, -1};  // ветка: 20.11.0 - это «Node 20»
            n.from = from;
            return n;
        }
    }
    size_t eng = pkg.find(L"\"engines\"");
    if (eng != std::wstring::npos) {
        std::wstring r = JsonString(pkg, L"node", eng);
        Ver v = FirstVer(r);
        if (v.Known()) {
            n.v = Ver{v.major, -1};
            n.atLeast = r.find(L'>') != std::wstring::npos;  // «^20» и «20.x» - ветка 20, «>=18» - 18 и новее
            n.from = L"engines в package.json";
        }
    }
    return n;
}

Need PythonNeed(const std::wstring& pythonVersion, const std::wstring& pyproject) {
    Need n;
    Ver v = ParseVer(FirstLine(pythonVersion));
    if (v.Known()) {
        n.v = v;
        n.from = L".python-version";
        return n;
    }
    size_t at = pyproject.find(L"requires-python");
    if (at != std::wstring::npos) {
        size_t q1 = pyproject.find(L'"', at);
        size_t q2 = q1 == std::wstring::npos ? std::wstring::npos : pyproject.find(L'"', q1 + 1);
        std::wstring r = q2 == std::wstring::npos ? L"" : pyproject.substr(q1 + 1, q2 - q1 - 1);
        v = FirstVer(r);
        if (v.Known()) {
            n.v = v;
            n.atLeast = r.find(L'>') != std::wstring::npos || r.find(L"~=") != std::wstring::npos;
            n.from = L"requires-python";
        }
    }
    return n;
}

Need CMakeNeed(const std::wstring& lists) {
    Need n;
    std::wstring low = lists;
    for (wchar_t& c : low) c = (wchar_t)towlower(c);
    size_t at = low.find(L"cmake_minimum_required");
    if (at == std::wstring::npos) return n;
    size_t ver = low.find(L"version", at);
    if (ver == std::wstring::npos || ver > at + 60) return n;
    n.v = FirstVer(low, ver + 7);
    n.atLeast = true;
    n.from = L"cmake_minimum_required";
    return n;
}

int Pick(const Need& n, const std::vector<Installed>& have) {
    int best = -1;
    for (int i = 0; i < (int)have.size(); ++i) {
        if (!Fits(n, have[i].v)) continue;
        if (best < 0) {
            best = i;
            continue;
        }
        const Ver &a = have[i].v, &b = have[best].v;
        if (a.major > b.major || (a.major == b.major && a.minor > b.minor)) best = i;  // новее среди подходящих
    }
    return best;
}

std::wstring VerText(const Ver& v) {
    if (!v.Known()) return L"?";
    return std::to_wstring(v.major) + (v.minor >= 0 ? L"." + std::to_wstring(v.minor) : L"");
}

}  // namespace tools
