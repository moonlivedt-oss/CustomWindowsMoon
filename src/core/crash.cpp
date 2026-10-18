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
    swprintf(out, n, L"%08lX (%ls) в %ls+0x%llx", (unsigned long)code, CodeName(code), leaf,
             (unsigned long long)((ULONG_PTR)addr - (ULONG_PTR)mod));
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

void Prune(const std::wstring& dir, int keep) {
    std::vector<std::wstring> d = Dumps(dir);
    for (size_t i = 0; i + keep < d.size(); ++i) {
        DeleteFileW((dir + L"\\" + d[i]).c_str());
        DeleteFileW((dir + L"\\" + Stem(d[i]) + L".txt").c_str());
    }
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

}  // namespace crash
