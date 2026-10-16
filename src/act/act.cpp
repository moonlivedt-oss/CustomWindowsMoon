// RadialTools.exe - всё, что удаляет файлы и завершает процессы. Основной RadialMenu.exe работает постоянно и сам
// этого не делает: он запускает RadialTools.exe по кнопке, читает ход из stdout и ждёт итога.
// Строки журнала идут как есть, итог - строкой «@ключ=значение». Код выхода 0 - получилось.
//
//   clean <имя кэша>          содержимое кэша из списка FindJunk (в TEMP - только старше суток)
//   ddc-trim <дней>           из кэша Unreal - то, что не открывалось дольше
//   projects <путь>...        папки мусора проектов; каждая заново проверяется по признакам
//   kill <pid> [exe]          завершить процесс (exe - проверить, что номер не достался другому)
//   close <exe> <мс>          закрыть программу: окнам WM_CLOSE, оставшиеся процессы - силой
//   empty-bin                 очистить корзину
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/text.h"
#include "tools/tools.h"

namespace {

void Out(const std::wstring& s) {
    std::string u = Narrow(s) + "\n";
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
}

ULONGLONG Stamp(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

ULONGLONG NowStamp() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return Stamp(ft);
}

constexpr ULONGLONG kDay = 24ull * 3600 * 10000000;

bool Dots(const wchar_t* n) { return n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2])); }

// Удалить содержимое папки (саму папку оставить); занятые файлы пропускаются.
// newer - файлы новее этой отметки не трогаются; oldAccess - удалять только то, что не открывалось и не менялось
// с этой отметки (0 - всё).
struct Del {
    ULONGLONG newer = 0, oldAccess = 0;
    int failed = 0, kept = 0;
};

ULONGLONG DeleteContents(const std::wstring& root, Del& d, bool removeSelf = false) {
    ULONGLONG freed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (Dots(fd.cFileName)) continue;
            std::wstring p = root + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                // ссылку удаляем как ссылку, внутрь не идём
                if (!((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(p.c_str())
                                                                       : DeleteFileW(p.c_str())))
                    ++d.failed;
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                freed += DeleteContents(p, d, true);
                continue;
            }
            if (d.newer && (Stamp(fd.ftLastWriteTime) > d.newer || Stamp(fd.ftCreationTime) > d.newer)) {
                ++d.kept;
                continue;
            }
            if (d.oldAccess && std::max(Stamp(fd.ftLastAccessTime), Stamp(fd.ftLastWriteTime)) >= d.oldAccess) {
                ++d.kept;
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(p.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if (DeleteFileW(p.c_str()))
                freed += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            else
                ++d.failed;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (removeSelf) RemoveDirectoryW(root.c_str());  // не пустая (что-то занято или оставлено) - останется
    return freed;
}

int Clean(const std::wstring& name) {
    // чистим только папку из своего списка, а не что прислали
    for (const tools::Junk& j : tools::FindJunk(false)) {
        if (j.name != name) continue;
        if (j.path.size() < 8) break;
        Del d;
        if (_wcsicmp(j.path.c_str(), tools::TempDir().c_str()) == 0) d.newer = NowStamp() - kDay;
        Out(L"Чищу: " + j.name + L"...");
        ULONGLONG freed = DeleteContents(j.path, d);
        Out(j.name + L": освобождено " + tools::Bytes(freed) +
            (d.failed ? L", занятых файлов пропущено: " + std::to_wstring(d.failed) : L"") +
            (d.kept ? L", свежих (моложе суток) оставлено: " + std::to_wstring(d.kept) : L""));
        Out(L"@freed=" + std::to_wstring(freed));
        return 0;
    }
    Out(L"Эту папку чистить не буду: её нет в списке кэшей.");
    Out(L"@freed=0");
    return 1;
}

int DdcTrim(int days) {
    std::wstring dir = tools::DdcDir();
    if (days < 7 || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Out(days < 7 ? L"Срок меньше недели - так можно выбросить нужное, не буду." : L"Кэша Unreal нет.");
        Out(L"@freed=0");
        return 1;
    }
    Del d;
    d.oldAccess = NowStamp() - (ULONGLONG)days * kDay;
    Out(L"Убираю из кэша Unreal то, что не открывалось " + std::to_wstring(days) + L" дней...");
    ULONGLONG freed = DeleteContents(dir, d);
    Out(L"Кэш Unreal: освобождено " + tools::Bytes(freed) + L", нужное недавно оставлено: " + std::to_wstring(d.kept) +
        L" файлов" + (d.failed ? L", занятых: " + std::to_wstring(d.failed) : L""));
    Out(L"@freed=" + std::to_wstring(freed));
    return 0;
}

int Projects(const std::vector<std::wstring>& paths) {
    ULONGLONG total = 0;
    int bad = 0;
    for (const std::wstring& p : paths) {
        std::wstring kind;
        if (!tools::IsProjectJunk(p, &kind)) {
            Out(L"Пропускаю (не похоже на мусор сборки): " + p);
            ++bad;
            continue;
        }
        Out(L"Удаляю " + kind + L": " + p);
        Del d;
        ULONGLONG freed = DeleteContents(p, d, true);
        total += freed;
        if (d.failed) Out(L"  занято файлов: " + std::to_wstring(d.failed) + L" (программа проекта запущена?)");
    }
    Out(L"Освобождено " + tools::Bytes(total));
    Out(L"@freed=" + std::to_wstring(total));
    return bad ? 1 : 0;
}

std::wstring ExePath(HANDLE h) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    if (!QueryFullProcessImageNameW(h, 0, buf, &n)) return L"";
    return std::wstring(buf, n);
}

std::wstring ExeName(HANDLE h) {
    std::wstring p = ExePath(h);
    return p.substr(p.find_last_of(L'\\') + 1);
}

int Kill(DWORD pid, const std::wstring& expect) {
    bool ok = false;
    if (pid > 4) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (h) {
            std::wstring full = ExePath(h), exe = full.substr(full.find_last_of(L'\\') + 1);
            // номер мог уже достаться другому процессу, пока кольцо было открыто
            bool same = expect.empty() || expect == L"?" || _wcsicmp(exe.c_str(), expect.c_str()) == 0;
            if (full.empty())
                Out(L"Процесс уже завершился или недоступен");
            else if (!same)
                Out(L"Номер " + std::to_wstring(pid) + L" уже у другого процесса: " + exe);
            // «svchost.exe» не из папки Windows - самозванец, его защищать не нужно
            bool guarded = tools::ProtectedExe(exe) && !tools::SystemImposter(full);
            ok = same && !full.empty() && !guarded && TerminateProcess(h, 1);
            if (ok) WaitForSingleObject(h, 1500);  // порт и файлы освобождаются, когда процесс действительно ушёл
            CloseHandle(h);
        }
    }
    Out(std::wstring(L"@ok=") + (ok ? L"1" : L"0"));
    return ok ? 0 : 1;
}

struct CloseCtx {
    const std::vector<DWORD>* pids;
    int posted = 0;
};

BOOL CALLBACK CloseTop(HWND w, LPARAM lp) {
    auto* c = (CloseCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (std::find(c->pids->begin(), c->pids->end(), pid) == c->pids->end()) return TRUE;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;  // только главные окна
    PostMessageW(w, WM_CLOSE, 0, 0);
    ++c->posted;
    return TRUE;
}

int Close(const std::wstring& exe, int waitMs) {
    int closed = 0, killed = 0, failed = 0;
    if (!tools::ProtectedExe(exe)) {
        DWORD mySession = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
        std::vector<HANDLE> hs;
        std::vector<DWORD> pids;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); snap != INVALID_HANDLE_VALUE && ok; ok = Process32NextW(snap, &pe)) {
            DWORD s = 0;
            if (_wcsicmp(pe.szExeFile, exe.c_str()) != 0 || !ProcessIdToSessionId(pe.th32ProcessID, &s) ||
                s != mySession)
                continue;
            HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   pe.th32ProcessID);
            if (!h) {
                ++failed;
                continue;
            }
            hs.push_back(h);
            pids.push_back(pe.th32ProcessID);
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        // сначала по-хорошему: главным окнам WM_CLOSE (браузер сохранит вкладки, редактор спросит про файлы)
        CloseCtx ctx{&pids};
        EnumWindows(CloseTop, (LPARAM)&ctx);
        if (ctx.posted) Out(L"Прошу " + exe + L" закрыться...");
        ULONGLONG until = GetTickCount64() + (ctx.posted ? waitMs : 0);
        for (HANDLE h : hs) {
            ULONGLONG now = GetTickCount64();
            DWORD wait = now < until ? (DWORD)(until - now) : 0;
            if (WaitForSingleObject(h, wait) == WAIT_OBJECT_0)
                ++closed;
            else if (TerminateProcess(h, 1) && WaitForSingleObject(h, 1500) == WAIT_OBJECT_0)
                ++killed;
            else
                ++failed;
            CloseHandle(h);
        }
    }
    Out(L"@closed=" + std::to_wstring(closed) + L"," + std::to_wstring(killed) + L"," + std::to_wstring(failed));
    return closed + killed ? 0 : 1;
}

// В Корзину (вернуть можно), только файлы прямо в Загрузках.
int Recycle(const std::vector<std::wstring>& paths) {
    std::wstring dl = Lower(tools::DownloadsDir());
    std::wstring list;  // двойной ноль в конце - формат SHFileOperation
    int ok = 0;
    for (const std::wstring& p : paths) {
        std::wstring l = Lower(p);
        DWORD a = GetFileAttributesW(p.c_str());
        size_t cut = l.find_last_of(L'\\');
        if (dl.empty() || cut == std::wstring::npos || l.substr(0, cut) != dl || a == INVALID_FILE_ATTRIBUTES ||
            (a & FILE_ATTRIBUTE_DIRECTORY)) {
            Out(L"Пропускаю (не файл из Загрузок): " + p);
            continue;
        }
        list += p;
        list.push_back(L'\0');
        ++ok;
    }
    if (!ok) {
        Out(L"@moved=0");
        return 1;
    }
    list.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) ok = 0;
    Out(rc == 0 ? L"В Корзине: " + std::to_wstring(ok) + L" файлов"
                : L"Корзина не приняла файлы (код " + std::to_wstring(rc) + L")");
    Out(L"@moved=" + std::to_wstring(ok));
    return rc == 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);  // фоновая работа не мешает игре
    std::vector<std::wstring> a(argv + 1, argv + argc);
    if (a.empty()) {
        Out(L"RadialTools: служебная часть RadialMenu (очистка и завершение процессов).");
        return 2;
    }
    const std::wstring& cmd = a[0];
    if (cmd == L"clean" && a.size() >= 2) return Clean(a[1]);
    if (cmd == L"ddc-trim" && a.size() >= 2) return DdcTrim(_wtoi(a[1].c_str()));
    if (cmd == L"projects" && a.size() >= 2) return Projects(std::vector<std::wstring>(a.begin() + 1, a.end()));
    if (cmd == L"kill" && a.size() >= 2) return Kill((DWORD)_wtoi(a[1].c_str()), a.size() >= 3 ? a[2] : L"");
    if (cmd == L"close" && a.size() >= 3) return Close(a[1], _wtoi(a[2].c_str()));
    if (cmd == L"recycle" && a.size() >= 2) return Recycle(std::vector<std::wstring>(a.begin() + 1, a.end()));
    if (cmd == L"empty-bin") {
        bool ok =
            SUCCEEDED(SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND));
        Out(std::wstring(L"@ok=") + (ok ? L"1" : L"0"));
        return ok ? 0 : 1;
    }
    Out(L"Неизвестная команда: " + cmd);
    return 2;
}
