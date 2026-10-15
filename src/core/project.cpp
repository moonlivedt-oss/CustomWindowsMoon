#include "core/project.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <map>
#include <vector>

#include "core/files.h"
#include "core/perf.h"
#include "core/text.h"

namespace project {

namespace {

std::map<std::wstring, Look> g_looks;
std::map<std::wstring, Info> g_infos;

bool FileThere(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ReadSmall(const std::wstring& p, size_t cap = 64 * 1024) {
    HANDLE h =
        CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string s(cap, '\0');
    DWORD got = 0;
    ReadFile(h, s.data(), (DWORD)cap, &got, nullptr);
    CloseHandle(h);
    s.resize(got);
    return s;
}

// "icon": "images/icon.png" из package.json; значки команд вида "$(book)" и svg не годятся
std::wstring PackageIcon(const std::wstring& dir) {
    std::string s = ReadSmall(dir + L"\\package.json");
    for (size_t at = s.find("\"icon\""); at != std::string::npos; at = s.find("\"icon\"", at + 6)) {
        size_t q1 = s.find('"', s.find(':', at) + 1);
        size_t q2 = q1 == std::string::npos ? q1 : s.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string v = s.substr(q1 + 1, q2 - q1 - 1);
        if (v.size() < 5 || v[0] == '$' || _stricmp(v.c_str() + v.size() - 4, ".png") != 0) continue;
        for (char& c : v)
            if (c == '/') c = '\\';
        std::wstring p = dir + L"\\" + Widen(v);
        if (FileThere(p)) return p;
    }
    return {};
}

bool HasExt(const std::wstring& n, const wchar_t* ext) {
    size_t e = wcslen(ext);
    return n.size() > e && _wcsicmp(n.c_str() + n.size() - e, ext) == 0;
}

Look Detect(const std::wstring& dir) {
    PERF_SCOPE("project.Detect");
    Look l;
    std::wstring name = dir.substr(dir.find_last_of(L'\\') + 1), low = Lower(name);
    for (const std::wstring& rel :
         {std::wstring(L"icon.png"), std::wstring(L"logo.png"), std::wstring(L"assets\\icon.png"),
          std::wstring(L"assets\\logo.png"), std::wstring(L"assets\\icons\\icon.png"),
          std::wstring(L"assets\\icons\\logo.png"), std::wstring(L"extension\\icon.png"),
          std::wstring(L"media\\icon.png"), std::wstring(L"res\\icon.png"), low + L"\\assets\\icon.png",
          low + L"\\assets\\logo.png"}) {
        if (FileThere(dir + L"\\" + rel)) {
            l.ownPng = dir + L"\\" + rel;
            return l;
        }
    }
    for (const std::wstring& d : {dir, dir + L"\\extension"}) {
        l.ownPng = PackageIcon(d);
        if (!l.ownPng.empty()) return l;
    }
    for (const std::wstring& rel :
         {std::wstring(L"app.ico"), std::wstring(L"icon.ico"), std::wstring(L"assets\\app.ico"),
          low + L"\\assets\\app.ico", std::wstring(L"src\\res\\app.ico")}) {
        if (FileThere(dir + L"\\" + rel)) {
            l.ownIco = dir + L"\\" + rel;
            return l;
        }
    }

    // тип - по файлам в корне
    int cpp = 0, py = 0, web = 0, script = 0, data = 0, docs = 0, total = 0;
    bool vscode = false, game = false, java = false, pkg = false, pyproj = false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || ++total > 300) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (_wcsicmp(n.c_str(), L"game") == 0 && FileThere(dir + L"\\game\\script.rpy")) game = true;
                continue;
            }
            if (_wcsicmp(n.c_str(), L"CMakeLists.txt") == 0 || HasExt(n, L".vcxproj") || HasExt(n, L".cpp")) cpp += 5;
            if (HasExt(n, L".h") || HasExt(n, L".hpp")) ++cpp;
            if (_wcsicmp(n.c_str(), L"pyproject.toml") == 0 || _wcsicmp(n.c_str(), L"requirements.txt") == 0 ||
                _wcsicmp(n.c_str(), L"setup.py") == 0)
                pyproj = true;
            if (HasExt(n, L".py") || HasExt(n, L".pyw")) ++py;
            if (_wcsicmp(n.c_str(), L"project.godot") == 0 || HasExt(n, L".uproject") || HasExt(n, L".rpy"))
                game = true;
            if (_wcsicmp(n.c_str(), L"pom.xml") == 0 || _wcsicmp(n.c_str(), L"build.gradle") == 0 ||
                HasExt(n, L".java"))
                java = true;
            if (_wcsicmp(n.c_str(), L"package.json") == 0) pkg = true;
            if (HasExt(n, L".html") || HasExt(n, L".css") || HasExt(n, L".js")) ++web;
            if (HasExt(n, L".ps1") || HasExt(n, L".psm1") || HasExt(n, L".bat") || HasExt(n, L".cmd") ||
                HasExt(n, L".reg") || HasExt(n, L".sh"))
                ++script;
            if (HasExt(n, L".sql") || HasExt(n, L".db") || HasExt(n, L".sqlite") || HasExt(n, L".csv")) ++data;
            if (HasExt(n, L".md") || HasExt(n, L".txt") || HasExt(n, L".pdf") || HasExt(n, L".docx")) ++docs;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (pkg) {
        std::string s = ReadSmall(dir + L"\\package.json");
        vscode = s.find("\"vscode\"") != std::string::npos && s.find("\"engines\"") != std::string::npos;
    }
    if (game)
        l.kind = Kind::Game;
    else if (cpp >= 5)
        l.kind = Kind::Cpp;
    else if (vscode)
        l.kind = Kind::VsCodeExt;
    else if (pyproj || py >= 2)
        l.kind = Kind::Python;
    else if (java)
        l.kind = Kind::Java;
    else if (pkg || web >= 2)
        l.kind = Kind::Web;
    else if (script >= 2 && script >= docs)
        l.kind = Kind::Scripts;
    else if (data >= 2)
        l.kind = Kind::Data;
    else if (docs >= 2)
        l.kind = Kind::Docs;
    if (l.kind == Kind::None && !name.empty()) {
        l.letter = name[0];
        CharUpperBuffW(&l.letter, 1);
    }
    return l;
}

std::wstring Date(const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[64] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"d MMM yyyy", buf, 64, nullptr);
    return buf;
}

std::wstring Size(ULONGLONG bytes) {
    wchar_t buf[32] = L"";
    StrFormatByteSizeW((LONGLONG)bytes, buf, 32);
    return buf;
}

std::wstring Count(int n, bool more) {
    std::wstring s = Plural(n, L"объект", L"объекта", L"объектов");  // уже с числом: «23 объекта»
    if (more) s.insert(s.find(L' '), L"+");
    return s;
}

// куда ведёт ярлык
std::wstring LinkTarget(const std::wstring& lnk) {
    IShellLinkW* sl = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)))
        return out;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
            wchar_t t[MAX_PATH] = L"";
            if (SUCCEEDED(sl->GetPath(t, MAX_PATH, nullptr, 0))) out = t;
        }
        pf->Release();
    }
    sl->Release();
    return out;
}

// описание и версия программы из её ресурсов
void ExeVersion(const std::wstring& exe, std::wstring& desc, std::wstring& ver) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (!size) return;
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, buf.data())) return;
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi)
        ver = std::to_wstring(HIWORD(fi->dwProductVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwProductVersionMS)) +
              L"." + std::to_wstring(HIWORD(fi->dwProductVersionLS));
    struct Lang {
        WORD lang, cp;
    }* tr = nullptr;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && tr && len >= sizeof(Lang)) {
        wchar_t key[64];
        swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
        wchar_t* d = nullptr;
        if (VerQueryValueW(buf.data(), key, (void**)&d, &len) && d && len > 1) desc = d;
    }
}

Info Describe(const std::wstring& full, bool dir, bool guess) {
    PERF_SCOPE("project.Describe");
    Info in;
    if (full.empty()) return in;
    if (Lower(full).find(L"recyclebin") != std::wstring::npos) {
        SHQUERYRBINFO rb{sizeof(rb)};
        in.kind = L"Корзина";
        if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb)))
            in.detail = rb.i64NumItems ? Count((int)rb.i64NumItems, false) + L" | " + Size(rb.i64Size) : L"пусто";
        return in;
    }
    if (full.rfind(L"shell:", 0) == 0) {
        in.kind = L"Системная папка";
        return in;
    }
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fa)) {
        in.kind = L"Не найдено";
        in.path = full;
        return in;
    }
    in.path = ParentDir(full);
    if (dir) {
        in.kind = L"Папка";
        if (guess) {
            const Look& l = LookOf(full);
            const wchar_t* kn = KindName(l.kind);
            in.kind = kn ? kn : (l.ownPng.empty() && l.ownIco.empty() ? L"Папка" : L"Проект");
        }
        int n = 0;
        bool more = false;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((full + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                    continue;
                if (++n >= 999) {
                    more = true;
                    break;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        in.detail = (n ? Count(n, more) : std::wstring(L"пусто")) + L" | изменено " + Date(fa.ftLastWriteTime);
        return in;
    }
    std::wstring target = full;
    if (HasExt(full, L".url")) {
        wchar_t url[1024] = L"";
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, 1024, full.c_str());
        std::wstring u = url;
        in.kind = u.rfind(L"steam://", 0) == 0                     ? L"Игра Steam"
                  : u.find(L"com.epicgames") != std::wstring::npos ? L"Игра Epic Games"
                                                                   : L"Ссылка";
        in.path = u;
        return in;
    }
    if (HasExt(full, L".lnk")) {
        target = LinkTarget(full);
        if (!target.empty()) in.path = target;
        if (target.empty() || !GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &fa)) {
            in.kind = L"Ярлык";
            return in;
        }
    }
    if (HasExt(target, L".exe")) {
        std::wstring desc, ver;
        ExeVersion(target, desc, ver);
        in.kind = desc.empty() ? L"Программа" : desc;
        ULONGLONG sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        in.detail = (ver.empty() || ver == L"0.0.0" ? L"" : L"версия " + ver + L" | ") + Size(sz);
        return in;
    }
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(target.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME)) in.kind = sfi.szTypeName;
    if (in.kind.empty()) in.kind = L"Файл";
    ULONGLONG sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    in.detail = Size(sz) + L" | изменено " + Date(fa.ftLastWriteTime);
    return in;
}

}  // namespace

const Look& LookOf(const std::wstring& dir) {
    auto it = g_looks.find(dir);
    if (it != g_looks.end()) return it->second;
    return g_looks[dir] = Detect(dir);
}

const wchar_t* KindIcon(Kind k) {
    switch (k) {
        case Kind::Cpp:
            return L"cpp.png";
        case Kind::Python:
            return L"code.png";
        case Kind::Web:
            return L"web.png";
        case Kind::VsCodeExt:
            return L"editor.png";
        case Kind::Game:
            return L"game.png";
        case Kind::Java:
            return L"java.png";
        case Kind::Scripts:
            return L"terminal.png";
        case Kind::Data:
            return L"db.png";
        case Kind::Docs:
            return L"files.png";
        default:
            return nullptr;
    }
}

const wchar_t* KindName(Kind k) {
    switch (k) {
        case Kind::Cpp:
            return L"Проект C++";
        case Kind::Python:
            return L"Проект Python";
        case Kind::Web:
            return L"Веб-проект";
        case Kind::VsCodeExt:
            return L"Расширение VS Code";
        case Kind::Game:
            return L"Игровой проект";
        case Kind::Java:
            return L"Проект Java";
        case Kind::Scripts:
            return L"Скрипты";
        case Kind::Data:
            return L"Данные";
        case Kind::Docs:
            return L"Документы";
        default:
            return nullptr;
    }
}

const Info& InfoOf(const std::wstring& full, bool dir, bool guess) {
    std::wstring key = full + (guess ? L"|g" : L"|-");
    auto it = g_infos.find(key);
    if (it != g_infos.end()) return it->second;
    return g_infos[key] = Describe(full, dir, guess);
}

void ForgetInfo() { g_infos.clear(); }

void Forget() {
    g_looks.clear();
    g_infos.clear();
}

}  // namespace project
