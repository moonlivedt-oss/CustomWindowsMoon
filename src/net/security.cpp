#include <winsock2.h>
#include <windows.h>
#include <netfw.h>
#include <netlistmgr.h>
#include <wlanapi.h>

#include "net/security.h"

namespace net {
namespace {
const CLSID kFwPolicy = {0xe2b3c97f, 0x6ae1, 0x41ac, {0x81, 0x7a, 0xf6, 0xf9, 0x21, 0x66, 0xd7, 0xdd}};
const IID kFwPolicy2 = {0x98325047, 0xc671, 0x4174, {0x8d, 0x81, 0xde, 0xfc, 0xd3, 0xf0, 0x31, 0x86}};
const CLSID kNetworkList = {0xdcb00c01, 0x570f, 0x4a9b, {0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b}};
const IID kCostManager = {0xdcb00008, 0x570f, 0x4a9b, {0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b}};
struct Com {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Com() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
}  // namespace

SecurityState ReadSecurity() {
    SecurityState s;
    Com com;
    INetFwPolicy2* fw = nullptr;
    if (SUCCEEDED(CoCreateInstance(kFwPolicy, nullptr, CLSCTX_INPROC_SERVER, kFwPolicy2, (void**)&fw))) {
        long profiles = 0;
        if (SUCCEEDED(fw->get_CurrentProfileTypes(&profiles)) && profiles) {
            bool complete = true, off = false;
            for (auto p : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
                if (!(profiles & p)) continue;
                VARIANT_BOOL enabled = VARIANT_FALSE;
                if (FAILED(fw->get_FirewallEnabled(p, &enabled))) complete = false;
                else if (enabled == VARIANT_FALSE) off = true;
            }
            if (complete || off) s.known |= FIREWALL_OFF;
            if (off) s.risks |= FIREWALL_OFF;
        }
        fw->Release();
    }

    HANDLE wlan = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &wlan) != ERROR_SUCCESS) return s;
    WLAN_INTERFACE_INFO_LIST* interfaces = nullptr;
    if (WlanEnumInterfaces(wlan, nullptr, &interfaces) == ERROR_SUCCESS && interfaces) {
        bool complete = true;
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            const auto& item = interfaces->InterfaceInfo[i];
            if (item.isState != wlan_interface_state_connected) continue;
            DWORD size = 0;
            WLAN_CONNECTION_ATTRIBUTES* c = nullptr;
            if (WlanQueryInterface(wlan, &item.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr,
                                   &size, (void**)&c, nullptr) != ERROR_SUCCESS || !c) {
                complete = false;  // в том числе запрет местоположения: его не обходим и не запрашиваем
                continue;
            }
            const auto& a = c->wlanSecurityAttributes;
            if (!a.bSecurityEnabled || a.dot11CipherAlgorithm == DOT11_CIPHER_ALGO_NONE) s.risks |= WIFI_OPEN;
            if (a.dot11CipherAlgorithm == DOT11_CIPHER_ALGO_WEP ||
                a.dot11CipherAlgorithm == DOT11_CIPHER_ALGO_WEP40 ||
                a.dot11CipherAlgorithm == DOT11_CIPHER_ALGO_WEP104 ||
                a.dot11CipherAlgorithm == DOT11_CIPHER_ALGO_TKIP) s.risks |= WIFI_LEGACY;
            WlanFreeMemory(c);
        }
        if (complete) s.known |= WIFI_OPEN | WIFI_LEGACY;
        s.known |= s.risks & (WIFI_OPEN | WIFI_LEGACY);
        WlanFreeMemory(interfaces);
    }
    WlanCloseHandle(wlan, nullptr);
    return s;
}

std::wstring SecurityAdvice(unsigned risks) {
    std::wstring text;
    if (risks & FIREWALL_OFF)
        text += L"Брандмауэр Windows отключён для активного профиля сети. Проверьте «Безопасность Windows — "
                L"Брандмауэр и защита сети». Если используете сторонний межсетевой экран, проверьте его состояние. ";
    if (risks & WIFI_OPEN)
        text += L"У подключённой Wi-Fi нет шифрования радиоканала. Предпочтите защищённую сеть; используйте HTTPS. ";
    if (risks & WIFI_LEGACY)
        text += L"У подключённой Wi-Fi устаревшее шифрование WEP/TKIP. Для своего роутера выберите WPA2-AES или WPA3. ";
    return text;
}

bool BackgroundTrafficAllowed() {
    Com com;
    INetworkCostManager* manager = nullptr;
    if (FAILED(CoCreateInstance(kNetworkList, nullptr, CLSCTX_INPROC_SERVER, kCostManager, (void**)&manager)))
        return false;
    DWORD cost = NLM_CONNECTION_COST_UNKNOWN;
    HRESULT hr = manager->GetCost(&cost, nullptr);
    manager->Release();
    return SUCCEEDED(hr) && cost == NLM_CONNECTION_COST_UNRESTRICTED;
}

}  // namespace net
