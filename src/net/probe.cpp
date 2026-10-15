// Пробы сети: прокси, адаптеры, маршруты, HTTPS, выходной узел, пинг, процессы прокси, DNS.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <netioapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include "net/net.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "core/perf.h"
#include "core/text.h"
#include "net/net_internal.h"

namespace net {

namespace {

constexpr wchar_t kInet[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return {};
    return Trim(buf);
}

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return 0;
    return v;
}

// «http=127.0.0.1:8080;https=127.0.0.1:8080» или «socks=...» -> адрес, через который пойдёт HTTPS
std::wstring ProxyHostPort(const std::wstring& server) {
    std::wstring s = server, pick;
    for (const std::wstring& part : Split(s, L';')) {
        size_t eq = part.find(L'=');
        if (eq == std::wstring::npos) {
            if (pick.empty()) pick = part;
            continue;
        }
        std::wstring scheme = Lower(part.substr(0, eq));
        if (scheme == L"https" || (scheme == L"http" && pick.empty()) || (scheme == L"socks" && pick.empty()))
            pick = part.substr(eq + 1);
    }
    for (const wchar_t* pre : {L"http://", L"https://", L"socks://", L"socks5://"})
        if (Lower(pick).rfind(pre, 0) == 0) pick = pick.substr(wcslen(pre));
    return Trim(pick);
}

bool SplitHostPort(const std::wstring& hp, std::wstring& host, int& port) {
    size_t c = hp.find_last_of(L':');
    if (c == std::wstring::npos || c == 0) return false;
    host = hp.substr(0, c);
    port = _wtoi(hp.c_str() + c + 1);
    if (!host.empty() && host.front() == L'[') host = host.substr(1, host.size() - 2);
    return port > 0 && port < 65536;
}

}  // namespace

void InitSockets() {
    static bool done = false;
    if (done) return;
    WSADATA wd;
    done = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
}

bool VpnName(const std::wstring& text) {
    std::wstring s = Lower(text);
    for (const wchar_t* h : {L"wireguard", L"wintun",      L"openvpn",  L"nekoray",   L"clash",      L"v2ray",
                             L"xray",      L"shadowsocks", L"hiddify",  L"amnezia",   L"outline",    L"proton",
                             L"nordlynx",  L"surfshark",   L"warp",     L"happ",      L"sing-tun",   L"singbox",
                             L"sing-box",  L"tap-windows", L"zerotier", L"tailscale", L"radmin vpn", L"hamachi"})
        if (s.find(h) != std::wstring::npos) return true;
    // короткие «tun» и «tap» - только отдельным словом, иначе ловят что попало
    for (const wchar_t* h : {L"tun", L"tap"}) {
        size_t n = wcslen(h);
        for (size_t at = s.find(h); at != std::wstring::npos; at = s.find(h, at + 1)) {
            bool l = at == 0 || !iswalpha(s[at - 1]);
            bool r = at + n >= s.size() || !iswalpha(s[at + n]);
            if (l && r) return true;
        }
    }
    return false;
}

int TcpConnectMs(const std::wstring& host, int port, int timeoutMs) {
    InitSockets();
    addrinfoW hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (GetAddrInfoW(host.c_str(), std::to_wstring(port).c_str(), &hints, &res) != 0 || !res) return -1;
    int out = -1;
    for (addrinfoW* a = res; a && out < 0; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        auto t0 = std::chrono::steady_clock::now();
        connect(s, a->ai_addr, (int)a->ai_addrlen);
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) {
            out = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                      .count();
        }
        closesocket(s);
    }
    FreeAddrInfoW(res);
    return out;
}

int TcpRtt(const std::wstring& ip, int port, int timeoutMs) { return TcpConnectMs(ip, port, timeoutMs); }

Proxy ReadProxy() {
    Proxy p;
    p.on = RegDword(HKEY_CURRENT_USER, kInet, L"ProxyEnable") != 0;
    p.server = RegStr(HKEY_CURRENT_USER, kInet, L"ProxyServer");
    if (p.server.empty()) p.on = false;
    p.pac = RegStr(HKEY_CURRENT_USER, kInet, L"AutoConfigURL");
    p.machine = RegStr(HKEY_LOCAL_MACHINE, kInet, L"ProxyServer");
    WINHTTP_PROXY_INFO wi{};
    if (WinHttpGetDefaultProxyConfiguration(&wi)) {
        if (wi.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && wi.lpszProxy) p.winhttp = wi.lpszProxy;
        if (wi.lpszProxy) GlobalFree(wi.lpszProxy);
        if (wi.lpszProxyBypass) GlobalFree(wi.lpszProxyBypass);
    }
    std::set<std::wstring> seen;
    for (const wchar_t* n : {L"HTTP_PROXY", L"HTTPS_PROXY", L"ALL_PROXY", L"FTP_PROXY"}) {
        wchar_t buf[1024];
        std::wstring v = GetEnvironmentVariableW(n, buf, 1024) ? Trim(buf) : L"";
        std::wstring stored = RegStr(HKEY_CURRENT_USER, L"Environment", n);
        for (const std::wstring& val : {v, stored})
            if (!val.empty() && seen.insert(std::wstring(n) + L"=" + val).second)
                p.env.push_back(std::wstring(n) + L"=" + val);
    }
    if (p.on) {
        std::wstring host;
        int port = 0;
        if (SplitHostPort(ProxyHostPort(p.server), host, port)) {
            std::wstring h = Lower(host);
            p.local = h == L"127.0.0.1" || h == L"localhost" || h == L"::1" || h.rfind(L"127.", 0) == 0;
            p.alive = TcpConnectMs(host, port, 900) >= 0;
        }
    }
    return p;
}

std::vector<Adapter> ReadAdapters() {
    std::vector<Adapter> out;
    ULONG size = 32 * 1024;
    std::vector<BYTE> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 3 && rc == ERROR_BUFFER_OVERFLOW; ++i) {
        buf.resize(size);
        rc =
            GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                 nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        Adapter ad;
        ad.name = a->FriendlyName ? a->FriendlyName : L"";
        ad.desc = a->Description ? a->Description : L"";
        ad.up = a->OperStatus == IfOperStatusUp;
        ad.vpn = VpnName(ad.name + L" " + ad.desc);
        ad.mtu = a->Mtu;
        ad.index = a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
        ad.gateway = a->FirstGatewayAddress != nullptr;
        out.push_back(ad);
    }
    return out;
}

bool RouteTo(const std::wstring& ip, std::wstring* adapter) {
    InitSockets();
    sockaddr_storage ss{};
    INT len = sizeof(ss);
    if (WSAStringToAddressW((LPWSTR)ip.c_str(), AF_INET, nullptr, (sockaddr*)&ss, &len) != 0) {
        len = sizeof(ss);
        if (WSAStringToAddressW((LPWSTR)ip.c_str(), AF_INET6, nullptr, (sockaddr*)&ss, &len) != 0) return false;
    }
    DWORD idx = 0;
    if (GetBestInterfaceEx((sockaddr*)&ss, &idx) != NO_ERROR) return false;
    for (const Adapter& a : ReadAdapters())
        if (a.index == idx) {
            if (adapter) *adapter = a.name;
            return a.vpn;
        }
    return false;
}

// ---- HTTPS через системные настройки ----

bool HttpGet(const wchar_t* host, const wchar_t* path, int timeoutMs, std::string* body, int* status) {
    // AUTOMATIC_PROXY берёт настройки пользователя, как браузер: застрявший прокси здесь и проявится
    HINTERNET s = WinHttpOpen(L"MoonRadial", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(L"MoonRadial", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    bool ok = false;
    if (HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE)) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, nullptr)) {
                DWORD code = 0, sz = sizeof(code);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz,
                                    nullptr);
                if (status) *status = (int)code;
                ok = code >= 200 && code < 400;
                if (body) {
                    char chunk[4096];
                    DWORD got = 0;
                    while (body->size() < 65536 && WinHttpReadData(r, chunk, sizeof(chunk), &got) && got)
                        body->append(chunk, got);
                }
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}

int HttpRtt(int timeoutMs) {
    PERF_SCOPE("net.HttpRtt");
    HINTERNET s = WinHttpOpen(L"MoonRadial", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return -1;
    WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    int best = -1;
    if (HINTERNET c = WinHttpConnect(s, L"cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        for (int i = 0; i < 4; ++i) {  // первый запрос открывает соединение (TLS), его время не считаем
            HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/cdn-cgi/trace", nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (!r) break;
            auto t0 = std::chrono::steady_clock::now();
            bool ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                      WinHttpReceiveResponse(r, nullptr);
            char buf[1024];
            DWORD got = 0;
            while (ok && WinHttpReadData(r, buf, sizeof(buf), &got) && got) {
            }
            int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                         .count();
            WinHttpCloseHandle(r);
            if (!ok) break;
            if (i > 0 && (best < 0 || ms < best)) best = ms;
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return best;
}

bool HttpsOk(int budgetMs) {
    PERF_SCOPE("net.HttpsOk");
    ULONGLONG t0 = GetTickCount64();
    const wchar_t* urls[][2] = {{L"www.gstatic.com", L"/generate_204"},
                                {L"cloudflare.com", L"/cdn-cgi/trace"},
                                {L"www.msftconnecttest.com", L"/connecttest.txt"}};
    for (auto& u : urls) {
        int left = budgetMs - (int)(GetTickCount64() - t0);
        if (left < 800) break;
        if (HttpGet(u[0], u[1], std::min(left, 5000), nullptr, nullptr)) return true;
    }
    return false;
}

Exit ExitNode(int timeoutMs) {
    PERF_SCOPE("net.ExitNode");
    Exit e;
    std::string body;
    if (!HttpGet(L"cloudflare.com", L"/cdn-cgi/trace", timeoutMs, &body, nullptr)) return e;
    for (const std::wstring& line : Split(Widen(body), L'\n')) {
        if (line.rfind(L"ip=", 0) == 0) e.ip = line.substr(3);
        if (line.rfind(L"loc=", 0) == 0) e.country = line.substr(4);
        if (line.rfind(L"colo=", 0) == 0) e.colo = line.substr(5);
    }
    e.ok = !e.ip.empty();
    return e;
}

std::wstring CountryName(const std::wstring& code) {
    if (code.empty()) return {};
    using Fn = int(WINAPI*)(LPCWSTR, DWORD, LPWSTR, int);  // GetGeoInfoEx: Windows 10 1709+
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetGeoInfoEx");
    wchar_t buf[128] = L"";
    constexpr DWORD kFriendly = 8;  // GEO_FRIENDLYNAME
    if (fn && fn(code.c_str(), kFriendly, buf, 128) > 0) return buf;
    return code;
}

// ---- пинг ----

int Ping(const std::wstring& host, int count, int timeoutMs, int* lossPct) {
    InitSockets();
    if (lossPct) *lossPct = 100;
    addrinfoW hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (GetAddrInfoW(host.c_str(), nullptr, &hints, &res) != 0 || !res) return -1;
    IPAddr dst = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    FreeAddrInfoW(res);
    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) return -1;
    char data[32] = "MoonRadialPing";
    BYTE reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 64];
    int got = 0, sum = 0;
    for (int i = 0; i < count; ++i) {
        if (IcmpSendEcho(h, dst, data, sizeof(data), nullptr, reply, sizeof(reply), timeoutMs) > 0) {
            auto* r = (ICMP_ECHO_REPLY*)reply;
            if (r->Status == IP_SUCCESS) {
                ++got;
                sum += (int)r->RoundTripTime;
            }
        }
    }
    IcmpCloseHandle(h);
    if (lossPct) *lossPct = count ? (count - got) * 100 / count : 100;
    return got ? sum / got : -1;
}

// ---- процессы прокси-клиентов ----

std::vector<ProxyProc> ProxyProcesses() {
    static const std::set<int> kPorts = {1080,  1081,  1082,  2080,  2081,  3128,  7890,  7891,  7892, 7897, 8118,
                                         10800, 10801, 10808, 10809, 10810, 20170, 20171, 12334, 2334, 10086};
    std::map<DWORD, std::set<int>> ports;
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<BYTE> buf(size + 4096);
    size = (ULONG)buf.size();
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        auto* t = (MIB_TCPTABLE_OWNER_PID*)buf.data();
        for (DWORD i = 0; i < t->dwNumEntries; ++i)
            ports[t->table[i].dwOwningPid].insert(ntohs((u_short)t->table[i].dwLocalPort));
    }
    std::vector<ProxyProc> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID <= 4 || pe.th32ProcessID == self) continue;
        std::wstring n = Lower(pe.szExeFile);
        bool byName = false;
        for (const wchar_t* h :
             {L"nekoray", L"nekobox", L"v2ray",     L"xray",        L"sing-box", L"singbox", L"clash",
              L"mihomo",  L"hiddify", L"happ",      L"shadowsocks", L"ss-local", L"sslocal", L"tun2socks",
              L"qv2ray",  L"netch",   L"proxifier", L"v2rayn",      L"warp-svc", L"amnezia", L"outline"})
            if (n.find(h) != std::wstring::npos) byName = true;
        std::vector<int> mine;
        auto it = ports.find(pe.th32ProcessID);
        bool byPort = false;
        if (it != ports.end())
            for (int p : it->second) {
                mine.push_back(p);
                if (kPorts.count(p)) byPort = true;
            }
        if (!byName && !byPort) continue;
        ProxyProc pp;
        pp.pid = pe.th32ProcessID;
        pp.name = pe.szExeFile;
        pp.ports = mine;
        pp.byName = byName;
        out.push_back(pp);
    }
    CloseHandle(snap);
    std::sort(out.begin(), out.end(), [](const ProxyProc& a, const ProxyProc& b) {
        if (a.byName != b.byName) return a.byName;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

bool Kill(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    bool ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok;
}

// ---- DNS ----

std::vector<DnsResult> TestDns() {
    InitSockets();
    const wchar_t* servers[][2] = {{L"Cloudflare", L"1.1.1.1"}, {L"Google", L"8.8.8.8"},
                                   {L"Quad9", L"9.9.9.9"},      {L"OpenDNS", L"208.67.222.222"},
                                   {L"Яндекс", L"77.88.8.8"},   {L"AdGuard", L"94.140.14.14"}};
    // запрос A example.com
    const unsigned char q[] = {0x12, 0x34, 1,   0,   0,   1, 0,   0,   0,   0, 0, 0, 7, 'e', 'x',
                               'a',  'm',  'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
    std::vector<DnsResult> out;
    for (auto& sv : servers) {
        DnsResult r;
        r.name = sv[0];
        r.ip = sv[1];
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            DWORD to = 1500;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(53);
            InetPtonW(AF_INET, sv[1], &a.sin_addr);
            auto t0 = std::chrono::steady_clock::now();
            if (sendto(s, (const char*)q, sizeof(q), 0, (sockaddr*)&a, sizeof(a)) > 0) {
                char buf[512];
                int n = recv(s, buf, sizeof(buf), 0);
                if (n > 12 && (unsigned char)buf[0] == 0x12 && (unsigned char)buf[1] == 0x34)
                    r.ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                      t0)
                               .count();
            }
            closesocket(s);
        }
        out.push_back(r);
    }
    std::stable_sort(out.begin(), out.end(), [](const DnsResult& a, const DnsResult& b) {
        if ((a.ms < 0) != (b.ms < 0)) return a.ms >= 0;
        return a.ms < b.ms;
    });
    return out;
}

// ---- остатки VPN: маршруты на отключённых адаптерах ----

std::vector<StaleRoute> StaleRoutes() {
    std::vector<StaleRoute> out;
    std::map<NET_IFINDEX, Adapter> byIndex;
    for (const Adapter& a : ReadAdapters()) byIndex[a.index] = a;
    MIB_IPFORWARD_TABLE2* t = nullptr;
    if (GetIpForwardTable2(AF_INET, &t) != NO_ERROR || !t) return out;
    for (ULONG i = 0; i < t->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& r = t->Table[i];
        auto ad = byIndex.find(r.InterfaceIndex);
        if (ad == byIndex.end() || ad->second.up) continue;  // неизвестный интерфейс - служебный, не трогаем
        const BYTE* b = (const BYTE*)&r.DestinationPrefix.Prefix.Ipv4.sin_addr;
        if (b[0] == 127 || b[0] >= 224) continue;  // loopback, multicast, broadcast
        wchar_t s[64];
        swprintf(s, 64, L"%u.%u.%u.%u/%u", b[0], b[1], b[2], b[3], r.DestinationPrefix.PrefixLength);
        StaleRoute sr;
        sr.prefix = s;
        sr.adapter = ad->second.name;
        sr.row = r;
        out.push_back(sr);
    }
    FreeMibTable(t);
    return out;
}

// ---- hosts ----

std::vector<std::wstring> SuspiciousHosts(std::vector<std::wstring>* all) {
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\drivers\\etc\\hosts", path, MAX_PATH);
    std::vector<std::wstring> out;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::string raw(256 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, raw.data(), (DWORD)raw.size(), &got, nullptr);
    CloseHandle(f);
    raw.resize(got);
    for (std::wstring line : Split(Widen(raw), L'\n')) {
        if (line.empty() || line[0] == L'#') continue;
        size_t hash = line.find(L'#');
        if (hash != std::wstring::npos) line = Trim(line.substr(0, hash));
        std::vector<std::wstring> parts;
        for (const std::wstring& p : Split(line, L' '))
            for (const std::wstring& q : Split(p, L'\t'))
                if (!q.empty()) parts.push_back(q);
        if (parts.size() < 2) continue;
        bool plain = true;
        for (size_t i = 1; i < parts.size(); ++i) {
            std::wstring n = Lower(parts[i]);
            if (n != L"localhost" && n != L"localhost.localdomain") plain = false;
        }
        if (plain) continue;
        if (all) all->push_back(line);
        // подозрительно: настоящий домен уведён на 0.0.0.0 или loopback (блокировка сайта);
        // записи dev-инструментов (*.docker.internal, *.local, *.localhost) безвредны
        const std::wstring& ip = parts[0];
        bool bad = ip == L"0.0.0.0";
        for (size_t i = 1; i < parts.size() && !bad; ++i) {
            std::wstring n = Lower(parts[i]);
            if (n == L"localhost" || EndsWithI(n, L".docker.internal") || EndsWithI(n, L".local") ||
                EndsWithI(n, L".localhost"))
                continue;
            if ((ip == L"127.0.0.1" || ip == L"::1") && n.find(L'.') != std::wstring::npos) bad = true;
        }
        if (bad) out.push_back(line);
    }
    return out;
}

}  // namespace net
