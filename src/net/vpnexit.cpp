// Выход VPN с лучшим пингом: конфиг Happ, замер мимо туннеля, файл замеров по странам.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>

#include <chrono>
#include <ctime>

#include "core/text.h"
#include "net/vpnexit.h"
#include "net/net.h"
#include "phone/phone.h"

namespace vpnexit {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;

std::string ReadBytes(const std::wstring& path, size_t max) {
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string u(std::min<DWORD>(GetFileSize(f, nullptr), (DWORD)max), '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    return u;
}

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

}  // namespace

std::vector<std::wstring> Bypass() {
    return ParseBypass(ReadBytes(Known(FOLDERID_LocalAppData) + L"\\Happ\\config.json", 4 << 20));
}

int DirectRtt(const std::wstring& ip, int port, int timeoutMs) {
    uint32_t from = phone::PickAddress(phone::LocalAdapters());
    if (!from || phone::HotspotIp(from)) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(from);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((u_short)port);
    int out = -1;
    if (bind(s, (sockaddr*)&local, sizeof(local)) == 0 && InetPtonW(AF_INET, ip.c_str(), &to.sin_addr) == 1) {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        auto t0 = std::chrono::steady_clock::now();
        connect(s, (sockaddr*)&to, sizeof(to));
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w))
            out = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                      .count();
    }
    closesocket(s);
    return out;
}

std::wstring Note(const std::wstring& game, const std::wstring& country, int ms) {
    if (game.empty() || country.empty() || ms <= 0) return L"";
    std::wstring path = Known(FOLDERID_RoamingAppData) + L"\\MoonDesk\\vpnexit.tsv";
    AcquireSRWLockExclusive(&g_lock);
    std::vector<Sample> list = Parse(Widen(ReadBytes(path, 1 << 20)));
    long long now = (long long)time(nullptr);
    int better = 0;
    std::wstring c = Better(list, game, country, ms, now, &better);
    list.push_back({game, country, ms, now});
    std::string u = Narrow(Save(list));
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
        CloseHandle(f);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (c.empty()) return L"";
    return L"Через выход «" + net::CountryName(c) + L"» эта игра шла за " + std::to_wstring(better) +
           L" мс - выберите его в Happ.";
}

}  // namespace vpnexit
