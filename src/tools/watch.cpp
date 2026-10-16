// Слежка без прав: снимок процессов (память, процессор, окна), утечки и фоновый процессор по истории,
// хвосты сборки, выделенная память и подкачка, батарея по драйверу, трафик физических адаптеров,
// новый автозапуск. Трафик по процессам и «что мешает сну» - только в процессе с правами (задача сети).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <batclass.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <psapi.h>
#include <setupapi.h>
#include <tcpestats.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <set>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

ULONGLONG Ft(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

ULONGLONG NowFt() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return Ft(f);
}

BOOL CALLBACK WindowPid(HWND w, LPARAM lp) {
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetWindowRect(w, &r) || r.right - r.left < 40 || r.bottom - r.top < 40) return TRUE;  // невидимые служебные
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    ((std::set<DWORD>*)lp)->insert(pid);
    return TRUE;
}

bool Is(const std::wstring& lowerName, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names)
        if (lowerName == n) return true;
    return false;
}

}  // namespace

int RunCapture(const std::wstring& cmdline, std::wstring& out, DWORD timeoutMs) {
    out.clear();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmdline.begin(), cmdline.end());
    line.push_back(0);
    BOOL ok =
        CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    std::string buf;
    char chunk[4096];
    DWORD got = 0;
    ULONGLONG until = GetTickCount64() + timeoutMs;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // процесс закрыл свой конец
        if (avail) {
            if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) || !got) break;
            buf.append(chunk, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
                   ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) && got)
                buf.append(chunk, got);
            break;
        }
        if (GetTickCount64() > until) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    CloseHandle(rd);
    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, 2000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    // консольные программы Windows пишут в кодировке консоли (cp866), остальные - в UTF-8 или ASCII
    UINT cp = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf.data(), (int)buf.size(), nullptr, 0) ? CP_UTF8
                                                                                                          : CP_OEMCP;
    int n = MultiByteToWideChar(cp, 0, buf.data(), (int)buf.size(), nullptr, 0);
    out.resize(n);
    if (n) MultiByteToWideChar(cp, 0, buf.data(), (int)buf.size(), out.data(), n);
    return (int)code;
}

// ---------------- процессы ----------------

std::vector<ProcInfo> Processes() {
    std::vector<ProcInfo> out;
    std::set<DWORD> windows;
    EnumWindows(WindowPid, (LPARAM)&windows);
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        DWORD s = 0;
        if (pe.th32ProcessID <= 4 || !ProcessIdToSessionId(pe.th32ProcessID, &s) || s != mySession) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        ProcInfo p;
        p.pid = pe.th32ProcessID;
        p.parent = pe.th32ParentProcessID;
        p.name = pe.szExeFile;
        FILETIME c, e, k, u;
        if (GetProcessTimes(h, &c, &e, &k, &u)) p.created = Ft(c), p.cpu = Ft(k) + Ft(u);
        p.mem = PrivateBytes(h);
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) p.path.assign(path, n);
        CloseHandle(h);
        p.window = windows.count(p.pid) > 0;
        out.push_back(std::move(p));
    }
    CloseHandle(snap);
    return out;
}

int ProcessMinutes(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return -1;
    FILETIME c, e, k, u;
    bool ok = GetProcessTimes(h, &c, &e, &k, &u);
    CloseHandle(h);
    return ok ? (int)((NowFt() - Ft(c)) / 600000000ull) : -1;
}

void ProcHistory::Feed(const std::vector<ProcInfo>& ps, ULONGLONG tick) {
    if (!first_) first_ = tick;
    lastTick_ = tick;
    std::set<std::pair<DWORD, ULONGLONG>> seen;
    for (const ProcInfo& p : ps) {
        auto key = std::make_pair(p.pid, p.created);
        seen.insert(key);
        Track& t = tracks_[key];
        t.last = p;
        t.samples.push_back({tick, p.mem, p.cpu});
        while (!t.samples.empty() && tick - t.samples.front().tick > 3 * 3600 * 1000ull)
            t.samples.erase(t.samples.begin());
    }
    for (auto it = tracks_.begin(); it != tracks_.end();)
        it = seen.count(it->first) ? std::next(it) : tracks_.erase(it);
}

bool ProcHistory::Ready(int minutes) const {
    return first_ && lastTick_ - first_ >= (ULONGLONG)minutes * 60000 - 30000;
}

std::vector<ProcHistory::Leak> ProcHistory::Leaks() const {
    std::vector<Leak> out;
    for (const auto& [key, t] : tracks_) {
        const std::vector<Sample>& s = t.samples;
        if (s.size() < 10) continue;
        const Sample& now = s.back();
        // замер примерно часовой давности: 55-90 минут назад
        const Sample* old = nullptr;
        for (const Sample& x : s)
            if (now.tick - x.tick <= 90 * 60000ull && now.tick - x.tick >= 55 * 60000ull) {
                old = &x;
                break;
            }
        if (!old || !old->mem) continue;
        if (now.mem < 2 * old->mem || now.mem - old->mem < 700ull * 1024 * 1024) continue;
        // рост ровный: последние пять замеров не падают (загрузка уровня - скачок, а не утечка)
        bool steady = true;
        for (size_t i = s.size() - 5; i + 1 < s.size(); ++i)
            if (s[i + 1].mem < s[i].mem * 97 / 100) steady = false;
        if (!steady) continue;
        Leak l;
        l.pid = key.first;
        l.name = t.last.name;
        l.path = t.last.path;
        l.was = old->mem;
        l.now = now.mem;
        l.minutes = (int)((now.tick - old->tick) / 60000);
        out.push_back(l);
    }
    std::sort(out.begin(), out.end(), [](const Leak& a, const Leak& b) { return a.now - a.was > b.now - b.was; });
    return out;
}

std::vector<ProcHistory::Hog> ProcHistory::CpuHogs(int minutes, bool backgroundOnly) const {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    double cpus = std::max<DWORD>(1, si.dwNumberOfProcessors);
    std::map<std::wstring, Hog> byName;
    for (const auto& [key, t] : tracks_) {
        const std::vector<Sample>& s = t.samples;
        if (s.size() < 2) continue;
        const Sample& now = s.back();
        const Sample* old = &s.front();
        for (const Sample& x : s)
            if (now.tick - x.tick <= (ULONGLONG)minutes * 60000) {
                old = &x;
                break;
            }
        if (old == &now || now.tick <= old->tick) continue;
        double pct = (double)(now.cpu - old->cpu) / 10000.0 / ((double)(now.tick - old->tick) * cpus) * 100.0;
        Hog& h = byName[Lower(t.last.name)];
        h.name = t.last.name;
        if (h.path.empty()) h.path = t.last.path;
        h.pids.push_back(key.first);
        h.percent += pct;
        h.window |= t.last.window;
        h.mem += t.last.mem;
    }
    std::vector<Hog> out;
    for (auto& [k, h] : byName)
        if (h.percent >= 0.5 && !(backgroundOnly && h.window) && k != L"radialmenu.exe") out.push_back(std::move(h));
    std::sort(out.begin(), out.end(), [](const Hog& a, const Hog& b) { return a.percent > b.percent; });
    return out;
}

std::vector<Leftover> BuildLeftovers(const std::vector<ProcInfo>& ps) {
    std::vector<Leftover> out;
    std::map<DWORD, const ProcInfo*> byPid;
    bool editor = false;
    for (const ProcInfo& p : ps) {
        byPid[p.pid] = &p;
        std::wstring n = Lower(p.name);
        editor |= n == L"unrealeditor.exe" || n == L"ue4editor.exe";
    }
    ULONGLONG now = NowFt();
    for (const ProcInfo& p : ps) {
        std::wstring n = Lower(p.name);
        int minutes = p.created ? (int)((now - p.created) / 600000000ull) : 0;
        Leftover l;
        l.pid = p.pid;
        l.name = p.name;
        l.mem = p.mem;
        l.minutes = minutes;
        if (Is(n, {L"shadercompileworker.exe", L"unrealbuildtool.exe", L"unrealheadertool.exe", L"unreallightmass.exe",
                   L"livecodingconsole.exe", L"crashreportclienteditor.exe"})) {
            if (editor || minutes < 2) continue;
            l.why = L"Unreal Editor закрыт, а этот процесс остался";
            out.push_back(l);
            continue;
        }
        if (!Is(n, {L"cl.exe", L"link.exe", L"mspdbsrv.exe", L"msbuild.exe", L"vctip.exe", L"cc1plus.exe", L"ld.exe",
                    L"ninja.exe", L"cmake.exe", L"node.exe", L"clangd.exe", L"esbuild.exe"}))
            continue;
        // родитель закрыт: его нет или номер уже у процесса, созданного позже
        auto par = byPid.find(p.parent);
        bool orphan = !p.parent || par == byPid.end() || par->second->created > p.created;
        if (!orphan || p.window || minutes < 10) continue;
        l.why = L"кто запустил - уже закрыт, работает " + std::to_wstring(minutes / 60 ? minutes / 60 : minutes) +
                (minutes / 60 ? L" ч" : L" мин");
        out.push_back(l);
    }
    std::sort(out.begin(), out.end(), [](const Leftover& a, const Leftover& b) { return a.mem > b.mem; });
    return out;
}

// ---------------- память ----------------

CommitInfo Commit() {
    CommitInfo c;
    PERFORMANCE_INFORMATION pi{sizeof(pi)};
    if (!GetPerformanceInfo(&pi, sizeof(pi))) return c;
    ULONGLONG page = pi.PageSize;
    c.total = pi.CommitTotal * page;
    c.limit = pi.CommitLimit * page;
    c.peak = pi.CommitPeak * page;
    c.phys = pi.PhysicalTotal * page;
    return c;
}

ULONGLONG PagefileBytes() {
    CommitInfo c = Commit();
    return c.limit > c.phys ? c.limit - c.phys : 0;
}

// ---------------- батарея ----------------

BatteryInfo Battery() {
    BatteryInfo b;
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        b.present = !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255;
        b.ac = ps.ACLineStatus == 1;
        b.charging = (ps.BatteryFlag & 8) != 0;
        if (ps.BatteryLifePercent <= 100) b.percent = ps.BatteryLifePercent;
        if (ps.BatteryLifeTime != (DWORD)-1) b.minutesLeft = (int)(ps.BatteryLifeTime / 60);
    }
    // ёмкость по паспорту и сейчас: IOCTL драйвера батареи (WMI BatteryStaticData у многих ноутбуков пуст)
    static const GUID kBattery = {0x72631e54, 0x78a4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};
    HDEVINFO dev = SetupDiGetClassDevsW(&kBattery, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev == INVALID_HANDLE_VALUE) return b;
    SP_DEVICE_INTERFACE_DATA did{sizeof(did)};
    if (SetupDiEnumDeviceInterfaces(dev, nullptr, &kBattery, 0, &did)) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(dev, &did, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need + sizeof(WCHAR));
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (need && SetupDiGetDeviceInterfaceDetailW(dev, &did, det, need, nullptr, nullptr)) {
            HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                BATTERY_QUERY_INFORMATION q{};
                DWORD wait = 0, got = 0;
                if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &q.BatteryTag,
                                    sizeof(q.BatteryTag), &got, nullptr) &&
                    q.BatteryTag) {
                    q.InformationLevel = BatteryInformation;
                    BATTERY_INFORMATION bi{};
                    if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof(q), &bi, sizeof(bi), &got,
                                        nullptr)) {
                        b.designed = bi.DesignedCapacity;
                        b.full = bi.FullChargedCapacity;
                        b.cycles = (int)bi.CycleCount;
                        b.present = true;
                    }
                }
                CloseHandle(h);
            }
        }
    }
    SetupDiDestroyDeviceInfoList(dev);
    return b;
}

std::vector<std::wstring> WakeDevices() {
    std::wstring out;
    std::vector<std::wstring> list;
    if (RunCapture(L"powercfg.exe /devicequery wake_armed", out, 10000) != 0) return list;
    for (const std::wstring& l : Split(out, L'\n')) {
        std::wstring t = Trim(l);
        if (!t.empty() && Lower(t) != L"нет" && Lower(t) != L"none") list.push_back(t);
    }
    return list;
}

// ---------------- сеть ----------------

void NetCounters(ULONGLONG& in, ULONGLONG& out) {
    in = out = 0;
    MIB_IF_TABLE2* t = nullptr;
    if (GetIfTable2(&t) != NO_ERROR || !t) return;
    for (ULONG i = 0; i < t->NumEntries; ++i) {
        const MIB_IF_ROW2& r = t->Table[i];
        // только настоящие адаптеры: без туннеля Happ, Hyper-V и фильтров Wi-Fi (иначе трафик посчитается дважды)
        if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface) continue;
        if (r.Type != IF_TYPE_ETHERNET_CSMACD && r.Type != IF_TYPE_IEEE80211) continue;
        if (r.OperStatus != IfOperStatusUp) continue;
        in += r.InOctets;
        out += r.OutOctets;
    }
    FreeMibTable(t);
}

namespace {

struct Conn4 {
    MIB_TCPROW row;
    DWORD pid;
    ULONGLONG in0, out0;
};
struct Conn6 {
    MIB_TCP6ROW row;
    DWORD pid;
    ULONGLONG in0, out0;
};

template <class Row, class Get>
bool ReadData(Row* r, Get get, ULONGLONG& in, ULONGLONG& out) {
    TCP_ESTATS_DATA_ROD_v0 rod{};
    if (get(r, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0, (PUCHAR)&rod, 0, sizeof(rod)) != NO_ERROR)
        return false;
    in = rod.DataBytesIn;
    out = rod.DataBytesOut;
    return true;
}

}  // namespace

std::vector<NetUse> NetTopAdminMain(int ms) {
    std::vector<Conn4> c4;
    std::vector<Conn6> c6;
    TCP_ESTATS_DATA_RW_v0 on{TRUE}, off{FALSE};
    {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        std::vector<BYTE> buf(size + 4096);
        size = (DWORD)buf.size();
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) == NO_ERROR) {
            auto* t = (MIB_TCPTABLE_OWNER_PID*)buf.data();
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCPROW_OWNER_PID& o = t->table[i];
                if (o.dwState != MIB_TCP_STATE_ESTAB) continue;
                Conn4 c{};
                c.row.dwState = o.dwState;
                c.row.dwLocalAddr = o.dwLocalAddr;
                c.row.dwLocalPort = o.dwLocalPort;
                c.row.dwRemoteAddr = o.dwRemoteAddr;
                c.row.dwRemotePort = o.dwRemotePort;
                c.pid = o.dwOwningPid;
                SetPerTcpConnectionEStats(&c.row, TcpConnectionEstatsData, (PUCHAR)&on, 0, sizeof(on), 0);
                c4.push_back(c);
            }
        }
    }
    {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        std::vector<BYTE> buf(size + 4096);
        size = (DWORD)buf.size();
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) == NO_ERROR) {
            auto* t = (MIB_TCP6TABLE_OWNER_PID*)buf.data();
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCP6ROW_OWNER_PID& o = t->table[i];
                if (o.dwState != MIB_TCP_STATE_ESTAB) continue;
                Conn6 c{};
                c.row.State = (MIB_TCP_STATE)o.dwState;
                memcpy(&c.row.LocalAddr, o.ucLocalAddr, 16);
                c.row.dwLocalScopeId = o.dwLocalScopeId;
                c.row.dwLocalPort = o.dwLocalPort;
                memcpy(&c.row.RemoteAddr, o.ucRemoteAddr, 16);
                c.row.dwRemoteScopeId = o.dwRemoteScopeId;
                c.row.dwRemotePort = o.dwRemotePort;
                c.pid = o.dwOwningPid;
                SetPerTcp6ConnectionEStats(&c.row, TcpConnectionEstatsData, (PUCHAR)&on, 0, sizeof(on), 0);
                c6.push_back(c);
            }
        }
    }
    // счётчики могли уже идти (кто-то включил раньше) - считаем разницу за окно
    for (Conn4& c : c4) ReadData(&c.row, GetPerTcpConnectionEStats, c.in0, c.out0);
    for (Conn6& c : c6) ReadData(&c.row, GetPerTcp6ConnectionEStats, c.in0, c.out0);
    Sleep(ms);
    std::map<DWORD, NetUse> by;
    double k = 1000.0 / std::max(ms, 1);
    for (Conn4& c : c4) {
        ULONGLONG in = 0, out = 0;
        if (ReadData(&c.row, GetPerTcpConnectionEStats, in, out)) {
            by[c.pid].down += (ULONGLONG)((in - std::min(in, c.in0)) * k);
            by[c.pid].up += (ULONGLONG)((out - std::min(out, c.out0)) * k);
        }
        SetPerTcpConnectionEStats(&c.row, TcpConnectionEstatsData, (PUCHAR)&off, 0, sizeof(off), 0);
    }
    for (Conn6& c : c6) {
        ULONGLONG in = 0, out = 0;
        if (ReadData(&c.row, GetPerTcp6ConnectionEStats, in, out)) {
            by[c.pid].down += (ULONGLONG)((in - std::min(in, c.in0)) * k);
            by[c.pid].up += (ULONGLONG)((out - std::min(out, c.out0)) * k);
        }
        SetPerTcp6ConnectionEStats(&c.row, TcpConnectionEstatsData, (PUCHAR)&off, 0, sizeof(off), 0);
    }
    std::map<DWORD, std::wstring> names;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        names[pe.th32ProcessID] = pe.szExeFile;
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    std::vector<NetUse> out;
    for (auto& [pid, u] : by) {
        if (u.down + u.up < 1024) continue;
        u.pid = pid;
        u.name = names.count(pid) ? names[pid] : L"?";
        out.push_back(u);
    }
    std::sort(out.begin(), out.end(), [](const NetUse& a, const NetUse& b) { return a.down + a.up > b.down + b.up; });
    return out;
}

std::wstring PowerRequestsAdminMain() {
    std::wstring req, timers;
    RunCapture(L"powercfg.exe /requests", req, 15000);
    RunCapture(L"powercfg.exe /waketimers", timers, 15000);
    // разделы без запросов («Нет.»/«None.») не нужны: оставляем только то, что держит или будит
    std::wstring out, section;
    for (const std::wstring& raw : Split(req, L'\n')) {
        std::wstring l = Trim(raw);
        if (l.empty()) continue;
        if (l.back() == L':' && l.find(L'[') == std::wstring::npos) {
            section = l.substr(0, l.size() - 1);
            continue;
        }
        std::wstring low = Lower(l);
        if (low == L"нет." || low == L"none.") continue;
        out += L"держит " + section + L": " + l + L"\n";
    }
    for (const std::wstring& raw : Split(timers, L'\n')) {
        std::wstring l = Trim(raw);
        std::wstring low = Lower(l);
        if (l.empty() || low.find(L"нет активных") != std::wstring::npos ||
            low.find(L"no active") != std::wstring::npos)
            continue;
        out += L"будит: " + l + L"\n";
    }
    return out;
}

// ---------------- новый автозапуск ----------------

std::vector<StartItem> NewStartupItems() {
    std::vector<StartItem> items = StartupItems(), fresh;
    std::wstring file = DataDir() + L"\\autostart.tsv";
    std::set<std::wstring> known;
    bool first = GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (!first) {
        std::string bytes;
        HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char chunk[8192];
            DWORD got = 0;
            while (ReadFile(f, chunk, sizeof(chunk), &got, nullptr) && got) bytes.append(chunk, got);
            CloseHandle(f);
        }
        for (const std::wstring& line : Split(Widen(bytes), L'\n')) {
            std::wstring key = line.substr(0, line.find(L'\t'));
            if (!key.empty()) known.insert(key);
        }
    }
    std::wstring all;
    for (const StartItem& it : items) {
        std::wstring key = it.where + L"|" + it.name;
        all += key + L"\t" + it.command + L"\n";
        if (!first && it.enabled && !known.count(key) && Lower(it.name).find(L"radial") == std::wstring::npos)
            fresh.push_back(it);
    }
    std::string utf = Narrow(all);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(h, utf.data(), (DWORD)utf.size(), &put, nullptr);
        CloseHandle(h);
    }
    return fresh;
}

}  // namespace tools
