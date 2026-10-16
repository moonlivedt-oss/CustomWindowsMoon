// Общее для RadialMenu.exe и RadialTools.exe: размеры, список кэшей, мусор проектов, кэш Unreal.
// Здесь только чтение: удаляет и завершает отдельный RadialTools.exe (src/act).
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <map>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

std::wstring Env(const wchar_t* s) {
    wchar_t b[MAX_PATH];
    ExpandEnvironmentStringsW(s, b, MAX_PATH);
    return b;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Dots(const wchar_t* n) { return n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2])); }

bool HasExt(const std::wstring& dir, const wchar_t* ext) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*" + ext).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

ULONGLONG Stamp(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

ULONGLONG NowStamp() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return Stamp(ft);
}

constexpr ULONGLONG kDay = 24ull * 3600 * 10000000;

}  // namespace

std::wstring DataDir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\tools", p, MAX_PATH);
    SHCreateDirectoryExW(nullptr, p, nullptr);
    return p;
}

std::wstring Bytes(ULONGLONG b) {
    wchar_t buf[32] = L"";
    StrFormatByteSizeW((LONGLONG)b, buf, 32);
    return buf;
}

// Размер папки целиком: обход без рекурсии, ссылки (reparse points) не раскрываем - иначе можно уйти по кругу.
ULONGLONG DirBytes(const std::wstring& root, int* files, volatile bool* cancel, volatile long* progress) {
    ULONGLONG sum = 0;
    std::vector<std::wstring> stack{root};
    while (!stack.empty()) {
        if (cancel && *cancel) break;
        std::wstring d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (Dots(fd.cFileName)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(d + L"\\" + fd.cFileName);
            } else {
                sum += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (files) ++*files;
                if (progress) InterlockedIncrement(progress);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return sum;
}

bool ProtectedExe(const std::wstring& exe) {
    static const wchar_t* kSkip[] = {L"system",
                                     L"registry",
                                     L"smss.exe",
                                     L"csrss.exe",
                                     L"wininit.exe",
                                     L"winlogon.exe",
                                     L"services.exe",
                                     L"lsass.exe",
                                     L"svchost.exe",
                                     L"dwm.exe",
                                     L"fontdrvhost.exe",
                                     L"memory compression",
                                     L"msmpeng.exe",
                                     L"avp.exe",
                                     L"avpui.exe",
                                     L"secure system",
                                     L"radialmenu.exe",
                                     L"radialtools.exe",
                                     L"sihost.exe",
                                     L"ctfmon.exe",
                                     L"explorer.exe",
                                     L"searchhost.exe",
                                     L"startmenuexperiencehost.exe",
                                     L"audiodg.exe",
                                     L"lsaiso.exe",
                                     L"vmmem",
                                     L"vmmemwsl",
                                     L"textinputhost.exe",
                                     L"shellexperiencehost.exe",
                                     L"runtimebroker.exe",
                                     L"conhost.exe"};
    std::wstring n = Lower(exe);
    for (const wchar_t* s : kSkip)
        if (n == s) return true;
    return false;
}

const std::vector<std::wstring>& SystemExeNames() {
    static const std::vector<std::wstring> names = {
        L"svchost.exe",   L"csrss.exe",       L"lsass.exe",         L"services.exe",   L"smss.exe",
        L"wininit.exe",   L"winlogon.exe",    L"dwm.exe",           L"conhost.exe",    L"rundll32.exe",
        L"taskhostw.exe", L"spoolsv.exe",     L"sihost.exe",        L"dllhost.exe",    L"ctfmon.exe",
        L"runtimebroker.exe", L"wmiprvse.exe", L"audiodg.exe",      L"explorer.exe",   L"fontdrvhost.exe",
        L"searchindexer.exe", L"lsaiso.exe",  L"regsvr32.exe",      L"userinit.exe"};
    return names;
}

bool InWindowsDir(const std::wstring& path) {
    static const std::wstring win = Lower(Env(L"%SystemRoot%"));
    std::wstring l = Lower(path);
    std::wstring d = l.substr(0, l.find_last_of(L'\\') == std::wstring::npos ? 0 : l.find_last_of(L'\\'));
    return d == win || d == win + L"\\system32" || d == win + L"\\syswow64" || d == win + L"\\system32\\wbem" ||
           d == win + L"\\syswow64\\wbem";
}

bool SystemImposter(const std::wstring& fullPath) {
    std::wstring leaf = Lower(fullPath.substr(fullPath.find_last_of(L'\\') + 1));
    const std::vector<std::wstring>& n = SystemExeNames();
    return std::find(n.begin(), n.end(), leaf) != n.end() && !InWindowsDir(fullPath);
}

// ---------------- кэши ----------------

std::vector<Junk> FindJunk(bool sizes) {
    // slow - пересоздаётся дорого (шейдеры собираются заново, первые запуски игр и редактора тормозят):
    // в «Всё сразу» не входит, только по отдельности
    struct Entry {
        const wchar_t *name, *path;
        bool slow;
    };
    static const Entry list[] = {
        {L"Временные файлы", L"%TEMP%", false},
        {L"Кэш npm", L"%LOCALAPPDATA%\\npm-cache", false},
        {L"Кэш pnpm", L"%LOCALAPPDATA%\\pnpm-cache", false},
        {L"Кэш pip", L"%LOCALAPPDATA%\\pip\\Cache", false},
        {L"Кэш uv", L"%LOCALAPPDATA%\\uv\\cache", false},
        {L"Кэш Yarn", L"%LOCALAPPDATA%\\Yarn\\Cache", false},
        {L"Кэш NuGet", L"%LOCALAPPDATA%\\NuGet\\v3-cache", false},
        {L"Кэш Go", L"%LOCALAPPDATA%\\go-build", false},
        {L"Кэш Gradle", L"%USERPROFILE%\\.gradle\\caches", false},
        {L"Архивы Cargo", L"%USERPROFILE%\\.cargo\\registry\\cache", false},
        {L"Аварийные дампы", L"%LOCALAPPDATA%\\CrashDumps", false},
        {L"Отчёты об ошибках", L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER\\ReportArchive", false},
        {L"Кэш шейдеров DirectX", L"%LOCALAPPDATA%\\D3DSCache", true},
        {L"Кэш шейдеров NVIDIA", L"%LOCALAPPDATA%\\NVIDIA\\DXCache", true},
        {L"Кэш Unreal Engine", L"%LOCALAPPDATA%\\UnrealEngine\\Common\\DerivedDataCache", true},
    };
    std::vector<Junk> out;
    for (const Entry& e : list) {
        Junk j;
        j.name = e.name;
        j.slow = e.slow;
        j.path = Env(e.path);
        if (!IsDir(j.path)) continue;
        if (!sizes) {
            out.push_back(j);
            continue;
        }
        j.bytes = DirBytes(j.path, nullptr, nullptr, nullptr);
        if (j.bytes) out.push_back(j);
    }
    std::sort(out.begin(), out.end(), [](const Junk& a, const Junk& b) { return a.bytes > b.bytes; });
    return out;
}

std::wstring TempDir() { return Env(L"%TEMP%"); }

// ---------------- кэш Unreal ----------------

std::wstring DdcDir() { return Env(L"%LOCALAPPDATA%\\UnrealEngine\\Common\\DerivedDataCache"); }

ULONGLONG OldBytes(const std::wstring& root, int days, int* files) {
    ULONGLONG cutoff = NowStamp() - (ULONGLONG)days * kDay, sum = 0;
    std::vector<std::wstring> stack{root};
    while (!stack.empty()) {
        std::wstring d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (Dots(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(d + L"\\" + fd.cFileName);
            } else if (std::max(Stamp(fd.ftLastAccessTime), Stamp(fd.ftLastWriteTime)) < cutoff) {
                sum += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (files) ++*files;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return sum;
}

// ---------------- мусор проектов ----------------

namespace {

// Что считается пересобираемым мусором проекта и по какому признаку рядом это видно.
// Признак обязателен: папка build без CMakeCache.txt может оказаться чьими-то файлами.
const wchar_t* JunkKind(const std::wstring& parent, const std::wstring& name) {
    std::wstring n = Lower(name), self = parent + L"\\" + name;
    if (n == L"node_modules") return IsFile(parent + L"\\package.json") ? L"node_modules" : nullptr;
    if (n == L".next" || n == L".nuxt" || n == L".parcel-cache" || n == L".turbo" || n == L".svelte-kit" ||
        n == L".angular")
        return IsFile(parent + L"\\package.json") ? L"кэш сборки JS" : nullptr;
    if (n == L"intermediate" || n == L"deriveddatacache")
        return HasExt(parent, L".uproject") || HasExt(parent, L".uplugin") ? L"Unreal: промежуточные" : nullptr;
    if (n == L"target") return IsFile(parent + L"\\Cargo.toml") ? L"Rust target" : nullptr;
    if (n == L".venv" || n == L"venv" || n == L"env") return IsFile(self + L"\\pyvenv.cfg") ? L"Python venv" : nullptr;
    if (n.rfind(L"build", 0) == 0 || n.rfind(L"cmake-build-", 0) == 0 || n == L"out")
        if (IsFile(self + L"\\CMakeCache.txt")) return L"сборка CMake";
    if (n == L"build" || n == L".gradle")
        if (IsFile(parent + L"\\build.gradle") || IsFile(parent + L"\\build.gradle.kts") ||
            IsFile(parent + L"\\settings.gradle") || IsFile(parent + L"\\settings.gradle.kts"))
            return L"сборка Gradle";
    if (n == L"obj") return HasExt(parent, L".csproj") ? L".NET obj" : nullptr;
    return nullptr;
}

bool SkipWalk(const std::wstring& name) {
    std::wstring n = Lower(name);
    return n == L".git" || n == L".svn" || n == L".hg" || n == L"$recycle.bin" || n == L"system volume information" ||
           n == L"windows" || n == L"program files" || n == L"program files (x86)" || n == L"appdata";
}

// Когда в проекте последний раз что-то меняли: самые свежие файлы в нём и на уровень глубже, без мусора.
int IdleDays(const std::wstring& dir) {
    ULONGLONG newest = 0;
    std::vector<std::pair<std::wstring, int>> stack{{dir, 0}};
    int seen = 0;
    while (!stack.empty() && seen < 4000) {
        auto [d, depth] = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (Dots(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            ++seen;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (depth < 2 && !SkipWalk(fd.cFileName) && !JunkKind(d, fd.cFileName) &&
                    _wcsicmp(fd.cFileName, L"Saved") != 0)
                    stack.push_back({d + L"\\" + fd.cFileName, depth + 1});
            } else {
                newest = std::max(newest, Stamp(fd.ftLastWriteTime));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!newest) return 0;
    ULONGLONG now = NowStamp();
    return now > newest ? (int)((now - newest) / kDay) : 0;
}

}  // namespace

bool IsProjectJunk(const std::wstring& pathIn, std::wstring* kind) {
    std::wstring path = pathIn;
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    size_t cut = path.find_last_of(L'\\');
    if (cut == std::wstring::npos || cut < 3 || !IsDir(path)) return false;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    const wchar_t* k = JunkKind(path.substr(0, cut), path.substr(cut + 1));
    if (k && kind) *kind = k;
    return k != nullptr;
}

std::vector<ProjectJunk> FindProjectJunk(const std::vector<std::wstring>& roots, volatile bool* cancel) {
    std::vector<ProjectJunk> out;
    std::vector<std::pair<std::wstring, int>> stack;
    for (const std::wstring& r : roots)
        if (IsDir(r)) stack.push_back({r, 0});
    int visited = 0;
    std::map<std::wstring, int> idle;
    while (!stack.empty() && visited < 40000) {
        if (cancel && *cancel) break;
        auto [d, depth] = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Dots(fd.cFileName) ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            ++visited;
            std::wstring p = d + L"\\" + fd.cFileName;
            if (const wchar_t* k = JunkKind(d, fd.cFileName)) {
                ProjectJunk j;
                j.path = p;
                j.kind = k;
                j.project = d;
                j.name = d.substr(d.find_last_of(L'\\') + 1);
                out.push_back(j);
                continue;  // внутрь мусора не идём
            }
            if (depth < 5 && !SkipWalk(fd.cFileName) && fd.cFileName[0] != L'.') stack.push_back({p, depth + 1});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // корни могут пересекаться: одна и та же папка - один раз
    std::sort(out.begin(), out.end(),
              [](const ProjectJunk& a, const ProjectJunk& b) { return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const ProjectJunk& a, const ProjectJunk& b) {
                              return _wcsicmp(a.path.c_str(), b.path.c_str()) == 0;
                          }),
              out.end());
    std::vector<ProjectJunk> sized;
    for (ProjectJunk& j : out) {
        if (cancel && *cancel) break;
        j.bytes = DirBytes(j.path, nullptr, cancel, nullptr);
        if (j.bytes < 1024 * 1024) continue;  // мелочь не показываем
        auto it = idle.find(j.project);
        j.idleDays = it != idle.end() ? it->second : (idle[j.project] = IdleDays(j.project));
        sized.push_back(j);
    }
    std::sort(sized.begin(), sized.end(), [](const ProjectJunk& a, const ProjectJunk& b) { return a.bytes > b.bytes; });
    return sized;
}

// ---------------- разбор Загрузок ----------------

std::wstring DownloadsDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::vector<DownloadJunk> FindDownloadJunk(int days) {
    std::vector<DownloadJunk> out;
    std::wstring dir = DownloadsDir();
    if (dir.empty()) return out;
    ULONGLONG now = NowStamp(), old = now - (ULONGLONG)days * kDay, week = now - 7 * kDay, two = now - 2 * kDay;
    // всё содержимое корня Загрузок: имя (без регистра) -> размер, и какие там папки
    struct F {
        std::wstring name;
        ULONGLONG bytes, time;
    };
    std::vector<F> files;
    std::map<std::wstring, ULONGLONG> sizeByName;
    std::map<std::wstring, bool> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (Dots(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dirs[Lower(fd.cFileName)] = true;
            continue;
        }
        ULONGLONG b = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        // скачанное: время создания - когда легло в папку
        files.push_back({fd.cFileName, b, std::max(Stamp(fd.ftCreationTime), Stamp(fd.ftLastWriteTime))});
        sizeByName[Lower(fd.cFileName)] = b;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    auto ext = [](const std::wstring& n) {
        size_t d = n.find_last_of(L'.');
        return d == std::wstring::npos ? std::wstring() : Lower(n.substr(d));
    };
    auto stem = [](const std::wstring& n) {
        std::wstring l = Lower(n);
        if (EndsWithI(l, L".tar.gz")) return l.substr(0, l.size() - 7);
        size_t d = l.find_last_of(L'.');
        return d == std::wstring::npos ? l : l.substr(0, d);
    };
    for (const F& f : files) {
        std::wstring e = ext(f.name), why;
        if ((e == L".exe" || e == L".msi" || e == L".msix" || e == L".appx" || e == L".msixbundle") && f.time < old) {
            why = L"установщик";
        } else if ((e == L".crdownload" || e == L".part" || e == L".partial" || e == L".tmp") && f.time < two) {
            why = L"недокачанный";
        } else if (e == L".torrent" && f.time < week) {
            why = L"торрент-файл";
        } else if ((e == L".zip" || e == L".rar" || e == L".7z" || EndsWithI(f.name, L".tar.gz")) && f.time < old) {
            // архив, который уже распакован рядом в папку с тем же именем
            if (dirs.count(stem(f.name))) why = L"уже распакован";
        }
        if (why.empty()) {
            // копия: «файл (1).zip», а рядом такой же «файл.zip» того же размера
            std::wstring s = stem(f.name);
            size_t br = s.rfind(L" (");
            if (br != std::wstring::npos && s.back() == L')') {
                std::wstring orig = s.substr(0, br) + e;
                auto it = sizeByName.find(orig);
                if (it != sizeByName.end() && it->second == f.bytes) why = L"копия";
            }
        }
        if (why.empty()) continue;
        DownloadJunk d;
        d.path = dir + L"\\" + f.name;
        d.name = f.name;
        d.why = why;
        d.bytes = f.bytes;
        d.days = now > f.time ? (int)((now - f.time) / kDay) : 0;
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const DownloadJunk& a, const DownloadJunk& b) { return a.bytes > b.bytes; });
    return out;
}

}  // namespace tools
