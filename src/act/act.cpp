// RadialTools.exe - всё, что удаляет файлы и завершает процессы. Основной RadialMenu.exe работает постоянно и сам
// этого не делает: он запускает RadialTools.exe по кнопке, читает ход из stdout и ждёт итога.
// Строки журнала идут как есть, итог - строкой «@ключ=значение». Код выхода 0 - получилось.
//
//   clean <имя кэша>...       содержимое кэшей из списка FindJunk (в TEMP - по правилу tools::TempRemovable); ход по
//   кэшам -
//                             строки «#start	<номер>» и «#freed	<номер>	<байт>»
//   ddc-trim <дней>           из кэша Unreal - то, что не открывалось дольше
//   projects <путь>...        папки мусора проектов; каждая заново проверяется по признакам
//   kill <pid> [exe]          завершить процесс (exe - проверить, что номер не достался другому)
//   close <exe> <мс>          закрыть программу: окнам WM_CLOSE, оставшиеся процессы - силой
//   empty-bin                 очистить корзину
//   recycle-dup <оставить> <копии>  копии в Корзину, если размер и SHA-256 совпадают с оставляемым
//   recycle-big <файл>        большой файл из списка MFT - в Корзину, если tools::BigFileKeep не против
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

// Удалить файл. Сначала POSIX-удаление (Windows 10 1709+): имя пропадает сразу, даже если файл кто-то держит открытым
// с разрешением на удаление, - папка с ним тоже удалится; место вернётся, когда держащий его закроет. Не вышло
// (старая Windows, FAT, сеть) - обычный DeleteFileW.
bool Remove(const std::wstring& p, DWORD attrs) {
    HANDLE h = CreateFileW(p.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        // FILE_DISPOSITION_INFO_EX: DELETE | POSIX_SEMANTICS | IGNORE_READONLY_ATTRIBUTE
        struct {
            DWORD flags;
        } disp{0x1 | 0x2 | 0x10};
        BOOL ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)21 /* FileDispositionInfoEx */, &disp,
                                             sizeof(disp));
        CloseHandle(h);
        if (ok) return true;
    }
    if (attrs & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    return DeleteFileW(p.c_str());
}

// Удалить содержимое папки (саму папку оставить); занятые файлы пропускаются.
// temp - правило TEMP (tools::TempRemovable): свежее и занятое не трогаем; oldAccess - удалять только то, что не
// открывалось и не менялось с этой отметки (0 - всё). busy - занятые файлы (до 300): кто их держит, скажет RadialMenu.
struct Del {
    bool temp = false;
    ULONGLONG oldAccess = 0;
    int failed = 0, kept = 0;
    std::vector<std::wstring> busy;
    void Busy(const std::wstring& p) {
        ++failed;
        if (busy.size() < 300) busy.push_back(p);
    }
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
            if (d.temp) {
                tools::WalkFile f;
                f.dir = &root;
                f.name = fd.cFileName;
                f.nameLen = wcslen(fd.cFileName);
                f.write = Stamp(fd.ftLastWriteTime);
                f.create = Stamp(fd.ftCreationTime);
                if (!tools::TempRemovable(f, NowStamp())) {
                    ++d.kept;
                    continue;
                }
            }
            if (d.oldAccess && std::max(Stamp(fd.ftLastAccessTime), Stamp(fd.ftLastWriteTime)) >= d.oldAccess) {
                ++d.kept;
                continue;
            }
            if (Remove(p, fd.dwFileAttributes))
                freed += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            else
                d.Busy(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (removeSelf) RemoveDirectoryW(root.c_str());  // не пустая (что-то занято или оставлено) - останется
    return freed;
}

// Программа на PATH: полный путь или пусто. Только папки PATH, по порядку: SearchPathW смотрел бы сначала рядом с
// RadialTools.exe и в текущей папке - туда программу можно подложить.
std::wstring Which(const wchar_t* exe) {
    DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring path(n, L'\0');
    path.resize(GetEnvironmentVariableW(L"PATH", path.data(), n));
    for (std::wstring dir : Split(path, L';')) {
        if (dir.size() > 1 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        if (dir.empty() || dir == L"." || dir.size() < 3 || dir[1] != L':') continue;  // только полные пути
        while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
        std::wstring full = dir + L"\\" + exe;
        DWORD a = GetFileAttributesW(full.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return full;
    }
    return L"";
}

// Запустить и дождаться (без окна); код выхода, -1 - не запустилась или не уложилась.
int Run(const std::wstring& cmdLine, DWORD timeoutMs) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmdLine.begin(), cmdLine.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// Кэши со своей командой очистки: сам менеджер знает, что у него занято (uv прямо пишет, что трогать его кэш руками
// небезопасно, если uv в этот момент работает). Нет программы - чистим сами: тогда и пользоваться кэшем некому.
struct Official {
    const wchar_t *junk, *exe, *args;
    bool viaCmd;  // npm - это npm.cmd, его запускает cmd
};
const Official kOfficial[] = {
    {L"Кэш uv", L"uv.exe", L"cache clean", false},
    {L"Кэш npm", L"npm.cmd", L"cache clean --force", true},
    {L"Кэш pip", L"pip.exe", L"cache purge", false},
    {L"Кэш Go", L"go.exe", L"clean -cache", false},
};

ULONGLONG CleanOne(const tools::Junk& j, int& failed, int& kept) {
    for (const Official& o : kOfficial) {
        if (j.name != o.junk) continue;
        std::wstring exe = Which(o.exe);
        if (exe.empty()) break;
        ULONGLONG before = tools::DirBytes(j.path);
        std::wstring cmd =
            o.viaCmd ? L"cmd.exe /d /c \"\"" + exe + L"\" " + o.args + L"\"" : L"\"" + exe + L"\" " + o.args;
        int code = Run(cmd, 10 * 60 * 1000);
        ULONGLONG after = tools::DirBytes(j.path);
        Out(j.name + L": " + o.exe + L" " + o.args + (code == 0 ? L"" : L" (код " + std::to_wstring(code) + L")"));
        if (code == 0) return before > after ? before - after : 0;
        Out(L"Команда не справилась - чищу папку сама.");
        break;
    }
    Del d;
    if (_wcsicmp(j.path.c_str(), tools::TempDir().c_str()) == 0) {
        // %TEMP% задаёт пользователь: если он указывает не на папку Temp (D:\, профиль), чистить нельзя
        std::wstring leaf = Lower(j.path.substr(j.path.find_last_of(L'\\') + 1));
        if (leaf != L"temp" && leaf != L"tmp") {
            Out(L"%TEMP% указывает на " + j.path + L" - это не папка временных файлов, не трогаю.");
            return 0;
        }
        d.temp = true;
    }
    ULONGLONG freed = DeleteContents(j.path, d);
    failed += d.failed;
    kept += d.kept;
    for (const std::wstring& b : d.busy) Out(L"#busy\t" + b);
    return freed;
}

int Clean(const std::vector<std::wstring>& names) {
    // чистим только папки из своего списка, а не что прислали
    std::vector<tools::Junk> known = tools::FindJunk(false);
    ULONGLONG total = 0;
    int bad = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        Out(L"#start	" + std::to_wstring(i));
        auto it = std::find_if(known.begin(), known.end(), [&](const tools::Junk& j) { return j.name == names[i]; });
        ULONGLONG freed = 0;
        if (it == known.end() || it->path.size() < 8) {
            Out(names[i] + L": этой папки нет в списке кэшей - не трогаю.");
            ++bad;
        } else {
            Out(L"Чищу: " + it->name + L"...");
            int failed = 0, kept = 0;
            freed = CleanOne(*it, failed, kept);
            Out(it->name + L": освобождено " + tools::Bytes(freed) +
                (failed ? L", занятых файлов пропущено: " + std::to_wstring(failed) : L"") +
                (kept ? L", свежих или занятых оставлено: " + std::to_wstring(kept) : L""));
        }
        Out(L"#freed	" + std::to_wstring(i) + L"	" + std::to_wstring(freed));
        total += freed;
    }
    Out(L"@freed=" + std::to_wstring(total));
    return bad ? 1 : 0;
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
// Дубликаты: каждую копию сверяем с оставляемым файлом заново - список мог устареть, пока кольцо было открыто.
int RecycleDup(const std::wstring& keep, const std::vector<std::wstring>& copies) {
    WIN32_FILE_ATTRIBUTE_DATA ka;
    std::wstring keepHash;
    if (!GetFileAttributesExW(keep.c_str(), GetFileExInfoStandard, &ka) ||
        (ka.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (keepHash = tools::FileSha256(keep)).empty()) {
        Out(L"Оставляемого файла нет - ничего не трогаю: " + keep);
        Out(L"@moved=0");
        return 1;
    }
    std::wstring list;
    int ok = 0;
    for (const std::wstring& p : copies) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (_wcsicmp(p.c_str(), keep.c_str()) == 0 || !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) ||
            (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || a.nFileSizeHigh != ka.nFileSizeHigh ||
            a.nFileSizeLow != ka.nFileSizeLow || tools::FileSha256(p) != keepHash) {
            Out(L"Пропускаю (уже не копия): " + p);
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
    Out(rc == 0 ? L"В Корзине: " + std::to_wstring(ok)
                : L"Корзина не приняла файлы (код " + std::to_wstring(rc) + L")");
    Out(L"@moved=" + std::to_wstring(ok));
    return ok ? 0 : 1;
}

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

// Хвосты удалённых программ - в Корзину. Список считает RadialMenu; здесь заново проверяется место: только папки
// прямо в AppData\Roaming или AppData\Local, не ссылки, не служебные папки Windows и программ разработки.
int RecycleLeftovers(const std::vector<std::wstring>& paths) {
    wchar_t roam[MAX_PATH] = {}, local[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%APPDATA%", roam, MAX_PATH);
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%", local, MAX_PATH);
    static const wchar_t* kKeep[] = {L"microsoft", L"packages", L"temp",    L"programs",  L"moondesk", L"nvidia",
                                     L"google",    L"mozilla",  L"windows", L"kaspersky", L"npm",      L"docker"};
    std::wstring list;
    int ok = 0;
    for (const std::wstring& p : paths) {
        size_t cut = p.find_last_of(L'\\');
        std::wstring parent = cut == std::wstring::npos ? L"" : p.substr(0, cut), leaf = Lower(p.substr(cut + 1));
        DWORD a = GetFileAttributesW(p.c_str());
        bool place = !_wcsicmp(parent.c_str(), roam) || !_wcsicmp(parent.c_str(), local);
        bool keep = leaf.empty();
        for (const wchar_t* k : kKeep) keep |= leaf.rfind(k, 0) == 0;
        if (!place || keep || a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) ||
            (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            Out(L"Пропускаю: " + p);
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
    Out(rc == 0 ? L"В Корзине папок: " + std::to_wstring(ok)
                : L"Корзина не приняла (код " + std::to_wstring(rc) + L")");
    Out(L"@moved=" + std::to_wstring(ok));
    return ok ? 0 : 1;
}

// Большой файл (И9) - в Корзину. Правило места то же, что у студии (tools::BigFileKeep), плюс атрибуты здесь:
// не папка, не ссылка, не системный.
int RecycleBig(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    std::wstring why = tools::BigFileKeep(p);
    if (why.empty() && (a == INVALID_FILE_ATTRIBUTES ||
                        (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM))))
        why = a == INVALID_FILE_ATTRIBUTES ? L"файла уже нет" : L"папка, ссылка или системный файл";
    if (!why.empty()) {
        Out(L"Не трогаю: " + why + L" - " + p);
        Out(L"@moved=0");
        return 1;
    }
    std::wstring list = p;
    list.push_back(L'\0');
    list.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    int rc = SHFileOperationW(&op);
    bool ok = rc == 0 && !op.fAnyOperationsAborted;
    Out(ok ? L"В Корзине: " + p
           : L"Корзина не приняла (код " + std::to_wstring(rc) + L"), файл занят или слишком велик");
    Out(std::wstring(L"@moved=") + (ok ? L"1" : L"0"));
    return ok ? 0 : 1;
}

// Сбросить Проводник: закрыть, удалить кэш значков и эскизов, запустить заново. Окна Проводника закроются.
int ExplorerReset() {
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    auto killAll = [&] {
        int n = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); snap != INVALID_HANDLE_VALUE && ok; ok = Process32NextW(snap, &pe)) {
            DWORD s = 0;
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0 || !ProcessIdToSessionId(pe.th32ProcessID, &s) ||
                s != mySession)
                continue;
            if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
                if (TerminateProcess(h, 1)) ++n, WaitForSingleObject(h, 3000);
                CloseHandle(h);
            }
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        return n;
    };
    Out(L"Закрываю Проводник...");
    killAll();
    wchar_t local[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%", local, MAX_PATH);
    std::wstring dir = std::wstring(local) + L"\\Microsoft\\Windows\\Explorer";
    int removed = 0;
    // Windows перезапускает Проводник сам: файлы кэша снова заняты - несколько попыток
    for (int attempt = 0; attempt < 4; ++attempt) {
        for (const wchar_t* mask : {L"\\iconcache_*.db", L"\\thumbcache_*.db"}) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + mask).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do removed += DeleteFileW((dir + L"\\" + fd.cFileName).c_str()) ? 1 : 0;
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        removed += DeleteFileW((std::wstring(local) + L"\\IconCache.db").c_str()) ? 1 : 0;
        if (attempt < 3) {
            Sleep(400);
            killAll();
        }
    }
    Sleep(1500);
    // если Windows не подняла Проводник сама - запустить
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        std::wstring exe = std::wstring(win) + L"\\explorer.exe";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmd(exe.begin(), exe.end());
        cmd.push_back(0);
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    Out(L"Удалено файлов кэша: " + std::to_wstring(removed) + L". Значки и эскизы соберутся заново.");
    Out(L"@ok=1");
    return 0;
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
    if (cmd == L"clean" && a.size() >= 2) return Clean(std::vector<std::wstring>(a.begin() + 1, a.end()));
    if (cmd == L"ddc-trim" && a.size() >= 2) return DdcTrim(_wtoi(a[1].c_str()));
    if (cmd == L"projects" && a.size() >= 2) return Projects(std::vector<std::wstring>(a.begin() + 1, a.end()));
    if (cmd == L"kill" && a.size() >= 2) return Kill((DWORD)_wtoi(a[1].c_str()), a.size() >= 3 ? a[2] : L"");
    if (cmd == L"close" && a.size() >= 3) return Close(a[1], _wtoi(a[2].c_str()));
    if (cmd == L"recycle" && a.size() >= 2) return Recycle(std::vector<std::wstring>(a.begin() + 1, a.end()));
    if (cmd == L"recycle-dup" && a.size() >= 3)
        return RecycleDup(a[1], std::vector<std::wstring>(a.begin() + 2, a.end()));
    if (cmd == L"recycle-leftover" && a.size() >= 2)
        return RecycleLeftovers(std::vector<std::wstring>(a.begin() + 1, a.end()));
    if (cmd == L"explorer-reset") return ExplorerReset();
    if (cmd == L"recycle-big" && a.size() >= 2) return RecycleBig(a[1]);
    if (cmd == L"empty-bin") {
        bool ok =
            SUCCEEDED(SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND));
        Out(std::wstring(L"@ok=") + (ok ? L"1" : L"0"));
        return ok ? 0 : 1;
    }
    Out(L"Неизвестная команда: " + cmd);
    return 2;
}
