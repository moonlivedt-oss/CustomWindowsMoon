// Окружение проекта (Л33): какие версии Node, Python и CMake стоят и какая первая в PATH.
#include <windows.h>

#include <map>

#include "tools/projver.h"
#include "tools/tools.h"

namespace tools {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::map<std::wstring, Ver> g_cache;  // exe -> версия (cmake --version - процесс, второй раз не запускаем)

std::wstring Ex(const wchar_t* s) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(s, buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? buf : L"";
}

bool IsFileW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Dir(const std::wstring& exe) { return exe.substr(0, exe.find_last_of(L'\\')); }

std::wstring ReadSmall(const std::wstring& path) {
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string s(64 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// версия exe: из ресурса версии, иначе (cmake из MSYS2 без ресурса) - «--version»
Ver ExeVer(const std::wstring& exe) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_cache.find(exe);
    Ver cached = it != g_cache.end() ? it->second : Ver{};
    bool have = it != g_cache.end();
    ReleaseSRWLockShared(&g_lock);
    if (have) return cached;
    Ver v;
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (size) {
        std::vector<BYTE> buf(size);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(exe.c_str(), 0, size, buf.data()) &&
            VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) && fi && HIWORD(fi->dwFileVersionMS))
            v = Ver{(int)HIWORD(fi->dwFileVersionMS), (int)LOWORD(fi->dwFileVersionMS)};
    }
    if (!v.Known()) {
        std::wstring out;
        if (RunCapture(L"\"" + exe + L"\" --version", out, 8000) == 0) {
            for (size_t i = 0; i < out.size(); ++i)
                if (iswdigit(out[i])) {
                    v = ParseVer(out.substr(i));
                    break;
                }
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_cache[exe] = v;
    ReleaseSRWLockExclusive(&g_lock);
    return v;
}

// exe в подпапках dir по маске («v*»), путь внутри - tail
void Glob(const std::wstring& dir, const wchar_t* mask, const wchar_t* tail, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + mask).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring p = dir + L"\\" + fd.cFileName + tail;
        if (IsFileW(p)) out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<std::wstring> NodeExes() {
    std::vector<std::wstring> v;
    for (const wchar_t* p : {L"%ProgramFiles%\\nodejs\\node.exe", L"%ProgramFiles(x86)%\\nodejs\\node.exe"})
        if (IsFileW(Ex(p))) v.push_back(Ex(p));
    for (const wchar_t* d : {L"%NVM_HOME%", L"%APPDATA%\\nvm"})
        if (Ex(d).find(L'%') == std::wstring::npos) Glob(Ex(d), L"v*", L"\\node.exe", v);
    Glob(Ex(L"%APPDATA%\\fnm\\node-versions"), L"v*", L"\\installation\\node.exe", v);
    Glob(Ex(L"%LOCALAPPDATA%\\Volta\\tools\\image\\node"), L"*", L"\\node.exe", v);
    return v;
}

std::vector<std::wstring> PythonExes() {
    std::vector<std::wstring> v;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY k;
        if (RegOpenKeyExW(root, L"Software\\Python\\PythonCore", 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t name[64];
        for (DWORD i = 0, n = 64; RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
             ++i, n = 64) {
            wchar_t path[MAX_PATH * 2];
            DWORD size = sizeof(path);
            if (RegGetValueW(k, (std::wstring(name) + L"\\InstallPath").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path,
                             &size) == ERROR_SUCCESS) {
                std::wstring exe = std::wstring(path);
                if (!exe.empty() && exe.back() != L'\\') exe += L'\\';
                exe += L"python.exe";
                if (IsFileW(exe)) v.push_back(exe);
            }
        }
        RegCloseKey(k);
    }
    Glob(Ex(L"%LOCALAPPDATA%\\Programs\\Python"), L"Python3*", L"\\python.exe", v);
    Glob(Ex(L"%USERPROFILE%\\.pyenv\\pyenv-win\\versions"), L"*", L"\\python.exe", v);
    return v;
}

std::vector<std::wstring> CMakeExes() {
    std::vector<std::wstring> v;
    for (const wchar_t* p : {L"%ProgramFiles%\\CMake\\bin\\cmake.exe", L"D:\\msys64\\ucrt64\\bin\\cmake.exe",
                             L"C:\\msys64\\ucrt64\\bin\\cmake.exe"})
        if (IsFileW(Ex(p))) v.push_back(Ex(p));
    // Visual Studio: <год>\<выпуск>\Common7\...\cmake.exe
    WIN32_FIND_DATAW fd;
    std::wstring vs = Ex(L"%ProgramFiles%\\Microsoft Visual Studio");
    HANDLE h = FindFirstFileW((vs + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            Glob(vs + L"\\" + fd.cFileName, L"*",
                 L"\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe", v);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return v;
}

std::wstring FirstInPath(const wchar_t* exe) {
    wchar_t buf[MAX_PATH * 2];
    if (!SearchPathW(nullptr, exe, nullptr, MAX_PATH * 2, buf, nullptr)) return L"";
    std::wstring p = buf;
    std::wstring low = p;
    for (wchar_t& c : low) c = (wchar_t)towlower(c);
    if (low.find(L"\\windowsapps\\") != std::wstring::npos) return L"";  // заглушка Store, не настоящий python
    return p;
}

}  // namespace

std::vector<EnvPick> ProjectPicks(const std::wstring& dirIn) {
    std::wstring dir = dirIn;
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    std::vector<EnvPick> out;
    struct Tool {
        const wchar_t* name;
        const wchar_t* exe;
        Need need;
        std::vector<std::wstring> (*list)();
    } tools[] = {
        {L"Node", L"node.exe",
         NodeNeed(ReadSmall(dir + L"\\.nvmrc"), ReadSmall(dir + L"\\.node-version"),
                  ReadSmall(dir + L"\\package.json")),
         NodeExes},
        {L"Python", L"python.exe",
         PythonNeed(ReadSmall(dir + L"\\.python-version"), ReadSmall(dir + L"\\pyproject.toml")), PythonExes},
        {L"CMake", L"cmake.exe", CMakeNeed(ReadSmall(dir + L"\\CMakeLists.txt")), CMakeExes},
    };
    for (Tool& t : tools) {
        if (!t.need.v.Known()) continue;
        // в своём venv Python уже правильный - его подключает DetectEnv
        if (!wcscmp(t.name, L"Python") &&
            (IsFileW(dir + L"\\.venv\\pyvenv.cfg") || IsFileW(dir + L"\\venv\\pyvenv.cfg")))
            continue;
        std::wstring first = FirstInPath(t.exe);
        Ver fv = first.empty() ? Ver{} : ExeVer(first);
        if (Fits(t.need, fv)) continue;
        std::vector<Installed> have;
        for (const std::wstring& e : t.list()) have.push_back({ExeVer(e), Dir(e)});
        int i = Pick(t.need, have);
        if (i < 0) continue;
        out.push_back({t.name, t.need, fv, have[i]});
    }
    return out;
}

}  // namespace tools
