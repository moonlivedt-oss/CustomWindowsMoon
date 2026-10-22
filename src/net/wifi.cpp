// winsock2 раньше windows.h (через wifi.h): иначе IP Helper (скорость связи) не видит свои типы
#include <winsock2.h>
#include <ws2tcpip.h>

#include "net/wifi.h"

#include <objidl.h>
#include <gdiplus.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <netlistmgr.h>

#include <algorithm>
#include <cwchar>
#include <map>

#include "gfx/qr.h"

namespace net {

namespace {

// Описатель Native Wifi на время одного вопроса.
struct Wlan {
    HANDLE h = nullptr;
    Wlan() {
        DWORD ver = 0;
        if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) h = nullptr;
    }
    ~Wlan() {
        if (h) WlanCloseHandle(h, nullptr);
    }
};

std::wstring Ssid(const DOT11_SSID& s) {
    std::string raw((const char*)s.ucSSID, std::min<ULONG>(s.uSSIDLength, DOT11_SSID_MAX_LENGTH));
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
    UINT cp = n ? CP_UTF8 : CP_ACP;  // имена бывают и не в UTF-8
    n = MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// значение тега в XML профиля (первое вхождение)
std::wstring Tag(const std::wstring& xml, const wchar_t* name) {
    std::wstring open = std::wstring(L"<") + name + L">", close = std::wstring(L"</") + name + L">";
    size_t a = xml.find(open), b = a == std::wstring::npos ? a : xml.find(close, a);
    return b == std::wstring::npos ? L"" : xml.substr(a + open.size(), b - a - open.size());
}

// XML не прячет & < > в пароле только в виде сущностей
std::wstring Unescape(std::wstring s) {
    const std::pair<const wchar_t*, const wchar_t*> ents[] = {
        {L"&lt;", L"<"}, {L"&gt;", L">"}, {L"&quot;", L"\""}, {L"&apos;", L"'"}, {L"&amp;", L"&"}};
    for (const auto& [from, to] : ents)
        for (size_t at; (at = s.find(from)) != std::wstring::npos;) s.replace(at, wcslen(from), to);
    return s;
}

}  // namespace

int WifiChannel(ULONG khz) {
    if (khz >= 2412000 && khz <= 2472000) return (int)(khz - 2407000) / 5000;
    if (khz == 2484000) return 14;
    if (khz >= 5150000 && khz <= 5895000) return (int)(khz - 5000000) / 5000;
    if (khz >= 5955000 && khz <= 7115000) return (int)(khz - 5950000) / 5000;  // 6 ГГц
    return 0;
}

bool ChannelsOverlap(int a, int b) {
    if (!a || !b) return false;
    bool a24 = a <= 14, b24 = b <= 14;
    if (a24 != b24) return false;
    return a24 ? std::abs(a - b) < 5 : a == b;
}

std::wstring WifiBandGuess(int mbps) {
    // 2,4 ГГц на 20/40 МГц даёт до ~300 Мбит/с (Wi-Fi 4 и 6 - 287-574 только на 40 МГц, у домашних роутеров редко);
    // выше 400 - каналы 80 МГц и шире, они бывают только в 5 и 6 ГГц
    if (mbps >= 400) return L"скорее 5 или 6 ГГц";
    if (mbps > 0 && mbps <= 150) return L"скорее 2,4 ГГц";
    return L"";
}

WifiInfo Wifi(bool scan) {
    WifiInfo r;
    Wlan w;
    if (!w.h) return r;
    WLAN_INTERFACE_INFO_LIST* list = nullptr;
    if (WlanEnumInterfaces(w.h, nullptr, &list) != ERROR_SUCCESS || !list) return r;
    for (DWORD i = 0; i < list->dwNumberOfItems && !r.ok; ++i) {
        const WLAN_INTERFACE_INFO& itf = list->InterfaceInfo[i];
        if (itf.isState != wlan_interface_state_connected) continue;
        r.adapter = itf.strInterfaceDescription;
        // скорость связи - из IP Helper: она есть и без местоположения
        NET_LUID luid{};
        if (ConvertInterfaceGuidToLuid(&itf.InterfaceGuid, &luid) == NO_ERROR) {
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = luid;
            if (GetIfEntry2(&row) == NO_ERROR) {
                r.linkRx = (int)(row.ReceiveLinkSpeed / 1000000);
                r.linkTx = (int)(row.TransmitLinkSpeed / 1000000);
            }
        }
        DWORD size = 0;
        WLAN_CONNECTION_ATTRIBUTES* conn = nullptr;
        DWORD err = WlanQueryInterface(w.h, &itf.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &size,
                                       (void**)&conn, nullptr);
        if (err == ERROR_ACCESS_DENIED) r.locationOff = true;
        if (err != ERROR_SUCCESS || !conn) continue;
        r.ok = true;
        const DOT11_MAC_ADDRESS& myBssid = conn->wlanAssociationAttributes.dot11Bssid;
        r.now.ssid = Ssid(conn->wlanAssociationAttributes.dot11Ssid);
        r.now.signal = (int)conn->wlanAssociationAttributes.wlanSignalQuality;
        r.now.open = !conn->wlanSecurityAttributes.bSecurityEnabled;
        if (scan) {  // свежий список: ответ приходит асинхронно, Windows обычно успевает за 2-4 с
            WlanScan(w.h, &itf.InterfaceGuid, nullptr, nullptr, nullptr);
            Sleep(3500);
        }
        WLAN_BSS_LIST* bss = nullptr;
        if (WlanGetNetworkBssList(w.h, &itf.InterfaceGuid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &bss) ==
                ERROR_SUCCESS &&
            bss) {
            std::map<std::wstring, WifiNet> best;  // по имени+каналу: у роутера бывает несколько точек
            for (DWORD k = 0; k < bss->dwNumberOfItems; ++k) {
                const WLAN_BSS_ENTRY& e = bss->wlanBssEntries[k];
                WifiNet n;
                n.ssid = Ssid(e.dot11Ssid);
                n.rssi = e.lRssi;
                n.signal = (int)e.uLinkQuality;
                n.channel = WifiChannel(e.ulChCenterFrequency);
                n.band5 = n.channel > 14;
                if (!memcmp(e.dot11Bssid, myBssid, sizeof(DOT11_MAC_ADDRESS))) {
                    r.now.channel = n.channel;
                    r.now.rssi = n.rssi;
                    r.now.band5 = n.band5;
                    continue;
                }
                if (n.ssid == r.now.ssid) continue;  // свои же точки (другой диапазон)
                std::wstring key = (n.ssid.empty() ? L"(скрытая)" : n.ssid) + L"|" + std::to_wstring(n.channel);
                auto it = best.find(key);
                if (it == best.end() || it->second.rssi < n.rssi) best[key] = n;
            }
            WlanFreeMemory(bss);
            for (auto& [k, n] : best) {
                if (n.ssid.empty()) n.ssid = L"(скрытая сеть)";
                r.all.push_back(n);
            }
            std::sort(r.all.begin(), r.all.end(), [](const WifiNet& a, const WifiNet& b) { return a.rssi > b.rssi; });
            for (const WifiNet& n : r.all)
                if (ChannelsOverlap(n.channel, r.now.channel)) r.same.push_back(n);
        }
        WlanFreeMemory(conn);
    }
    WlanFreeMemory(list);
    if (!r.ok) return r;
    // совет: на 2,4 ГГц - лучший из 1/6/11 по числу соседей; на 5 ГГц соседей мешает только тот же канал
    if (r.now.channel && r.now.channel <= 14 && r.same.size() >= 3) {
        int bestCh = 0;
        size_t fewest = SIZE_MAX;
        for (int ch : {1, 6, 11}) {
            size_t n = 0;
            for (const WifiNet& x : r.all) n += ChannelsOverlap(x.channel, ch) && x.rssi > -80;
            if (n < fewest) fewest = n, bestCh = ch;
        }
        r.advice = L"На канале " + std::to_wstring(r.now.channel) + L" и рядом ещё " + std::to_wstring(r.same.size()) +
                   L" сетей. " +
                   (bestCh != r.now.channel ? L"В настройках роутера свободнее канал " + std::to_wstring(bestCh)
                                            : std::wstring(L"Это и так самый свободный из 1, 6, 11")) +
                   L"; ещё лучше - 5 ГГц, если роутер умеет.";
    } else if (r.now.signal && r.now.signal < 40) {
        r.advice = L"Сигнал слабый: ближе к роутеру или без стен на пути станет заметно быстрее.";
    }
    return r;
}

std::wstring WifiPassword(const std::wstring& ssid, bool* open, bool* known) {
    if (open) *open = false;
    if (known) *known = false;
    Wlan w;
    if (!w.h) return L"";
    WLAN_INTERFACE_INFO_LIST* list = nullptr;
    if (WlanEnumInterfaces(w.h, nullptr, &list) != ERROR_SUCCESS || !list) return L"";
    std::wstring pass;
    for (DWORD i = 0; i < list->dwNumberOfItems && pass.empty(); ++i) {
        LPWSTR xml = nullptr;
        DWORD flags = WLAN_PROFILE_GET_PLAINTEXT_KEY, access = 0;
        // имя профиля обычно совпадает с именем сети
        if (WlanGetProfile(w.h, &list->InterfaceInfo[i].InterfaceGuid, ssid.c_str(), nullptr, &xml, &flags, &access) !=
                ERROR_SUCCESS ||
            !xml)
            continue;
        std::wstring x = xml;
        WlanFreeMemory(xml);
        if (known) *known = true;
        if (Tag(x, L"authentication") == L"open" && open) *open = true;
        // protected = true - ключ зашифрован (так отдают только администратору): показать нечего
        if (Tag(x, L"protected") != L"true") pass = Unescape(Tag(x, L"keyMaterial"));
    }
    WlanFreeMemory(list);
    return pass;
}

std::wstring WifiProfileNow() {
    std::wstring found;
    INetworkListManager* nlm = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager, (void**)&nlm)))
        return found;
    IEnumNetworks* nets = nullptr;
    if (SUCCEEDED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &nets)) && nets) {
        INetwork* n = nullptr;
        while (found.empty() && nets->Next(1, &n, nullptr) == S_OK) {
            BSTR name = nullptr;
            if (SUCCEEDED(n->GetName(&name)) && name) {
                // профиль Wi-Fi с таким именем есть - это беспроводная сеть (у проводной имени-профиля нет).
                // Повторно подключённой сети Windows дописывает номер: «Дом 2» - профиль «Дом»
                std::wstring nm = name;
                for (int pass = 0; pass < 2 && found.empty(); ++pass) {
                    bool known = false;
                    WifiPassword(nm, nullptr, &known);
                    if (known) found = nm;
                    size_t sp = nm.find_last_of(L' ');
                    if (sp == std::wstring::npos || sp + 1 >= nm.size() ||
                        nm.find_first_not_of(L"0123456789", sp + 1) != std::wstring::npos)
                        break;
                    nm.resize(sp);
                }
                SysFreeString(name);
            }
            n->Release();
        }
        nets->Release();
    }
    nlm->Release();
    return found;
}

bool WifiQrPng(const std::wstring& ssid, const std::wstring& password, bool open, const std::wstring& path) {
    using namespace Gdiplus;
    qr::Code c = qr::Encode(qr::WifiText(Utf8(ssid), Utf8(password), open));
    if (!c.size) return false;
    const int px = 12, quiet = 4, side = (c.size + 2 * quiet) * px, caption = 70;
    Bitmap bmp(side, side + caption, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.Clear(Color(255, 255, 255, 255));
    SolidBrush ink(Color(255, 0, 0, 0));
    for (int y = 0; y < c.size; ++y)
        for (int x = 0; x < c.size; ++x)
            if (c.At(x, y)) g.FillRectangle(&ink, (x + quiet) * px, (y + quiet) * px, px, px);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    FontFamily fam(L"Segoe UI");
    Font big(&fam, 22, FontStyleBold, UnitPixel), small(&fam, 16, FontStyleRegular, UnitPixel);
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    g.DrawString(ssid.c_str(), -1, &big, RectF(0, (REAL)side - 10, (REAL)side, 30), &center, &ink);
    std::wstring sub = open ? L"открытая сеть" : L"Камера телефона - и подключится";
    SolidBrush grey(Color(255, 90, 90, 90));
    g.DrawString(sub.c_str(), -1, &small, RectF(0, (REAL)side + 24, (REAL)side, 26), &center, &grey);
    CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    return bmp.Save(path.c_str(), &png, nullptr) == Ok;
}

}  // namespace net
