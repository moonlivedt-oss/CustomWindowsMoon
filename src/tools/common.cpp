// Общее для RadialMenu.exe и RadialTools.exe: размеры, список кэшей, мусор проектов, кэш Unreal.
// Здесь только чтение: удаляет и завершает отдельный RadialTools.exe (src/act).
#include <windows.h>
#include <bcrypt.h>
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

// Первый мегабайт двух файлов совпадает: разные файлы обычно расходятся уже в начале, и полный SHA-256 гигабайтного
// установщика не нужен.
bool SameHead(const std::wstring& a, const std::wstring& b) {
    auto head = [](const std::wstring& p) {
        std::string s(1 << 20, '\0');
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return std::string();
        DWORD got = 0;
        if (!ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr)) got = 0;
        CloseHandle(f);
        s.resize(got);
        return s;
    };
    std::string x = head(a);
    return !x.empty() && x == head(b);
}

}  // namespace

std::wstring DataDir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\tools", p, MAX_PATH);
    SHCreateDirectoryExW(nullptr, p, nullptr);
    return p;
}

// Ход консольной программы: последний процент в её выводе (RunCapture с percent).
int LastPercent(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char ch : raw)
        if (ch) s += ch;
    for (size_t at = s.rfind('%'); at != std::string::npos && at > 0; at = s.rfind('%', at - 1)) {
        size_t b = at;
        while (b > 0 && (isdigit((unsigned char)s[b - 1]) || s[b - 1] == '.' || s[b - 1] == ',')) --b;
        if (b == at || !isdigit((unsigned char)s[b])) continue;
        int v = atoi(s.c_str() + b);
        if (v >= 0 && v <= 100) return v;
    }
    return -1;
}

std::wstring Bytes(ULONGLONG b) {
    wchar_t buf[32] = L"";
    StrFormatByteSizeW((LONGLONG)b, buf, 32);
    return buf;
}

namespace {

// NtQueryDirectoryFile отдаёт за один вызов столько записей, сколько влезет в буфер, сразу с размерами и временем;
// FindFirstFileEx поверх него делает то же, но с лишним копированием в WIN32_FIND_DATA. Своих имён у структур -
// заголовки MinGW объявляют их не везде.
struct IoStatus {
    union {
        LONG Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};
struct DirInfo {  // FILE_DIRECTORY_INFORMATION
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength;
    WCHAR FileName[1];
};
using NtQueryDir = LONG(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, IoStatus*, PVOID, ULONG, int, BOOLEAN, PVOID, BOOLEAN);
constexpr int kFileDirectoryInformation = 1;
constexpr LONG kNoMoreFiles = (LONG)0x80000006;

NtQueryDir NtQuery() {
    static NtQueryDir f = (NtQueryDir)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile");
    return f;
}

// Одна папка: файлы - в fn, вложенные папки (не ссылки) - в stack. false - открыть не удалось.
// Файлы-ссылки (OneDrive «только в облаке», AppExecLink, symlink) идут в fn со своим EndOfFile - так же их считает
// MFT; папки-ссылки не раскрываются (можно уйти по кругу или в чужой том).
bool ListNt(const std::wstring& d, std::vector<std::wstring>& stack, const std::function<void(const WalkFile&)>& fn) {
    HANDLE h = CreateFileW(d.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    alignas(8) static thread_local BYTE buf[64 * 1024];
    IoStatus io{};
    for (BOOLEAN first = TRUE;; first = FALSE) {
        LONG st = NtQuery()(h, nullptr, nullptr, nullptr, &io, buf, sizeof(buf), kFileDirectoryInformation, FALSE,
                            nullptr, first);
        if (st < 0) break;  // kNoMoreFiles или ошибка - дальше нечего
        for (BYTE* p = buf;;) {
            const DirInfo* e = (const DirInfo*)p;
            const WCHAR* n = e->FileName;
            size_t len = e->FileNameLength / sizeof(WCHAR);
            bool dots = (len == 1 && n[0] == L'.') || (len == 2 && n[0] == L'.' && n[1] == L'.');
            bool link = e->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
            if (!dots && !(link && (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY))) {
                if (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    stack.push_back(d + L"\\" + std::wstring(n, len));
                } else {
                    WalkFile f;
                    f.dir = &d;
                    f.name = n;
                    f.nameLen = len;
                    f.attrs = e->FileAttributes;
                    f.size = (ULONGLONG)e->EndOfFile.QuadPart;
                    f.write = (ULONGLONG)e->LastWriteTime.QuadPart;
                    f.create = (ULONGLONG)e->CreationTime.QuadPart;
                    f.access = (ULONGLONG)e->LastAccessTime.QuadPart;
                    fn(f);
                }
            }
            if (!e->NextEntryOffset) break;
            p += e->NextEntryOffset;
        }
    }
    CloseHandle(h);
    return true;
}

void ListFind(const std::wstring& d, std::vector<std::wstring>& stack, const std::function<void(const WalkFile&)>& fn) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (Dots(fd.cFileName) || LinkDir(fd.dwFileAttributes)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            stack.push_back(d + L"\\" + fd.cFileName);
            continue;
        }
        WalkFile f;
        f.dir = &d;
        f.name = fd.cFileName;
        f.nameLen = wcslen(fd.cFileName);
        f.attrs = fd.dwFileAttributes;
        f.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        f.write = Stamp(fd.ftLastWriteTime);
        f.create = Stamp(fd.ftCreationTime);
        f.access = Stamp(fd.ftLastAccessTime);
        fn(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

// Обход без рекурсии, папки-ссылки (reparse points) не раскрываем - иначе можно уйти по кругу.
void WalkFiles(const std::wstring& root, const std::function<void(const WalkFile&)>& fn, std::atomic<bool>* cancel) {
    std::vector<std::wstring> stack{root};
    while (!stack.empty()) {
        if (cancel && *cancel) break;
        std::wstring d = stack.back();
        stack.pop_back();
        if (!NtQuery() || !ListNt(d, stack, fn)) ListFind(d, stack, fn);
    }
}

ULONGLONG FreeableBytes(const std::wstring& root, std::atomic<bool>* cancel) {
    ULONGLONG sum = 0;
    WalkFiles(
        root,
        [&](const WalkFile& f) {
            if (!CloudOnly(f.attrs)) sum += f.size;
        },
        cancel);
    return sum;
}

// Размер папки целиком.
ULONGLONG DirBytes(const std::wstring& root, int* files, std::atomic<bool>* cancel, std::atomic<long>* progress) {
    ULONGLONG sum = 0;
    WalkFiles(
        root,
        [&](const WalkFile& f) {
            sum += f.size;
            if (files) ++*files;
            if (progress) progress->fetch_add(1, std::memory_order_relaxed);
        },
        cancel);
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
        L"svchost.exe",       L"csrss.exe",    L"lsass.exe",    L"services.exe", L"smss.exe",
        L"wininit.exe",       L"winlogon.exe", L"dwm.exe",      L"conhost.exe",  L"rundll32.exe",
        L"taskhostw.exe",     L"spoolsv.exe",  L"sihost.exe",   L"dllhost.exe",  L"ctfmon.exe",
        L"runtimebroker.exe", L"wmiprvse.exe", L"audiodg.exe",  L"explorer.exe", L"fontdrvhost.exe",
        L"searchindexer.exe", L"lsaiso.exe",   L"regsvr32.exe", L"userinit.exe"};
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

std::wstring FileSha256(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::wstring out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<BYTE> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
            ok = BCryptHashData(hh, buf.data(), got, 0) == 0;
        BYTE hash[32];
        if (ok && BCryptFinishHash(hh, hash, 32, 0) == 0) {
            wchar_t hx[3];
            for (BYTE b : hash) swprintf(hx, 3, L"%02x", b), out += hx;
        }
    }
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// ---------------- кэши ----------------

namespace {

// TEMP делим на две части по тому же правилу, что у RadialTools (TempRemovable): что чистка удалит (bytes) и что
// оставит до следующего раза (fresh). Иначе «можно освободить» обещает больше, чем освободится.
void TempBytes(Junk& j) {
    ULONGLONG now = NowStamp();
    WalkFiles(j.path, [&](const WalkFile& f) {
        if (!CloudOnly(f.attrs)) (TempRemovable(f, now) ? j.bytes : j.fresh) += f.size;
    });
}

}  // namespace

CleanLine ParseCleanLine(const std::wstring& s) {
    CleanLine c;
    if (s.empty() || s[0] != L'#') return c;
    std::vector<std::wstring> p = Split(s, L'\t');
    auto index = [](const std::wstring& v) {
        return !v.empty() && iswdigit(v[0]) ? (size_t)_wcstoui64(v.c_str(), nullptr, 10) : (size_t)-1;
    };
    if (p.size() == 2 && p[0] == L"#start") {
        c.kind = CleanLine::START;
        c.index = index(p[1]);
    } else if (p.size() == 3 && p[0] == L"#freed") {
        c.kind = CleanLine::FREED;
        c.index = index(p[1]);
        c.bytes = _wcstoui64(p[2].c_str(), nullptr, 10);
    } else if (s.rfind(L"#busy\t", 0) == 0) {
        c.kind = CleanLine::BUSY;
        c.text = s.substr(6);  // путь может содержать что угодно, кроме перевода строки
    }
    return c;
}

ULONGLONG ParseStoreSize(const std::wstring& dismOut) {
    // «Actual Size of Component Store : 7.89 GB» / «Фактический размер хранилища компонентов : 7,89 ГБ»
    for (const std::wstring& raw : Split(dismOut, L'\n')) {
        std::wstring line = Lower(raw);
        if (line.find(L"actual size") == std::wstring::npos && line.find(L"фактический размер") == std::wstring::npos)
            continue;
        size_t colon = line.rfind(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring num;
        size_t i = colon + 1;
        while (i < line.size() && iswspace(line[i])) ++i;
        for (; i < line.size() && (iswdigit(line[i]) || line[i] == L',' || line[i] == L'.'); ++i)
            num += line[i] == L',' ? L'.' : line[i];
        if (num.empty()) continue;
        while (i < line.size() && iswspace(line[i])) ++i;
        double mul = 1;
        wchar_t u = i < line.size() ? line[i] : 0;
        if (u == L'k' || u == L'к') mul = 1024.0;
        if (u == L'm' || u == L'м') mul = 1024.0 * 1024;
        if (u == L'g' || u == L'г') mul = 1024.0 * 1024 * 1024;
        if (u == L't' || u == L'т') mul = 1024.0 * 1024 * 1024 * 1024;
        return (ULONGLONG)(_wtof(num.c_str()) * mul);
    }
    return 0;
}

bool FileInUse(const std::wstring& path) {
    // без общего доступа файл открывается, только если его больше никто не держит; право DELETE - что удалить его
    // нам вообще можно (файлы, оставленные установщиком с правами администратора, только для чтения). Держим
    // микросекунды.
    HANDLE h = CreateFileW(path.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return true;  // занят или удалить нельзя - в обоих случаях не наш
    CloseHandle(h);
    return false;
}

bool TempRemovable(const WalkFile& f, ULONGLONG now) {
    ULONGLONG touched = std::max(f.write, f.create), age = now > touched ? now - touched : 0;
    const ULONGLONG hour = kDay / 24;
    std::wstring n(f.name, f.nameLen);
    bool pch =
        n.size() > 13 && _wcsnicmp(n.c_str(), L"preamble-", 9) == 0 && _wcsicmp(n.c_str() + n.size() - 4, L".pch") == 0;
    if (age < (pch ? hour : 6 * hour)) return false;
    // занятый не удалится всё равно, и обещать его в «можно освободить» нечестно - в любом возрасте
    return !FileInUse(f.Path());
}

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
        if (_wcsicmp(j.path.c_str(), TempDir().c_str()) == 0)
            TempBytes(j);
        else
            j.bytes = FreeableBytes(j.path);
        if (j.bytes || j.fresh) out.push_back(j);
    }
    std::sort(out.begin(), out.end(), [](const Junk& a, const Junk& b) { return a.bytes > b.bytes; });
    return out;
}

std::wstring TempDir() { return Env(L"%TEMP%"); }

// ---------------- кэш Unreal ----------------

std::wstring DdcDir() { return Env(L"%LOCALAPPDATA%\\UnrealEngine\\Common\\DerivedDataCache"); }

ULONGLONG OldBytes(const std::wstring& root, int days, int* files) {
    ULONGLONG cutoff = NowStamp() - (ULONGLONG)days * kDay, sum = 0;
    WalkFiles(root, [&](const WalkFile& f) {
        if (std::max(f.access, f.write) >= cutoff || CloudOnly(f.attrs)) return;
        sum += f.size;
        if (files) ++*files;
    });
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

std::vector<ProjectJunk> FindProjectJunk(const std::vector<std::wstring>& roots, std::atomic<bool>* cancel) {
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
        j.bytes = FreeableBytes(j.path, cancel);  // проект в OneDrive: облачное место на диске не займёт
        if (j.bytes < 1024 * 1024) continue;      // мелочь не показываем
        auto it = idle.find(j.project);
        j.idleDays = it != idle.end() ? it->second : (idle[j.project] = IdleDays(j.project));
        sized.push_back(j);
    }
    std::sort(sized.begin(), sized.end(), [](const ProjectJunk& a, const ProjectJunk& b) { return a.bytes > b.bytes; });
    return sized;
}

// ---------------- разбор Загрузок ----------------

// Прогноз места (И10): прямая по замерам последних 21 дня; здесь, а не в health.cpp, - чтобы проверяли тесты.
int DaysLeft(const std::vector<FreePoint>& ptsIn, double* perDay) {
    if (perDay) *perDay = 0;
    if (ptsIn.empty()) return -1;
    int last = ptsIn.back().day;
    for (const FreePoint& p : ptsIn) last = std::max(last, p.day);
    std::vector<FreePoint> pts;
    for (const FreePoint& p : ptsIn)
        if (p.day > last - 21) pts.push_back(p);
    if (pts.size() < 3) return -1;
    double n = (double)pts.size(), sx = 0, sy = 0, sxx = 0, sxy = 0;
    int first = last;
    for (const FreePoint& p : pts) first = std::min(first, p.day);
    if (last - first < 2) return -1;
    for (const FreePoint& p : pts) {
        double x = p.day - first, y = (double)p.free;
        sx += x, sy += y, sxx += x * x, sxy += x * y;
    }
    double den = n * sxx - sx * sx;
    if (den <= 0) return -1;
    double slope = (n * sxy - sx * sy) / den;  // байт в сутки, убывает - отрицательный
    if (perDay) *perDay = -slope;
    if (-slope < 200.0 * 1024 * 1024) return -1;
    ULONGLONG now = 0;
    for (const FreePoint& p : pts)
        if (p.day == last) now = p.free;
    return (int)std::min(3650.0, now / -slope);
}

// Системные гиганты (И11): здесь, а не в giants.cpp, - чтобы проверяли тесты.
ULONGLONG ParseShadowUsed(const std::wstring& text) {
    // vssadmin list shadowstorage: «Used Shadow Copy Storage space: 2.53 GB (0%)» или по-русски «Используемое ...:
    // 2,53 GB (0%)» - первая строка с размером и процентом в скобках
    for (const std::wstring& line : Split(text, L'\n')) {
        size_t pct = line.find(L"%)"), colon = line.find(L':');
        if (pct == std::wstring::npos || colon == std::wstring::npos || colon > pct) continue;
        std::wstring v = Trim(line.substr(colon + 1));
        double num = 0;
        size_t i = 0;
        std::wstring digits;
        for (; i < v.size() && (iswdigit(v[i]) || v[i] == L'.' || v[i] == L',' || v[i] == L' ' || v[i] == 0xA0); ++i)
            if (v[i] != L' ' && v[i] != 0xA0) digits += v[i] == L',' ? L'.' : v[i];
        if (digits.empty()) continue;
        num = _wtof(digits.c_str());
        std::wstring unit = Lower(Trim(v.substr(i, 3)));
        double mul = 1;
        if (unit.rfind(L"kb", 0) == 0 || unit.rfind(L"кб", 0) == 0) mul = 1024.0;
        if (unit.rfind(L"mb", 0) == 0 || unit.rfind(L"мб", 0) == 0) mul = 1024.0 * 1024;
        if (unit.rfind(L"gb", 0) == 0 || unit.rfind(L"гб", 0) == 0) mul = 1024.0 * 1024 * 1024;
        if (unit.rfind(L"tb", 0) == 0 || unit.rfind(L"тб", 0) == 0) mul = 1024.0 * 1024 * 1024 * 1024;
        return (ULONGLONG)(num * mul);
    }
    return 0;
}

std::wstring BigFileKeep(const std::wstring& path) {
    std::wstring l = Lower(path);
    if (l.size() < 4 || l[1] != L':' || l[2] != L'\\') return L"не полный путь";
    size_t slash = l.find(L'\\', 3);
    if (slash == std::wstring::npos) return L"файл в корне диска - системный";  // pagefile.sys, hiberfil.sys
    std::wstring top = l.substr(3, slash - 3);
    if (top == L"windows" || top == L"$recycle.bin" || top == L"system volume information" || top == L"recovery" ||
        top == L"$windows.~bt" || top == L"$windows.~ws" || top == L"windows.old")
        return L"системная папка - освобождается через «Системные гиганты»";
    if (top == L"program files" || top == L"program files (x86)" || top == L"programdata")
        return L"часть установленной программы - удалять через её деинсталлятор";
    for (const wchar_t* part : {L"\\steamapps\\common\\", L"\\epic games\\", L"\\moonradial\\",
                                L"\\appdata\\local\\packages\\", L"\\windowsapps\\", L"\\.git\\"})
        if (l.find(part) != std::wstring::npos) return L"часть программы, игры или репозитория";
    for (const wchar_t* ext : {L".vhdx", L".vhd", L".vmdk"})
        if (EndsWithI(l, ext)) return L"диск виртуальной машины или WSL - через «Docker и WSL»";
    for (const wchar_t* ext : {L".sys", L".dll", L".exe"})
        if (EndsWithI(l, ext)) return L"программа или системный файл";
    return L"";
}

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
                // одинаковый размер - ещё не копия (две версии документа бывают одного размера): сверяем содержимое
                if (it != sizeByName.end() && it->second == f.bytes &&
                    SameHead(dir + L"\\" + f.name, dir + L"\\" + orig)) {
                    std::wstring a = FileSha256(dir + L"\\" + f.name), b = FileSha256(dir + L"\\" + orig);
                    if (!a.empty() && a == b) why = L"копия";
                }
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
