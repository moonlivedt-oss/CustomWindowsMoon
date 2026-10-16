// Порты и блокировки: кто слушает порт, кто держит файл (Restart Manager), путь из буфера обмена.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <restartmanager.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <set>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

std::map<DWORD, std::wstring> ProcessNames() {
    std::map<DWORD, std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) out[pe.th32ProcessID] = pe.szExeFile;
    CloseHandle(snap);
    return out;
}

template <class Table, class Row>
void Collect(ULONG family, bool udp, std::set<std::pair<int, DWORD>>& seen, std::vector<Listener>& out,
             const std::map<DWORD, std::wstring>& names) {
    ULONG size = 0;
    if (udp)
        GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
    else
        GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<BYTE> buf(size + 4096);
    size = (ULONG)buf.size();
    DWORD rc = udp ? GetExtendedUdpTable(buf.data(), &size, FALSE, family, UDP_TABLE_OWNER_PID, 0)
                   : GetExtendedTcpTable(buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (rc != NO_ERROR) return;
    auto* t = (Table*)buf.data();
    static const std::set<int> kDev = {80,   443,  1313, 3000, 3001, 3002, 3003, 3306,  4000,
                                       4200, 5000, 5001, 5173, 5174, 5432, 5500, 6006,  6379,
                                       8000, 8080, 8081, 8443, 8888, 9000, 9229, 11434, 27017};
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const Row& r = t->table[i];
        int port = ntohs((u_short)r.dwLocalPort);
        DWORD pid = r.dwOwningPid;
        if (pid <= 4 || !seen.insert({port * 2 + (udp ? 1 : 0), pid}).second) continue;
        Listener l;
        l.port = port;
        l.udp = udp;
        l.pid = pid;
        auto it = names.find(pid);
        l.process = it != names.end() ? it->second : L"?";
        l.dev = kDev.count(port) > 0;
        out.push_back(l);
    }
}

}  // namespace

std::vector<Listener> Listeners() {
    std::vector<Listener> out;
    std::set<std::pair<int, DWORD>> seen;
    auto names = ProcessNames();
    Collect<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(AF_INET, false, seen, out, names);
    Collect<MIB_TCP6TABLE_OWNER_PID, MIB_TCP6ROW_OWNER_PID>(AF_INET6, false, seen, out, names);
    Collect<MIB_UDPTABLE_OWNER_PID, MIB_UDPROW_OWNER_PID>(AF_INET, true, seen, out, names);
    // соединения на слушающих портах: сколько клиентов сейчас у сервера
    std::map<std::pair<int, DWORD>, int> conns;
    for (ULONG fam : {(ULONG)AF_INET, (ULONG)AF_INET6}) {
        ULONG size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, fam, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        std::vector<BYTE> buf(size + 4096);
        size = (ULONG)buf.size();
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, fam, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR) continue;
        if (fam == AF_INET) {
            auto* t = (MIB_TCPTABLE_OWNER_PID*)buf.data();
            for (DWORD i = 0; i < t->dwNumEntries; ++i)
                if (t->table[i].dwState == MIB_TCP_STATE_ESTAB)
                    conns[{ntohs((u_short)t->table[i].dwLocalPort), t->table[i].dwOwningPid}]++;
        } else {
            auto* t = (MIB_TCP6TABLE_OWNER_PID*)buf.data();
            for (DWORD i = 0; i < t->dwNumEntries; ++i)
                if (t->table[i].dwState == MIB_TCP_STATE_ESTAB)
                    conns[{ntohs((u_short)t->table[i].dwLocalPort), t->table[i].dwOwningPid}]++;
        }
    }
    for (Listener& l : out)
        if (!l.udp) {
            auto it = conns.find({l.port, l.pid});
            if (it != conns.end()) l.conns = it->second;
        }
    std::sort(out.begin(), out.end(), [](const Listener& a, const Listener& b) {
        if (a.dev != b.dev) return a.dev;
        if (a.udp != b.udp) return !a.udp;
        return a.port < b.port;
    });
    return out;
}

namespace {

using NtQip = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);

NtQip NtQuery() {
    static NtQip f = (NtQip)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    return f;
}

struct UStr {
    USHORT Length, MaximumLength;
    PWSTR Buffer;
};

// Командная строка процесса (ProcessCommandLineInformation, Windows 8.1+, хватает прав на запрос сведений).
std::wstring CommandLine(HANDLE h) {
    if (!NtQuery()) return L"";
    ULONG need = 0;
    NtQuery()(h, 60, nullptr, 0, &need);
    if (!need || need > 65536) return L"";
    std::vector<BYTE> buf(need);
    if (NtQuery()(h, 60, buf.data(), need, &need) != 0) return L"";
    auto* u = (UStr*)buf.data();
    return std::wstring(u->Buffer, u->Length / sizeof(wchar_t));
}

// Текущая папка процесса из его PEB (свои процессы того же пользователя, 64 бита).
std::wstring CurrentDir(HANDLE h) {
    struct Basic {
        LONG ExitStatus;
        PVOID PebBaseAddress;
        ULONG_PTR AffinityMask;
        LONG BasePriority;
        ULONG_PTR UniqueProcessId, InheritedFromUniqueProcessId;
    } bi{};
    if (!NtQuery() || NtQuery()(h, 0, &bi, sizeof(bi), nullptr) != 0 || !bi.PebBaseAddress) return L"";
    PVOID params = nullptr;
    SIZE_T got = 0;
    if (!ReadProcessMemory(h, (BYTE*)bi.PebBaseAddress + 0x20, &params, sizeof(params), &got) || !params) return L"";
    UStr cur{};
    if (!ReadProcessMemory(h, (BYTE*)params + 0x38, &cur, sizeof(cur), &got) || !cur.Length || cur.Length > 2048)
        return L"";
    std::wstring s(cur.Length / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(h, cur.Buffer, s.data(), cur.Length, &got)) return L"";
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

// Подняться от пути к папке проекта: там, где лежит package.json, Cargo.toml, pyproject.toml...
std::wstring ProjectOf(std::wstring p) {
    static const wchar_t* kMarks[] = {L"package.json", L"Cargo.toml",     L"pyproject.toml", L"manage.py",
                                      L"go.mod",       L"CMakeLists.txt", L"composer.json",  L".git"};
    std::wstring l = Lower(p);
    if (l.find(L"\\program files") != std::wstring::npos || l.find(L"\\appdata\\") != std::wstring::npos ||
        l.find(L"\\windows\\") != std::wstring::npos)
        return L"";
    for (int up = 0; up < 8 && p.size() > 3; ++up) {
        // внутри node_modules проект - выше него
        size_t nm = Lower(p).rfind(L"\\node_modules");
        if (nm != std::wstring::npos) p = p.substr(0, nm);
        for (const wchar_t* m : kMarks)
            if (GetFileAttributesW((p + L"\\" + m).c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        size_t cut = p.find_last_of(L'\\');
        if (cut == std::wstring::npos || cut < 3) break;
        p = p.substr(0, cut);
    }
    return L"";
}

std::vector<std::wstring> Tokens(const std::wstring& cmd) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool q = false;
    for (wchar_t c : cmd) {
        if (c == L'"') {
            q = !q;
        } else if (c == L' ' && !q) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

void ListenerProjects(std::vector<Listener>& ls) {
    std::map<DWORD, std::pair<std::wstring, std::wstring>> done;  // pid -> проект, команда
    for (Listener& l : ls) {
        if (!l.dev) continue;
        auto it = done.find(l.pid);
        if (it == done.end()) {
            std::wstring project, command;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, l.pid);
            if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, l.pid);
            if (h) {
                std::vector<std::wstring> t = Tokens(CommandLine(h));
                for (size_t i = 1; i < t.size() && project.empty(); ++i)
                    if (t[i].size() > 3 && t[i][1] == L':' && t[i][2] == L'\\') project = ProjectOf(t[i]);
                if (project.empty()) project = ProjectOf(CurrentDir(h));  // python manage.py, npm run dev
                for (size_t i = 1; i < t.size(); ++i) {
                    std::wstring a = t[i];
                    // длинные пути - по имени файла
                    if (a.size() > 3 && a[1] == L':') a = a.substr(a.find_last_of(L'\\') + 1);
                    command += (command.empty() ? L"" : L" ") + a;
                }
                if (command.size() > 80) command = command.substr(0, 79) + L"…";
                CloseHandle(h);
            }
            it = done.insert({l.pid, {project, command}}).first;
        }
        l.project = it->second.first;
        l.command = it->second.second;
    }
}

std::vector<Locker> WhoLocks(const std::wstring& pathIn) {
    std::vector<Locker> out;
    std::wstring path = Trim(pathIn);
    if (path.size() > 1 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
    // для папки - её файлы (не больше 400): Restart Manager спрашивает про файлы
    std::vector<std::wstring> files;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return out;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        std::vector<std::wstring> stack{path};
        while (!stack.empty() && files.size() < 400) {
            std::wstring d = stack.back();
            stack.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.cFileName[0] == L'.') continue;
                std::wstring p = d + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    stack.push_back(p);
                else
                    files.push_back(p);
            } while (FindNextFileW(h, &fd) && files.size() < 400);
            FindClose(h);
        }
    } else {
        files.push_back(path);
    }
    if (files.empty()) return out;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return out;
    std::vector<LPCWSTR> ptrs;
    for (const std::wstring& f : files) ptrs.push_back(f.c_str());
    if (RmRegisterResources(session, (UINT)ptrs.size(), ptrs.data(), 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
        UINT need = 0, have = 0;
        DWORD reason = 0;
        std::vector<RM_PROCESS_INFO> info;
        DWORD rc = RmGetList(session, &need, &have, nullptr, &reason);
        if (rc == ERROR_MORE_DATA && need) {
            info.resize(need);
            have = need;
            rc = RmGetList(session, &need, &have, info.data(), &reason);
        }
        if (rc == ERROR_SUCCESS)
            for (UINT i = 0; i < have; ++i) out.push_back({info[i].Process.dwProcessId, info[i].strAppName});
    }
    RmEndSession(session);
    // папку держат и программы, запущенные из неё самой: Restart Manager про них не всегда знает
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        std::wstring prefix = Lower(path);
        while (!prefix.empty() && prefix.back() == L'\\') prefix.pop_back();
        prefix += L'\\';
        for (const auto& [pid, name] : ProcessNames()) {
            if (pid <= 4) continue;
            bool known = false;
            for (const Locker& l : out) known = known || l.pid == pid;
            if (known) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) continue;
            wchar_t buf[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, buf, &n) && Lower(std::wstring(buf, n)).rfind(prefix, 0) == 0)
                out.push_back({pid, name});
            CloseHandle(h);
        }
    }
    return out;
}

std::wstring ClipboardPath() {
    std::wstring out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        wchar_t p[MAX_PATH];
        if (DragQueryFileW((HDROP)h, 0, p, MAX_PATH)) out = p;
    } else if (HANDLE t = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* s = (const wchar_t*)GlobalLock(t)) {
            std::wstring v = Trim(s);
            GlobalUnlock(t);
            if (v.size() > 1 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
            if (v.find(L'\n') == std::wstring::npos && GetFileAttributesW(v.c_str()) != INVALID_FILE_ATTRIBUTES)
                out = v;
        }
    }
    CloseClipboard();
    return out;
}

}  // namespace tools
