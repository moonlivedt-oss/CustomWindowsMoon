// Разработка: конфликты версий в PATH, терминал с окружением проекта, папка из Проводника.
#include <windows.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>

#include "core/text.h"
#include "tools/projver.h"
#include "tools/tools.h"

namespace tools {

namespace {

constexpr wchar_t kUserEnv[] = L"Environment";

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool HasExt(const std::wstring& dir, const wchar_t* ext, std::wstring* first = nullptr) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*" + ext).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    if (first) *first = dir + L"\\" + fd.cFileName;
    FindClose(h);
    return true;
}

std::wstring DirOf(const std::wstring& f) { return f.substr(0, f.find_last_of(L'\\')); }

std::wstring Norm(std::wstring p) {
    p = Lower(Trim(p));
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

// Версия программы: из ресурса версии exe, иначе из пути (Python310 -> 3.10, ...\14.2.0\...), иначе пусто.
std::wstring Version(const std::wstring& exe) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(exe.c_str(), &h);
    if (n) {
        std::vector<BYTE> buf(n);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(exe.c_str(), 0, n, buf.data()) &&
            VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi && (fi->dwFileVersionMS || fi->dwFileVersionLS))
            return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) +
                   L"." + std::to_wstring(HIWORD(fi->dwFileVersionLS));
    }
    std::wstring l = Lower(exe);
    size_t py = l.find(L"\\python3");
    if (py != std::wstring::npos) {
        size_t a = py + 8, b = a;
        while (b < l.size() && iswdigit(l[b])) ++b;
        if (b > a) return L"3." + l.substr(a, b - a);
    }
    // первая группа вида 1.2 или 1.2.3 в пути
    for (size_t i = 0; i < l.size(); ++i) {
        if (!iswdigit(l[i]) || (i && (iswalnum(l[i - 1])))) continue;
        size_t j = i, dots = 0;
        while (j < l.size() && (iswdigit(l[j]) || (l[j] == L'.' && j + 1 < l.size() && iswdigit(l[j + 1])))) {
            if (l[j] == L'.') ++dots;
            ++j;
        }
        if (dots) return l.substr(i, j - i);
    }
    return L"";
}

std::wstring MajorMinor(const std::wstring& v) {
    size_t a = v.find(L'.');
    if (a == std::wstring::npos) return v;
    size_t b = v.find(L'.', a + 1);
    return b == std::wstring::npos ? v : v.substr(0, b);
}

std::vector<std::wstring> UserPathRaw() {
    DWORD size = 0;
    std::vector<std::wstring> out;
    if (RegGetValueW(HKEY_CURRENT_USER, kUserEnv, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr,
                     nullptr, &size) != ERROR_SUCCESS)
        return out;
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kUserEnv, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr,
                     s.data(), &size) != ERROR_SUCCESS)
        return out;
    s.resize(wcslen(s.c_str()));
    for (const std::wstring& p : Split(s, L';'))
        if (!p.empty()) out.push_back(p);
    return out;
}

std::wstring Expand(const std::wstring& s) {
    wchar_t buf[4096];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 4096);
    return n && n <= 4096 ? std::wstring(buf) : s;
}

}  // namespace

// ---------------- конфликты версий ----------------

std::vector<Conflict> PathConflicts() {
    std::vector<std::wstring> user;
    for (const std::wstring& p : UserPathRaw()) user.push_back(Norm(Expand(p)));
    auto inUser = [&](const std::wstring& exe) {
        return std::find(user.begin(), user.end(), Norm(DirOf(exe))) != user.end();
    };
    std::vector<Conflict> out;
    for (const Which& w : WhichAll()) {
        if (w.other.empty()) continue;
        std::wstring wv = Version(w.path);
        for (const std::wstring& o : w.other) {
            // заглушки «Магазина» и ярлыки WindowsApps версии не говорят - не конфликт
            if (Lower(o).find(L"\\windowsapps\\") != std::wstring::npos) continue;
            std::wstring ov = Version(o);
            if (wv.empty() || ov.empty() || MajorMinor(wv) == MajorMinor(ov)) continue;
            Conflict c;
            c.cmd = w.cmd;
            c.winner = w.path;
            c.loser = o;
            c.winnerVer = MajorMinor(wv);
            c.loserVer = MajorMinor(ov);
            c.fixable = inUser(w.path) && inUser(o);
            out.push_back(c);
            break;  // по одной команде - один конфликт: с ближайшей затенённой
        }
    }
    return out;
}

bool MoveUserPathFirst(const std::wstring& dir, const Log& log) {
    std::vector<std::wstring> raw = UserPathRaw(), out;
    std::wstring want = Norm(dir);
    bool found = false;
    for (const std::wstring& p : raw)
        if (!found && Norm(Expand(p)) == want) {
            out.insert(out.begin(), p);
            found = true;
        } else {
            out.push_back(p);
        }
    if (!found) {
        log(L"Этой папки нет в PATH пользователя: " + dir);
        return false;
    }
    return WriteUserPath(out, log);
}

// ---------------- терминал с окружением ----------------

namespace {

std::wstring FindMsys() {
    for (const std::wstring& p : UserPathRaw()) {
        std::wstring e = Expand(p), l = Lower(e);
        if (l.find(L"msys64\\ucrt64\\bin") != std::wstring::npos && IsDir(e)) return e;
    }
    for (const wchar_t* c : {L"D:\\msys64\\ucrt64\\bin", L"C:\\msys64\\ucrt64\\bin"})
        if (IsDir(c)) return c;
    return L"";
}

// Папка движка Unreal для проекта: EngineAssociation из .uproject -> реестр установленных версий.
std::wstring UnrealEngine(const std::wstring& uproject) {
    std::wstring text;
    HANDLE f = CreateFileW(uproject.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[8192];
        DWORD got = 0;
        if (ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr)) text = Widen(std::string(buf, got));
        CloseHandle(f);
    }
    std::wstring assoc;
    size_t k = text.find(L"\"EngineAssociation\"");
    if (k != std::wstring::npos) {
        size_t a = text.find(L'"', text.find(L':', k) + 1), b = a == std::wstring::npos ? a : text.find(L'"', a + 1);
        if (b != std::wstring::npos) assoc = text.substr(a + 1, b - a - 1);
    }
    auto reg = [](HKEY root, const std::wstring& key, const wchar_t* name) {
        wchar_t v[MAX_PATH] = L"";
        DWORD sz = sizeof(v);
        if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, v, &sz) != ERROR_SUCCESS)
            return std::wstring();
        return std::wstring(v);
    };
    std::wstring dir;
    if (!assoc.empty()) {
        dir = reg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EpicGames\\Unreal Engine\\" + assoc, L"InstalledDirectory");
        if (dir.empty()) dir = reg(HKEY_CURRENT_USER, L"Software\\Epic Games\\Unreal Engine\\Builds", assoc.c_str());
    }
    for (const wchar_t* c : {L"D:\\Unreal Engine", L"C:\\Program Files\\Epic Games"})
        if (dir.empty() && IsDir(c)) {
            // любая установленная версия: UE_5.x
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((std::wstring(c) + L"\\UE_*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                dir = std::wstring(c) + L"\\" + fd.cFileName;
                FindClose(h);
            }
            if (dir.empty() && IsDir(std::wstring(c) + L"\\Engine")) dir = c;
        }
    std::replace(dir.begin(), dir.end(), L'/', L'\\');
    return IsDir(dir + L"\\Engine\\Build\\BatchFiles") ? dir : L"";
}

}  // namespace

TermEnv DetectEnv(const std::wstring& dirIn) {
    TermEnv e;
    e.dir = dirIn;
    while (e.dir.size() > 3 && e.dir.back() == L'\\') e.dir.pop_back();
    std::wstring path;  // что добавить в начало PATH
    for (const wchar_t* v : {L".venv", L"venv", L"env"}) {
        std::wstring act = e.dir + L"\\" + v + L"\\Scripts\\activate.bat";
        if (IsFile(e.dir + L"\\" + v + L"\\pyvenv.cfg") && IsFile(act)) {
            e.setup += L"call \"" + act + L"\" && ";
            e.what.push_back(L"Python " + std::wstring(v));
            break;
        }
    }
    bool cpp = IsFile(e.dir + L"\\CMakeLists.txt") || IsFile(e.dir + L"\\meson.build") || HasExt(e.dir, L".cpp") ||
               HasExt(e.dir, L".c") || IsFile(e.dir + L"\\Makefile");
    if (cpp) {
        std::wstring msys = FindMsys();
        if (!msys.empty()) {
            path += msys + L";";
            e.what.push_back(L"MSYS2 ucrt64");
        }
    }
    if (IsFile(e.dir + L"\\package.json") && IsDir(e.dir + L"\\node_modules\\.bin")) {
        path += e.dir + L"\\node_modules\\.bin;";
        e.what.push_back(L"node_modules\\.bin");
    }
    std::wstring up;
    if (HasExt(e.dir, L".uproject", &up)) {
        std::wstring eng = UnrealEngine(up);
        if (!eng.empty()) {
            path += eng + L"\\Engine\\Build\\BatchFiles;" + eng + L"\\Engine\\Binaries\\Win64;";
            e.setup += L"set \"UE_ENGINE=" + eng + L"\" && set \"UPROJECT=" + up + L"\" && ";
            e.what.push_back(L"Unreal (RunUAT, Build.bat)");
        }
    }
    if (IsFile(e.dir + L"\\Cargo.toml")) e.what.push_back(L"Rust");
    // Л33: проект просит другую версию Node, Python или CMake, чем первая в PATH, а нужная стоит - она первой
    for (const EnvPick& p : ProjectPicks(e.dir)) {
        path = p.use.dir + L";" + (p.tool == L"Python" ? p.use.dir + L"\\Scripts;" : L"") + path;  // и pip
        e.what.push_back(p.tool + L" " + VerText(p.use.v) + L" (" + p.need.from + L"; в PATH первой - " +
                         VerText(p.first) + L")");
    }
    if (!path.empty()) e.setup = L"set \"PATH=" + path + L"%PATH%\" && " + e.setup;
    return e;
}

bool OpenEnvTerminal(const TermEnv& env) {
    if (!IsDir(env.dir)) return false;
    std::wstring title = env.dir.substr(env.dir.find_last_of(L'\\') + 1);
    std::wstring hello = L"echo Окружение: " + (env.what.empty() ? std::wstring(L"обычное") : [&] {
                             std::wstring s;
                             for (const std::wstring& w : env.what) s += (s.empty() ? L"" : L", ") + w;
                             return s;
                         }());
    std::wstring inner = L"chcp 65001 >nul && " + env.setup + hello + (env.run.empty() ? L"" : L" && " + env.run);
    wchar_t wt[MAX_PATH];
    if (SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, wt, nullptr)) {
        // «;» в командной строке wt разделяет вкладки - в PATH его нужно экранировать
        std::wstring esc;
        for (wchar_t c : inner) esc += c == L';' ? std::wstring(L"\\;") : std::wstring(1, c);
        std::wstring args = L"-d \"" + env.dir + L"\" --title \"" + title + L"\" cmd /k \"" + esc + L"\"";
        return (INT_PTR)ShellExecuteW(nullptr, nullptr, wt, args.c_str(), nullptr, SW_SHOWNORMAL) > 32;
    }
    std::wstring args = L"/k \"" + inner + L"\"";
    return (INT_PTR)ShellExecuteW(nullptr, nullptr, L"cmd.exe", args.c_str(), env.dir.c_str(), SW_SHOWNORMAL) > 32;
}

// Папка самого верхнего окна Проводника (по порядку окон на экране).
std::wstring ExplorerFolder() {
    std::wstring out;
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw)))) return out;
    // окна Проводника по порядку z: первое найденное через EnumWindows - верхнее
    std::vector<HWND> order;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            wchar_t cls[64];
            if (GetClassNameW(w, cls, 64) && wcscmp(cls, L"CabinetWClass") == 0 && IsWindowVisible(w))
                ((std::vector<HWND>*)lp)->push_back(w);
            return TRUE;
        },
        (LPARAM)&order);
    long n = 0;
    sw->get_Count(&n);
    size_t best = order.size();
    for (long i = 0; i < n; ++i) {
        VARIANT v;
        v.vt = VT_I4;
        v.lVal = i;
        IDispatch* d = nullptr;
        if (FAILED(sw->Item(v, &d)) || !d) continue;
        IWebBrowserApp* app = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&app)))) {
            SHANDLE_PTR h = 0;
            app->get_HWND(&h);
            size_t pos = std::find(order.begin(), order.end(), (HWND)h) - order.begin();
            if (pos < best) {
                IServiceProvider* sp = nullptr;
                if (SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&sp)))) {
                    IShellBrowser* sb = nullptr;
                    if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb)))) {
                        IShellView* view = nullptr;
                        if (SUCCEEDED(sb->QueryActiveShellView(&view))) {
                            IFolderView* fv = nullptr;
                            if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&fv)))) {
                                IPersistFolder2* pf = nullptr;
                                if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf)))) {
                                    PIDLIST_ABSOLUTE pidl = nullptr;
                                    wchar_t p[MAX_PATH];
                                    if (SUCCEEDED(pf->GetCurFolder(&pidl)) && SHGetPathFromIDListW(pidl, p)) {
                                        out = p;
                                        best = pos;
                                    }
                                    CoTaskMemFree(pidl);
                                    pf->Release();
                                }
                                fv->Release();
                            }
                            view->Release();
                        }
                        sb->Release();
                    }
                    sp->Release();
                }
            }
            app->Release();
        }
        d->Release();
    }
    sw->Release();
    return out;
}

}  // namespace tools
