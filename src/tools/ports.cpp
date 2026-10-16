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
    std::sort(out.begin(), out.end(), [](const Listener& a, const Listener& b) {
        if (a.dev != b.dev) return a.dev;
        if (a.udp != b.udp) return !a.udp;
        return a.port < b.port;
    });
    return out;
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
