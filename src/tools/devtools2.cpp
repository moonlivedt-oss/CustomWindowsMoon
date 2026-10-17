// Разработка и место: состояние Git в проектах, диски Docker и WSL (размер, сжатие), папки в AppData от
// программ, которых больше нет.
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <set>

#include "core/elevate.h"
#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

bool IsDirP(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Env(const wchar_t* s) {
    wchar_t b[MAX_PATH] = {};
    ExpandEnvironmentStringsW(s, b, MAX_PATH);
    return b;
}

ULONGLONG FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)
               ? ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow
               : 0;
}

}  // namespace

// ---------------- Git ----------------

std::vector<GitRepo> GitRepos(const std::vector<std::wstring>& roots) {
    std::vector<GitRepo> out;
    std::wstring git;
    for (const Which& w : WhichAll())
        if (w.cmd == L"git") git = w.path;
    if (git.empty()) return out;
    // репозитории: папка с .git, не глубже трёх уровней от корня проектов; внутрь найденного не идём
    std::vector<std::wstring> found;
    std::set<std::wstring> seen;
    std::vector<std::pair<std::wstring, int>> stack;
    for (const std::wstring& r : roots)
        if (IsDirP(r)) stack.push_back({r, 0});
    while (!stack.empty() && found.size() < 60) {
        auto [d, depth] = stack.back();
        stack.pop_back();
        if (!seen.insert(Lower(d)).second) continue;
        if (GetFileAttributesW((d + L"\\.git").c_str()) != INVALID_FILE_ATTRIBUTES) {
            found.push_back(d);
            continue;
        }
        if (depth >= 3) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h =
            FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            std::wstring n = Lower(fd.cFileName);
            if (n == L"node_modules" || n == L"build" || n == L"intermediate" || n == L"binaries" || n == L"saved")
                continue;
            stack.push_back({d + L"\\" + fd.cFileName, depth + 1});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const std::wstring& d : found) {
        std::wstring outText;
        // -c core.quotepath=off: русские имена файлов без \320\277
        if (RunCapture(L"\"" + git + L"\" -c core.quotepath=off -C \"" + d + L"\" status --porcelain=v1 -b", outText,
                       15000) != 0)
            continue;
        GitRepo g;
        g.path = d;
        g.name = d.substr(d.find_last_of(L'\\') + 1);
        for (const std::wstring& line : Split(outText, L'\n')) {
            if (line.rfind(L"## ", 0) == 0) {
                std::wstring b = line.substr(3);
                g.branch = b.substr(0, b.find(L"..."));
                g.branch = g.branch.substr(0, g.branch.find(L' '));
                g.noUpstream = b.find(L"...") == std::wstring::npos;
                size_t a = b.find(L"ahead ");
                if (a != std::wstring::npos) g.ahead = _wtoi(b.c_str() + a + 6);
                size_t be = b.find(L"behind ");
                if (be != std::wstring::npos) g.behind = _wtoi(b.c_str() + be + 7);
            } else if (!Trim(line).empty()) {
                ++g.changed;
            }
        }
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(),
              [](const GitRepo& a, const GitRepo& b) { return a.changed + a.ahead * 3 > b.changed + b.ahead * 3; });
    return out;
}

// ---------------- Docker и WSL ----------------

std::vector<VirtualDisk> VirtualDisks() {
    std::vector<VirtualDisk> out;
    std::wstring local = Env(L"%LOCALAPPDATA%");
    // Docker Desktop: диск образов и служебный дистрибутив
    for (const wchar_t* rel : {L"\\Docker\\wsl\\disk\\docker_data.vhdx", L"\\Docker\\wsl\\data\\ext4.vhdx",
                               L"\\Docker\\wsl\\main\\ext4.vhdx"}) {
        std::wstring p = local + rel;
        if (ULONGLONG s = FileSize(p)) out.push_back({p, rel[8] == L'w' ? L"Docker" : L"Docker", s, true});
    }
    // дистрибутивы WSL из Магазина: Packages\<дистрибутив>\LocalState\ext4.vhdx
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((local + L"\\Packages\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring p = local + L"\\Packages\\" + fd.cFileName + L"\\LocalState\\ext4.vhdx";
            if (ULONGLONG s = FileSize(p)) {
                std::wstring n = fd.cFileName;
                n = n.substr(0, n.find(L'_'));
                size_t dot = n.find_last_of(L'.');
                out.push_back({p, L"WSL: " + (dot == std::wstring::npos ? n : n.substr(dot + 1)), s, false});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const VirtualDisk& a, const VirtualDisk& b) { return a.bytes > b.bytes; });
    return out;
}

// В процессе с правами: сжать диски WSL и Docker (diskpart compact). Список считается здесь же и только из
// известных мест в профиле; WSL перед этим должен быть остановлен (wsl --shutdown делает обычный процесс).
int CompactVhdAdminMain(const Log& log) {
    std::vector<VirtualDisk> list = VirtualDisks();
    if (list.empty()) {
        log(L"Дисков WSL и Docker нет.");
        return 0;
    }
    CreateDirectoryW(elevate::DataDir().c_str(), nullptr);
    int done = 0;
    for (const VirtualDisk& v : list) {
        if (v.bytes < 1024ull * 1024 * 1024) continue;  // меньше гигабайта - не стоит
        std::wstring script = elevate::DataDir() + L"\\compact.txt";
        std::string body = Narrow(L"select vdisk file=\"" + v.path +
                                  L"\"\r\nattach vdisk readonly\r\ncompact vdisk\r\ndetach vdisk\r\nexit\r\n");
        HANDLE f = CreateFileW(script.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD put = 0;
        WriteFile(f, body.data(), (DWORD)body.size(), &put, nullptr);
        CloseHandle(f);
        log(L"Сжимаю " + v.name + L" (" + Bytes(v.bytes) + L"), это может занять несколько минут...");
        std::wstring out;
        int code = RunCapture(L"diskpart.exe /s \"" + script + L"\"", out, 60 * 60 * 1000);
        ULONGLONG after = FileSize(v.path);
        if (code == 0 && after) {
            log(v.name + L": было " + Bytes(v.bytes) + L", стало " + Bytes(after) + L".");
            ++done;
        } else {
            log(v.name + L": не вышло (диск занят? закройте Docker Desktop и повторите).");
        }
        DeleteFileW(script.c_str());
    }
    return done;
}

// ---------------- хвосты удалённых программ ----------------

namespace {

// Что установлено: имена и издатели из «Программ и компонентов» - по словам длиннее трёх букв.
std::set<std::wstring> InstalledWords() {
    std::set<std::wstring> w;
    auto add = [&](const std::wstring& s) {
        std::wstring cur;
        for (wchar_t c : Lower(s) + L" ") {
            if (iswalnum(c)) {
                cur += c;
            } else {
                if (cur.size() > 3) w.insert(cur);
                cur.clear();
            }
        }
    };
    struct Root {
        HKEY root;
        REGSAM view;
    } roots[] = {{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}, {HKEY_CURRENT_USER, 0}};
    for (const Root& r : roots) {
        HKEY k;
        if (RegOpenKeyExW(r.root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | r.view,
                          &k) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; ++i) {
            wchar_t sub[256];
            DWORD n = 256;
            if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            add(sub);
            for (const wchar_t* val : {L"DisplayName", L"Publisher", L"InstallLocation"}) {
                wchar_t buf[512];
                DWORD size = sizeof(buf);
                if (RegGetValueW(k, sub, val, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) add(buf);
            }
        }
        RegCloseKey(k);
    }
    // программы, у которых нет записи об удалении, но они есть в Program Files или запущены
    for (const wchar_t* pf : {L"%ProgramFiles%", L"%ProgramFiles(x86)%", L"%LOCALAPPDATA%\\Programs"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((Env(pf) + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) add(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const ProcInfo& p : Processes()) add(p.path);
    return w;
}

// Папки, которые нельзя трогать никогда: Windows, браузеры, среды разработки, данные пользователя.
bool Guarded(const std::wstring& lowerName) {
    static const wchar_t* kKeep[] = {L"microsoft",
                                     L"packages",
                                     L"temp",
                                     L"programs",
                                     L"windows",
                                     L"nvidia",
                                     L"intel",
                                     L"amd",
                                     L"google",
                                     L"mozilla",
                                     L"yandex",
                                     L"opera",
                                     L"brave",
                                     L"apple",
                                     L"adobe",
                                     L"jetbrains",
                                     L"code",
                                     L"npm",
                                     L"pip",
                                     L"python",
                                     L"node",
                                     L"git",
                                     L"github",
                                     L"docker",
                                     L"unrealengine",
                                     L"epic",
                                     L"steam",
                                     L"discord",
                                     L"telegram",
                                     L"kaspersky",
                                     L"moondesk",
                                     L"claude",
                                     L"anthropic",
                                     L"asus",
                                     L"realtek",
                                     L"comms",
                                     L"connecteddevicesplatform",
                                     L"crashdumps",
                                     L"d3dscache",
                                     L"fontcache",
                                     L"history",
                                     L"virtualstore",
                                     L"publisher",
                                     L"package cache",
                                     L"ssh",
                                     L"gnupg",
                                     L"history",
                                     L"wsl",
                                     L"uv",
                                     L"pnpm",
                                     L"yarn",
                                     L"nuget",
                                     L"cargo",
                                     L"go-build",
                                     L"gradle",
                                     L"vs code",
                                     L"visual studio",
                                     L"msbuild",
                                     L"ollama",
                                     L"winget"};
    for (const wchar_t* k : kKeep)
        if (lowerName == k || lowerName.rfind(k, 0) == 0) return true;
    return false;
}

}  // namespace

std::vector<Leftover2> AppLeftovers(int idleDays, ULONGLONG minBytes) {
    std::vector<Leftover2> out;
    std::set<std::wstring> words = InstalledWords();
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG cut =
        (((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - (ULONGLONG)idleDays * 24 * 36000000000ull;
    for (const wchar_t* base : {L"%APPDATA%", L"%LOCALAPPDATA%"}) {
        std::wstring root = Env(base);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            std::wstring n = Lower(fd.cFileName);
            if (Guarded(n)) continue;
            // имя папки совпадает с установленной программой или издателем хоть одним словом - она живая
            bool alive = false;
            std::wstring cur;
            for (wchar_t c : n + L" ") {
                if (iswalnum(c)) {
                    cur += c;
                } else {
                    if (cur.size() > 3 && words.count(cur)) alive = true;
                    cur.clear();
                }
            }
            if (alive || words.count(n)) continue;
            std::wstring path = root + L"\\" + fd.cFileName;
            // давно не трогали: самый свежий файл внутри старше idleDays
            ULONGLONG newest = 0, bytes = 0;
            int files = 0;
            std::vector<std::wstring> stack{path};
            while (!stack.empty() && files < 20000) {
                std::wstring d = stack.back();
                stack.pop_back();
                WIN32_FIND_DATAW f2;
                HANDLE h2 =
                    FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &f2, FindExSearchNameMatch, nullptr, 0);
                if (h2 == INVALID_HANDLE_VALUE) continue;
                do {
                    if (f2.cFileName[0] == L'.' && (!f2.cFileName[1] || f2.cFileName[1] == L'.')) continue;
                    if (f2.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        stack.push_back(d + L"\\" + f2.cFileName);
                    } else {
                        ++files;
                        bytes += ((ULONGLONG)f2.nFileSizeHigh << 32) | f2.nFileSizeLow;
                        newest = std::max(newest, ((ULONGLONG)f2.ftLastWriteTime.dwHighDateTime << 32) |
                                                      f2.ftLastWriteTime.dwLowDateTime);
                    }
                } while (FindNextFileW(h2, &f2));
                FindClose(h2);
            }
            if (bytes < minBytes || newest > cut || !newest) continue;
            Leftover2 l;
            l.path = path;
            l.name = fd.cFileName;
            l.bytes = bytes;
            ULONGLONG nowv = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
            l.idleDays = (int)((nowv - newest) / (24 * 36000000000ull));
            out.push_back(l);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const Leftover2& a, const Leftover2& b) { return a.bytes > b.bytes; });
    return out;
}

}  // namespace tools
