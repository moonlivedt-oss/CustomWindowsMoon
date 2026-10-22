// Разбор вывода сборки: первая настоящая ошибка и подсказка (Л31). Без окон, под тестами.
#include "tools/builderr.h"

#include <windows.h>

#include <cwctype>
#include <vector>

namespace builderr {

namespace {

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool Has(const std::wstring& s, const wchar_t* w) { return s.find(w) != std::wstring::npos; }

int Num(const std::wstring& s, size_t& at) {
    int n = 0;
    size_t from = at;
    while (at < s.size() && iswdigit(s[at])) n = n * 10 + (s[at++] - L'0');
    return at > from ? n : -1;
}

// «путь(12): error C2065: текст», «путь(12,5): fatal error C1083: текст» - MSVC и UBT
bool Msvc(const std::wstring& l, Error& e) {
    size_t err = l.find(L": error ");
    size_t fat = l.find(L": fatal error ");
    size_t at = fat != std::wstring::npos && (err == std::wstring::npos || fat < err) ? fat : err;
    if (at == std::wstring::npos || at < 4 || l[at - 1] != L')') return false;
    size_t open = l.rfind(L'(', at);
    if (open == std::wstring::npos || open == 0) return false;
    size_t p = open + 1;
    int line = Num(l, p), col = 0;
    if (line <= 0) return false;
    if (p < l.size() && l[p] == L',') {
        ++p;
        col = Num(l, p);
    }
    if (p != at - 1) return false;
    e.file = Trim(l.substr(0, open));
    e.line = line;
    e.col = col > 0 ? col : 0;
    std::wstring rest = l.substr(l.find(L"error ", at) + 6);
    size_t colon = rest.find(L':');
    if (colon != std::wstring::npos && colon < 10 && colon > 0 && iswupper(rest[0])) {
        e.code = rest.substr(0, colon);
        rest = rest.substr(colon + 1);
    }
    e.text = Trim(rest);
    return !e.file.empty();
}

// «путь:12:5: error: текст», «путь:12: fatal error: текст» - GCC и Clang (путь может начинаться с «D:»)
bool Gcc(const std::wstring& l, Error& e) {
    size_t at = l.find(L": error: ");
    size_t fat = l.find(L": fatal error: ");
    if (fat != std::wstring::npos && (at == std::wstring::npos || fat < at)) at = fat;
    if (at == std::wstring::npos) return false;
    // назад от «: error»: [:столбец]:строка
    size_t end = at;
    size_t c1 = l.rfind(L':', end - 1);
    if (c1 == std::wstring::npos) return false;
    size_t p = c1 + 1;
    int a = Num(l, p);
    if (a <= 0 || p != end) return false;
    int line = a, col = 0;
    size_t fileEnd = c1;
    if (c1 > 0) {
        size_t c2 = l.rfind(L':', c1 - 1);
        if (c2 != std::wstring::npos) {
            size_t q = c2 + 1;
            int b = Num(l, q);
            if (b > 0 && q == c1) line = b, col = a, fileEnd = c2;
        }
    }
    std::wstring file = Trim(l.substr(0, fileEnd));
    if (file.empty() || (file.size() == 1 && iswalpha(file[0]))) return false;  // «D» от «D:» - это не файл
    e.file = file;
    e.line = line;
    e.col = col;
    e.code.clear();
    e.text = Trim(l.substr(l.find(L"error: ", at) + 7));
    return true;
}

// «CMake Error at CMakeLists.txt:12 (add_executable):» - текст на следующей строке
bool CMake(const std::vector<std::wstring>& lines, size_t i, Error& e) {
    const std::wstring& l = lines[i];
    if (l.rfind(L"CMake Error at ", 0) != 0) return false;
    std::wstring rest = l.substr(15);
    size_t paren = rest.find(L" (");
    std::wstring where = paren == std::wstring::npos ? rest : rest.substr(0, paren);
    size_t colon = where.rfind(L':');
    if (colon == std::wstring::npos) return false;
    size_t p = colon + 1;
    int line = Num(where, p);
    if (line <= 0) return false;
    e.file = where.substr(0, colon);
    e.line = line;
    e.col = 0;
    e.code = L"CMake";
    e.text.clear();
    for (size_t j = i + 1; j < lines.size() && j < i + 4 && e.text.empty(); ++j) e.text = Trim(lines[j]);
    return true;
}

// итоговые строки - следствие ошибки, а не она сама
bool Summary(const std::wstring& l) {
    return Has(l, L"ld returned") || Has(l, L"build stopped") || l.rfind(L"FAILED:", 0) == 0 ||
           Has(l, L"Result: Failed") || Has(l, L"Error 1") || Has(l, L"error(s)") || Has(l, L"errors generated") ||
           Has(l, L"Build failed") || Has(l, L"subcommand failed") || Has(l, L"OtherCompilationError") ||
           Has(l, L"--build") || l.rfind(L"make", 0) == 0;
}

}  // namespace

bool First(const std::wstring& log, Error& e) {
    std::vector<std::wstring> lines;
    size_t at = 0;
    while (at <= log.size()) {
        size_t end = log.find(L'\n', at);
        if (end == std::wstring::npos) end = log.size();
        std::wstring l = log.substr(at, end - at);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        lines.push_back(Trim(l));
        at = end + 1;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        Error t;
        if (Msvc(lines[i], t) || Gcc(lines[i], t) || CMake(lines, i, t)) {
            e = t;
            return true;
        }
    }
    // без файла и строки: линкер, Live Coding, UBT «ERROR:»
    for (const std::wstring& l : lines) {
        if (Summary(l)) continue;
        size_t lnk = l.find(L"error LNK");
        if (lnk != std::wstring::npos) {
            e = {};
            size_t colon = l.find(L':', lnk);
            e.code = l.substr(lnk + 6, colon == std::wstring::npos ? 7 : colon - lnk - 6);
            e.text = colon == std::wstring::npos ? l : Trim(l.substr(colon + 1));
            return true;  // слева от ошибки линкера - .obj или .dll, а не исходник: открывать нечего
        }
        if (Has(l, L"undefined reference to") || l.rfind(L"ERROR: ", 0) == 0 || l.rfind(L"error: ", 0) == 0 ||
            l.rfind(L"fatal error: ", 0) == 0) {
            e = {};
            size_t u = l.find(L"undefined reference to");
            e.text = u != std::wstring::npos ? l.substr(u) : Trim(l.substr(l.find(L':') + 1));
            return true;
        }
    }
    return false;
}

std::wstring Plain(const Error& e) {
    std::wstring s;
    if (!e.file.empty()) {
        size_t slash = e.file.find_last_of(L"\\/");
        s = slash == std::wstring::npos ? e.file : e.file.substr(slash + 1);
        if (e.line) s += L", строка " + std::to_wstring(e.line);
        s += L": ";
    }
    std::wstring t = e.text.size() > 160 ? e.text.substr(0, 157) + L"..." : e.text;
    return s + (e.code.empty() || e.code == L"CMake" ? L"" : e.code + L" ") + t;
}

std::wstring Hint(const Error& e) {
    const std::wstring& c = e.code;
    std::wstring t = e.text;
    for (wchar_t& ch : t) ch = (wchar_t)towlower(ch);
    if (Has(t, L"live coding")) return L"Идёт Live Coding: закройте редактор или нажмите в нём Ctrl+Alt+F11.";
    if (c == L"LNK1104" || c == L"LNK1168" || (Has(t, L"cannot open") && (Has(t, L".exe") || Has(t, L".dll"))) ||
        Has(t, L"permission denied"))
        return L"Файл занят: закройте запущенную программу или редактор и соберите снова.";
    if (c == L"LNK2019" || c == L"LNK2001" || Has(t, L"undefined reference") || Has(t, L"unresolved external"))
        return L"Объявлено, но не собрано: нет определения функции, не подключён модуль (Build.cs) или .lib.";
    if (c == L"C2065" || Has(t, L"undeclared identifier") || Has(t, L"was not declared"))
        return L"Имя не объявлено: опечатка или не подключён заголовок.";
    if (c == L"C1083" || Has(t, L"no such file") || Has(t, L"file not found"))
        return L"Не найден заголовок или файл: путь, include или зависимость модуля (Build.cs).";
    if (c == L"C2039" || Has(t, L"is not a member of") || Has(t, L"has no member"))
        return L"У класса нет такого поля или функции: опечатка или забыт заголовок.";
    if (c == L"C2143" || c == L"C2059" || Has(t, L"expected ';'") || Has(t, L"expected '"))
        return L"Синтаксис: пропущена «;» или скобка - часто строкой выше.";
    if (c == L"C2084" || c == L"C2011" || Has(t, L"redefinition"))
        return L"Определено дважды: повтор в другом файле или нет #pragma once.";
    if (c == L"C4430" || Has(t, L"does not name a type") || Has(t, L"unknown type name"))
        return L"Неизвестный тип: забыт include или предварительное объявление.";
    if (c == L"C2440" || c == L"C2664" || Has(t, L"cannot convert") || Has(t, L"invalid conversion"))
        return L"Не тот тип: проверьте, что передаётся (указатель, ссылка, const).";
    if (c == L"CMake") return L"Ошибка в CMakeLists.txt - строка указана, текст под ней в выводе.";
    if (Has(t, L"uproperty") || Has(t, L"ufunction") || Has(t, L"generated.h"))
        return L"Ошибка в макросах Unreal (UHT): проверьте UPROPERTY/UFUNCTION и что .generated.h - последний include.";
    return L"";
}

std::wstring GotoArg(const Error& e, const std::wstring& base) {
    if (e.file.empty()) return L"";
    std::wstring f = e.file;
    for (wchar_t& ch : f)
        if (ch == L'/') ch = L'\\';
    bool rooted = (f.size() > 2 && f[1] == L':') || f.rfind(L"\\\\", 0) == 0;
    if (!rooted && !base.empty()) {
        wchar_t full[MAX_PATH * 2];
        std::wstring joined = base + L"\\" + f;
        DWORD n = GetFullPathNameW(joined.c_str(), MAX_PATH * 2, full, nullptr);
        f = n && n < MAX_PATH * 2 ? full : joined;
    }
    std::wstring s = f;
    if (e.line) s += L":" + std::to_wstring(e.line) + (e.col ? L":" + std::to_wstring(e.col) : L"");
    return s;
}

}  // namespace builderr
