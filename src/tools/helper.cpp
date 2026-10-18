// Обёртки над RadialTools.exe: удаление и завершение процессов идут отдельным процессом (см. src/act/act.cpp).
// Строки журнала передаются в log по мере появления, итог - строка «@ключ=значение».
#include <windows.h>

#include <algorithm>
#include <map>

#include "core/files.h"
#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

// Аргумент в кавычках по правилам CommandLineToArgvW: «D:\» без удвоения черты перед кавычкой съел бы её
// и склеил аргумент со следующими.
std::wstring Quote(const std::wstring& s) {
    std::wstring o = L"\"" + s;
    for (size_t i = s.size(); i > 0 && s[i - 1] == L'\\'; --i) o += L'\\';
    return o + L"\"";
}

// Запустить RadialTools.exe и дождаться: журнал - в log, итоговые пары - в res. Код выхода, -1 - не запустился.
int RunTools(const std::wstring& args, const Log& log, std::map<std::wstring, std::wstring>* res,
             DWORD timeoutMs = 30 * 60 * 1000) {
    std::wstring exe = ExeDir() + L"\\RadialTools.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (log) log(L"Нет RadialTools.exe рядом с RadialMenu.exe - пересобери проект (build.cmd).");
        return -1;
    }
    std::wstring cmd = Quote(exe) + L" " + args;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             ExeDir().c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        if (log) log(L"RadialTools.exe не запустился (антивирус?).");
        return -1;
    }
    std::string buf;
    char chunk[4096];
    DWORD got = 0;
    auto flush = [&](bool all) {
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos || (all && !buf.empty())) {
            std::string one = nl == std::string::npos ? buf : buf.substr(0, nl);
            buf.erase(0, nl == std::string::npos ? buf.size() : nl + 1);
            if (!one.empty() && one.back() == '\r') one.pop_back();
            std::wstring w = Widen(one);
            if (w.empty()) continue;
            if (w[0] == L'@') {
                size_t eq = w.find(L'=');
                if (res && eq != std::wstring::npos) (*res)[w.substr(1, eq - 1)] = w.substr(eq + 1);
            } else if (log) {
                log(w);
            }
        }
    };
    // читаем, пока есть что, но не дольше timeoutMs: зависший RadialTools не должен подвесить кольцо навсегда
    ULONGLONG until = GetTickCount64() + timeoutMs;
    bool timedOut = false;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // RadialTools закрыл свой конец
        if (avail) {
            if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) || !got) break;
            buf.append(chunk, got);
            flush(false);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
                   ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) && got)
                buf.append(chunk, got);
            break;
        }
        if (GetTickCount64() > until) {
            timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    flush(true);
    CloseHandle(rd);
    if (timedOut && log) log(L"RadialTools.exe не уложился во время и остановлен.");
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

ULONGLONG Freed(const std::map<std::wstring, std::wstring>& r) {
    auto it = r.find(L"freed");
    return it == r.end() ? 0 : _wcstoui64(it->second.c_str(), nullptr, 10);
}

}  // namespace

ULONGLONG CleanJunk(const Junk& j, const Log& log) { return CleanJunks({j}, nullptr, nullptr, log); }

ULONGLONG CleanJunks(const std::vector<Junk>& list, const std::function<void(size_t)>& started,
                     const std::function<void(size_t, ULONGLONG)>& done, const Log& log,
                     std::vector<std::wstring>* busy) {
    if (list.empty()) return 0;
    std::wstring args = L"clean";
    for (const Junk& j : list) args += L" " + Quote(j.name);
    std::vector<bool> reported(list.size(), false);
    std::map<std::wstring, std::wstring> r;
    RunTools(
        args,
        [&](const std::wstring& s) {
            CleanLine c = ParseCleanLine(s);
            if (c.kind == CleanLine::LOG) {
                if (log) log(s);
            } else if (c.kind == CleanLine::BUSY) {
                if (busy) busy->push_back(c.text);
            } else if (c.index < list.size()) {
                if (c.kind == CleanLine::START && started) started(c.index);
                if (c.kind == CleanLine::FREED) {
                    reported[c.index] = true;
                    if (done) done(c.index, c.bytes);
                }
            }
        },
        &r);
    // RadialTools упал или его не было - сцена не должна ждать вечно
    for (size_t i = 0; i < list.size(); ++i)
        if (!reported[i] && done) done(i, 0);
    return Freed(r);
}

ULONGLONG TrimDdc(int days, const Log& log) {
    std::map<std::wstring, std::wstring> r;
    RunTools(L"ddc-trim " + std::to_wstring(days), log, &r);
    return Freed(r);
}

ULONGLONG CleanProjects(const std::vector<std::wstring>& paths, const Log& log) {
    std::wstring args = L"projects";
    for (const std::wstring& p : paths) args += L" " + Quote(p);
    std::map<std::wstring, std::wstring> r;
    RunTools(args, log, &r);
    return Freed(r);
}

bool KillPid(DWORD pid, const std::wstring& expect) {
    if (pid <= 4 || pid == GetCurrentProcessId()) return false;
    std::map<std::wstring, std::wstring> r;
    RunTools(L"kill " + std::to_wstring(pid) + (expect.empty() ? L"" : L" " + Quote(expect)), nullptr, &r, 15000);
    return r[L"ok"] == L"1";
}

AppClose CloseApp(const HeavyApp& app, int waitMs, const Log& log) {
    std::map<std::wstring, std::wstring> r;
    RunTools(L"close " + Quote(app.name) + L" " + std::to_wstring(waitMs), log, &r, waitMs + 30000);
    AppClose c;
    std::vector<std::wstring> p = Split(r[L"closed"], L',');
    if (p.size() == 3) c.closed = _wtoi(p[0].c_str()), c.killed = _wtoi(p[1].c_str()), c.failed = _wtoi(p[2].c_str());
    return c;
}

int RecycleDownloads(const std::vector<std::wstring>& paths, const Log& log) {
    int moved = 0;
    // строка команды не бесконечна: по 40 файлов за раз
    for (size_t i = 0; i < paths.size(); i += 40) {
        std::wstring args = L"recycle";
        for (size_t j = i; j < paths.size() && j < i + 40; ++j) args += L" " + Quote(paths[j]);
        std::map<std::wstring, std::wstring> r;
        RunTools(args, log, &r);
        moved += _wtoi(r[L"moved"].c_str());
    }
    return moved;
}

int RecycleDuplicates(const std::wstring& keep, const std::vector<std::wstring>& copies, const Log& log) {
    int moved = 0;
    for (size_t i = 0; i < copies.size(); i += 30) {
        std::wstring args = L"recycle-dup " + Quote(keep);
        for (size_t j = i; j < copies.size() && j < i + 30; ++j) args += L" " + Quote(copies[j]);
        std::map<std::wstring, std::wstring> r;
        RunTools(args, log, &r);
        moved += _wtoi(r[L"moved"].c_str());
    }
    return moved;
}

int RecycleLeftovers(const std::vector<std::wstring>& paths, const Log& log) {
    int moved = 0;
    for (size_t i = 0; i < paths.size(); i += 30) {
        std::wstring args = L"recycle-leftover";
        for (size_t j = i; j < paths.size() && j < i + 30; ++j) args += L" " + Quote(paths[j]);
        std::map<std::wstring, std::wstring> r;
        RunTools(args, log, &r);
        moved += _wtoi(r[L"moved"].c_str());
    }
    return moved;
}

bool ResetExplorer(const Log& log) {
    std::map<std::wstring, std::wstring> r;
    RunTools(L"explorer-reset", log, &r, 60000);
    return r[L"ok"] == L"1";
}

bool EmptyRecycleBin() {
    std::map<std::wstring, std::wstring> r;
    RunTools(L"empty-bin", nullptr, &r);
    return r[L"ok"] == L"1";
}

}  // namespace tools
