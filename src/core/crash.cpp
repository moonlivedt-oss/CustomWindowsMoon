#include "core/crash.h"

#include <dbghelp.h>
#include <shlobj.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cwchar>
#include <exception>

namespace crash {

namespace {

typedef BOOL(WINAPI* WriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// всё нужное для записи - заранее: в момент падения куча может быть испорчена, грузить dbghelp поздно
wchar_t g_dir[MAX_PATH] = L"";
WriteDumpFn g_write = nullptr;
volatile LONG g_once = 0;
constexpr DWORD kTerminate = 0xE0000A11;  // свой код: std::terminate и abort идут через тот же обработчик

[[noreturn]] void RaiseTerminate() {
    RaiseException(kTerminate, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

const wchar_t* CodeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return L"обращение к памяти";
        case EXCEPTION_STACK_OVERFLOW:
            return L"переполнение стека";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return L"деление на ноль";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return L"недопустимая инструкция";
        case EXCEPTION_IN_PAGE_ERROR:
            return L"ошибка чтения страницы";
        case 0xC0000409:
            return L"порча стека или abort";
        case 0xE06D7363:
            return L"исключение C++";
        case kTerminate:
            return L"необработанное исключение C++ или abort";
        default:
            return L"исключение";
    }
}

// «C0000005 (обращение к памяти) в RadialMenu.exe+0x1a2b3» - без кучи, в буфер на стеке
void Describe(EXCEPTION_POINTERS* ep, wchar_t* out, size_t n) {
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (addr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod))
        GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    // метка сборки (TimeDateStamp заголовка PE) - по ней --crash-decode найдёт карту символов той самой сборки
    DWORD stamp = 0;
    if (mod) {
        auto* dos = (const IMAGE_DOS_HEADER*)mod;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
            stamp = ((const IMAGE_NT_HEADERS*)((const BYTE*)mod + dos->e_lfanew))->FileHeader.TimeDateStamp;
    }
    swprintf(out, n, L"%08lX (%ls) в %ls+0x%llx, сборка %08lX", (unsigned long)code, CodeName(code), leaf,
             (unsigned long long)((ULONG_PTR)addr - (ULONG_PTR)mod), (unsigned long)stamp);
}

wchar_t g_restartExe[MAX_PATH] = {};
wchar_t g_restartCmd[MAX_PATH + 32] = {};
volatile LONG g_restart = 0;
ULONGLONG g_startTick = GetTickCount64();

// Новая копия после падения - только если жили дольше минуты (не петля падений на старте) и прошлый перезапуск
// был больше 10 минут назад (время - в HKCU, переживает сами перезапуски).
void RestartAfterCrash() {
    if (!g_restart || GetTickCount64() - g_startTick < 60000) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime, last = 0;
    DWORD size = sizeof(last);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\MoonRadial", L"CrashRestartAt", RRF_RT_REG_QWORD, nullptr, &last,
                 &size);
    if (last && now - last < 10ull * 60 * 10000000) return;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\MoonRadial", L"CrashRestartAt", REG_QWORD, &now, sizeof(now));
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(g_restartExe, g_restartCmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_once, 1) || !g_dir[0]) return EXCEPTION_CONTINUE_SEARCH;  // второе падение - не пишем
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t base[MAX_PATH + 64];
    swprintf(base, MAX_PATH + 64, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u-%lu", g_dir, t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    wchar_t path[MAX_PATH + 80];
    if (g_write) {
        swprintf(path, MAX_PATH + 80, L"%ls.dmp", base);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mi{GetCurrentThreadId(), ep, FALSE};
            g_write(GetCurrentProcess(), GetCurrentProcessId(), f,
                    (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData),
                    ep ? &mi : nullptr, nullptr, nullptr);
            CloseHandle(f);
        }
    }
    // рядом - строка «что и где»: её прочитает следующий запуск, не разбирая дамп
    swprintf(path, MAX_PATH + 80, L"%ls.txt", base);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        wchar_t what[512];
        Describe(ep, what, 512);
        char utf8[1024];
        int len = WideCharToMultiByte(CP_UTF8, 0, what, -1, utf8, sizeof(utf8), nullptr, nullptr);
        DWORD wr = 0;
        if (len > 1) WriteFile(f, utf8, (DWORD)(len - 1), &wr, nullptr);
        CloseHandle(f);
    }
    RestartAfterCrash();
    return EXCEPTION_EXECUTE_HANDLER;  // дальше Windows закроет процесс без окна «программа не отвечает»
}

std::vector<std::wstring> Dumps(const std::wstring& dir) {  // имена crash-*.dmp по возрастанию (= по времени)
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\crash-*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::wstring Stem(const std::wstring& name) { return name.substr(0, name.size() - 4); }  // без .dmp

std::string ReadSmall(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    char buf[2048];
    DWORD got = 0;
    if (!ReadFile(f, buf, sizeof(buf), &got, nullptr)) got = 0;
    CloseHandle(f);
    return std::string(buf, got);
}

std::wstring Widen8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

}  // namespace

std::wstring Dir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\crash", p, MAX_PATH);
    SHCreateDirectoryExW(nullptr, p, nullptr);
    return p;
}

void Install(const std::wstring& dirIn) {
    std::wstring dir = dirIn.empty() ? Dir() : dirIn;
    wcsncpy(g_dir, dir.c_str(), MAX_PATH - 1);
    // только из System32: dbghelp.dll рядом с exe мог бы подложить кто угодно
    if (HMODULE dh = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        g_write = (WriteDumpFn)(void*)GetProcAddress(dh, "MiniDumpWriteDump");
    SetUnhandledExceptionFilter(Filter);
    std::set_terminate([] { RaiseTerminate(); });  // иначе MinGW молча зовёт abort - ни дампа, ни строки
    signal(SIGABRT, [](int) { RaiseTerminate(); });
    Prune(dir, kKeep - 1);  // место под ещё один: дампов не станет больше kKeep
}

void SetRestart(bool on) {
    if (on && !g_restartExe[0]) {
        GetModuleFileNameW(nullptr, g_restartExe, MAX_PATH);
        swprintf(g_restartCmd, MAX_PATH + 32, L"\"%ls\" --after-crash", g_restartExe);
    }
    InterlockedExchange(&g_restart, on ? 1 : 0);
}

void Prune(const std::wstring& dir, int keep) {
    std::vector<std::wstring> d = Dumps(dir);
    for (size_t i = 0; i + keep < d.size(); ++i) {
        DeleteFileW((dir + L"\\" + d[i]).c_str());
        DeleteFileW((dir + L"\\" + Stem(d[i]) + L".txt").c_str());
    }
}

std::vector<Report> All(const std::wstring& dir) {
    std::vector<Report> out;
    for (const std::wstring& name : Dumps(dir)) {
        Report r;
        r.dump = dir + L"\\" + name;
        if (name.size() >= 21)
            r.when =
                name.substr(12, 2) + L"." + name.substr(10, 2) + L" " + name.substr(15, 2) + L":" + name.substr(17, 2);
        r.what = Widen8(ReadSmall(dir + L"\\" + Stem(name) + L".txt"));
        out.push_back(r);
    }
    return out;
}

std::vector<Report> Unreported(const std::wstring& dir, bool mark) {
    std::vector<Report> out;
    std::wstring markFile = dir + L"\\reported";
    std::wstring last = Widen8(ReadSmall(markFile));
    while (!last.empty() && (last.back() == L'\n' || last.back() == L'\r')) last.pop_back();
    std::vector<std::wstring> d = Dumps(dir);
    for (const std::wstring& name : d) {
        if (name <= last) continue;
        Report r;
        r.dump = dir + L"\\" + name;
        // crash-20261018-140501-1234.dmp -> «18.10 14:05»
        if (name.size() >= 21)
            r.when =
                name.substr(12, 2) + L"." + name.substr(10, 2) + L" " + name.substr(15, 2) + L":" + name.substr(17, 2);
        r.what = Widen8(ReadSmall(dir + L"\\" + Stem(name) + L".txt"));
        out.push_back(r);
    }
    if (mark && !d.empty() && d.back() > last) {
        HANDLE f =
            CreateFileW(markFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            int n = WideCharToMultiByte(CP_UTF8, 0, d.back().c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, d.back().c_str(), -1, s.data(), n, nullptr, nullptr);
            DWORD wr = 0;
            WriteFile(f, s.data(), (DWORD)s.size(), &wr, nullptr);
            CloseHandle(f);
        }
    }
    return out;
}

// --- разбор адреса по карте символов (Н10) ---

bool ParseWhere(const std::wstring& what, std::wstring& module, ULONGLONG& offset, DWORD& stamp) {
    size_t plus = what.find(L"+0x");
    if (plus == std::wstring::npos) return false;
    size_t a = what.find_last_of(L" \\/", plus);
    module = what.substr(a == std::wstring::npos ? 0 : a + 1, plus - (a == std::wstring::npos ? 0 : a + 1));
    offset = wcstoull(what.c_str() + plus + 3, nullptr, 16);
    stamp = 0;
    size_t b = what.find(L"сборка ");
    if (b != std::wstring::npos) stamp = wcstoul(what.c_str() + b + 7, nullptr, 16);
    return !module.empty() && offset != 0;
}

std::wstring Symbolize(const std::string& map, ULONGLONG addr) {
    // строки nm --numeric-sort: «0000000140001000 T имя»; берём ближайший код (T/t) не выше адреса
    ULONGLONG best = 0;
    std::string name;
    size_t pos = 0;
    while (pos < map.size()) {
        size_t e = map.find('\n', pos);
        if (e == std::string::npos) e = map.size();
        const char* line = map.c_str() + pos;
        char* end = nullptr;
        ULONGLONG at = strtoull(line, &end, 16);
        if (end && end != line && end + 3 <= map.c_str() + e && (end[1] == 'T' || end[1] == 't') && end[2] == ' ') {
            if (at > addr) break;
            best = at;
            name.assign(end + 3, (size_t)(map.c_str() + e - (end + 3)));
            if (!name.empty() && name.back() == '\r') name.pop_back();
        }
        pos = e + 1;
    }
    if (name.empty()) return L"";
    wchar_t tail[32];
    swprintf(tail, 32, L"+0x%llx", (unsigned long long)(addr - best));
    return Widen8(name) + tail;
}

std::wstring MapsDir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\integrity\\maps", p, MAX_PATH);
    return p;
}

std::wstring Decode(const std::wstring& what, const std::wstring& mapsDir) {
    std::wstring module;
    ULONGLONG offset = 0;
    DWORD stamp = 0;
    if (!ParseWhere(what, module, offset, stamp)) return L"";
    std::wstring stem = module.size() > 4 && !_wcsicmp(module.c_str() + module.size() - 4, L".exe")
                            ? module.substr(0, module.size() - 4)
                            : module;
    wchar_t hex[16];
    swprintf(hex, 16, L"%08lX", (unsigned long)stamp);
    std::wstring dir = mapsDir.empty() ? MapsDir() : mapsDir;
    std::wstring map = dir + L"\\" + stem + L"-" + hex + L".map";
    if (!stamp || GetFileAttributesW(map.c_str()) == INVALID_FILE_ATTRIBUTES) return L"";  // своей карты нет
    HANDLE f = CreateFileW(map.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string text((size_t)std::min<LONGLONG>(size.QuadPart, 64ll << 20), '\0');
    DWORD got = 0;
    ReadFile(f, text.data(), (DWORD)text.size(), &got, nullptr);
    CloseHandle(f);
    text.resize(got);
    // 64-битный exe MinGW грузится по 0x140000000 (без ASLR в карте); смещение - от начала модуля
    return Symbolize(text, 0x140000000ull + offset);
}

}  // namespace crash
