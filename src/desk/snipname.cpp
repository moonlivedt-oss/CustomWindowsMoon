// Скриншот в работу: имя и папка снимка (без окон и файлов, под тестами).
#include "desk/snipname.h"

#include <cwctype>

namespace snipname {

namespace {

bool EndsI(const std::wstring& s, const std::wstring& tail) {
    if (s.size() < tail.size()) return false;
    for (size_t i = 0; i < tail.size(); ++i)
        if (towlower(s[s.size() - tail.size() + i]) != towlower(tail[i])) return false;
    return true;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    return s.substr(a);
}

}  // namespace

std::wstring Context(const std::wstring& titleIn, const std::wstring& exe) {
    std::wstring title = Trim(titleIn);
    // хвост с названием программы: « - Visual Studio Code», « - Google Chrome», « — Mozilla Firefox»...
    for (const wchar_t* tail :
         {L" - Visual Studio Code", L" - Google Chrome", L" - Mozilla Firefox", L" — Mozilla Firefox",
          L" - Microsoft Edge", L" - Microsoft​ Edge", L" - Яндекс Браузер", L" - Opera", L" - Brave",
          L" - Unreal Editor", L" - Blender", L" - Telegram"})
        if (EndsI(title, tail)) {
            title = title.substr(0, title.size() - wcslen(tail));
            if (std::wstring(tail) == L" - Visual Studio Code") {
                // «файл - папка» -> «папка файл»; «● » у несохранённого - прочь
                if (title.rfind(L"● ", 0) == 0) title = title.substr(2);
                size_t dash = title.rfind(L" - ");
                if (dash != std::wstring::npos) title = title.substr(dash + 3) + L" " + title.substr(0, dash);
            }
            break;
        }
    (void)exe;
    return Trim(title);
}

std::wstring FileName(const std::wstring& context, const SYSTEMTIME& t) {
    wchar_t when[40];
    std::wstring c;
    for (wchar_t ch : context) {
        if (wcschr(L"<>:\"/\\|?*", ch) || ch < 32) ch = L' ';
        if (ch == L' ' && (c.empty() || c.back() == L' ')) continue;
        c += ch;
    }
    c = Trim(c);
    while (!c.empty() && (c.back() == L'.' || c.back() == L' ')) c.pop_back();
    if (c.size() > 56) c = Trim(c.substr(0, 56));
    if (c.empty()) {
        swprintf(when, 40, L"Снимок %04d-%02d-%02d %02d-%02d-%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond);
        return when;
    }
    swprintf(when, 40, L" %04d-%02d-%02d %02d-%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return c + when;
}

std::wstring TargetDir(const std::wstring& folder, bool isProject) {
    if (folder.empty()) return L"";
    std::wstring f = folder;
    while (f.size() > 3 && f.back() == L'\\') f.pop_back();
    return isProject ? f + L"\\screenshots" : f;
}

}  // namespace snipname
