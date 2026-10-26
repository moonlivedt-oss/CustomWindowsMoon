// «Телефон рядом»: сессия передачи - ключ, адрес, буфер ПК для страницы, команды пульта, конец по времени.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <netfw.h>
#include <netlistmgr.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <ctime>
#include <memory>

#include "app/autos.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/macro.h"
#include "core/secrets.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "core/winlist.h"
#include "desk/corners.h"
#include "desk/ocr.h"
#include "desk/volume.h"
#include "phone/cert.h"
#include "phone/phone_internal.h"

namespace phone {

using namespace detail;

namespace {

HWND g_app = nullptr;
void (*g_runner)(const std::wstring&) = nullptr;  // main: действие из кольца (action:..., keys:, macro:)

std::wstring OutboxFile() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir + L"\\MoonDesk\\phone-outbox.txt";
}

const Config* g_cfg = nullptr;
BoostCard g_note;  // итог: «Передано на телефон», «С телефона: 5 фото - показать»
int g_port = 0, g_tlsPort = 0;
std::vector<PCCERT_CONTEXT> g_certs;  // сертификаты сервера этой передачи (Schannel держит их до конца)
uint32_t g_certIp = ~0u;
uint32_t g_ip = 0;
bool g_loopback = false;
std::wstring g_hint;     // что делать на выбранном пути (раздача, Bluetooth)
std::wstring g_netHint;  // общественная сеть, брандмауэр
bool g_fwOk = true;
bool g_bluetooth = false;
DWORD g_clipSeq = 0;
ULONGLONG g_addrAt = 0;
// пачка принятого с телефона: карточка «показать», когда 4 с ничего не приходит
int g_batch = 0;
std::wstring g_batchLast;
ULONGLONG g_batchAt = 0;
std::wstring g_status;
SRWLOCK g_cfgLock = SRWLOCK_INIT;  // папки для телефона читают потоки сервера
std::vector<std::wstring> g_roots;
std::atomic<bool> g_input{true};
std::atomic<bool> g_screen{false}, g_files{false}, g_screenStopped{false};
HWND g_watchFrame[4] = {};  // рамка по краю экрана, пока телефон смотрит экран
BoostCard g_alert;          // приложение на телефоне: уведомление, SMS, звонок
bool g_callShown = false;   // на g_alert сейчас «Звонит» - убрать, когда взяли или сбросили
BoostCard g_watchCard;      // «Телефон смотрит экран - Стоп»
bool g_watching = false;
void Watching(bool on);  // ниже: рамка и карточка, пока телефон смотрит экран

struct CallReq {
    std::string op, out;
    bool ok = false;
};
ULONGLONG g_cfgAt = 0, g_rateAt = 0, g_standAt = 0;
long long g_sentWas = 0, g_recvWas = 0;
std::wstring g_rate;      // «Отдаю 38 МБ/с, ещё 20 с» - на карточке
std::wstring g_lastScan;  // скан с телефона, который распознаётся

struct Cmd {
    std::string c;
    float vol = -1;
};

ThemeInfo Theme() { return CurrentTheme(*g_cfg); }

std::wstring SizeText(long long b) {
    wchar_t s[32];
    if (b < (1 << 20))
        swprintf(s, 32, L"%lld КБ", std::max(1ll, b >> 10));
    else if (b < (1ll << 30))
        swprintf(s, 32, L"%.1f МБ", b / 1048576.0);
    else
        swprintf(s, 32, L"%.2f ГБ", b / 1073741824.0);
    std::wstring t = s;
    std::replace(t.begin(), t.end(), L'.', L',');
    return t;
}

std::vector<Adapter> Adapters() {
    std::vector<Adapter> out;
    ULONG len = 16 << 10;
    std::vector<unsigned char> buf(len);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &len);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(len);
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &len);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::wstring d = Lower(std::wstring(a->Description ? a->Description : L"") + L" " +
                               (a->FriendlyName ? a->FriendlyName : L""));
        bool virt = a->IfType == 53 || a->IfType == IF_TYPE_TUNNEL;
        for (const wchar_t* w :
             {L"virtual", L"vpn", L"tap-", L"wintun", L"wireguard", L"hyper-v", L"vmware", L"virtualbox", L"vethernet",
              L"wsl", L"happ", L"xray", L"tun", L"zerotier", L"tailscale", L"radmin", L"hamachi"})
            if (d.find(w) != std::wstring::npos) virt = true;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            uint32_t ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
            if ((ip >> 16) == ((169u << 8) | 254u)) continue;  // адрес без DHCP - сети нет
            Adapter x;
            x.name = a->FriendlyName ? a->FriendlyName : L"";
            x.ip = ip;
            x.type = (int)a->IfType;
            x.virt = virt && !HotspotIp(ip);  // хот-спот Windows - «Wi-Fi Direct Virtual Adapter», но он нам и нужен
            x.guid = Widen(a->AdapterName ? a->AdapterName : "");
            x.mac = MacText(a->PhysicalAddress, a->PhysicalAddressLength);
            x.prefix = u->OnLinkPrefixLength;
            out.push_back(x);
        }
    }
    return out;
}

bool HasBluetooth() {  // есть включённый Bluetooth: радио находится только у включённого адаптера
    struct Params {
        DWORD size;
    };
    using FindFirst = HANDLE(WINAPI*)(const Params*, HANDLE*);
    using FindClose = BOOL(WINAPI*)(HANDLE);
    HMODULE m = LoadLibraryExW(L"bthprops.cpl", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
    auto first = (FindFirst)(void*)GetProcAddress(m, "BluetoothFindFirstRadio");
    auto close = (FindClose)(void*)GetProcAddress(m, "BluetoothFindRadioClose");
    bool ok = false;
    if (first && close) {
        Params p{sizeof(p)};
        HANDLE radio = nullptr;
        if (HANDLE f = first(&p, &radio)) {
            ok = true;
            CloseHandle(radio);
            close(f);
        }
    }
    FreeLibrary(m);
    if (ok) return true;
    // радио не нашлось, но адаптер есть (Bluetooth выключен) - окно передачи само предложит включить
    static const GUID kClass = {0xE0CBF06C, 0xCD8B, 0x4647, {0xBB, 0x8A, 0x26, 0x3B, 0x43, 0xF0, 0xF9, 0x74}};
    HDEVINFO h = SetupDiGetClassDevsW(&kClass, nullptr, nullptr, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA d{sizeof(d)};
    ok = SetupDiEnumDeviceInfo(h, 0, &d);
    SetupDiDestroyDeviceInfoList(h);
    return ok;
}

// Пропустит ли брандмауэр входящие к этой программе (без прав - только спросить)
bool FirewallAllows(int port) {
    static const CLSID kFwMgr = {0x304CE942, 0x6E39, 0x40D8, {0x94, 0x3A, 0xB9, 0x13, 0xC4, 0x0C, 0x9C, 0xD4}};
    static const IID kIFwMgr = {0xF7898AF5, 0xCAC4, 0x4632, {0xA2, 0xEC, 0xDA, 0x06, 0xE5, 0x11, 0x1A, 0xF2}};
    INetFwMgr* fw = nullptr;
    if (FAILED(CoCreateInstance(kFwMgr, nullptr, CLSCTX_INPROC_SERVER, kIFwMgr, (void**)&fw)) || !fw) return true;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    BSTR e = SysAllocString(exe);
    VARIANT allowed, restricted;
    VariantInit(&allowed);
    VariantInit(&restricted);
    bool ok = true;
    if (SUCCEEDED(
            fw->IsPortAllowed(e, NET_FW_IP_VERSION_V4, port, nullptr, NET_FW_IP_PROTOCOL_TCP, &allowed, &restricted)))
        ok = allowed.vt == VT_BOOL && allowed.boolVal != VARIANT_FALSE;
    SysFreeString(e);
    fw->Release();
    return ok;
}

// Сеть адаптера с адресом для телефона помечена общественной? Тогда брандмауэр телефон не пустит. Пусто - нет.
std::wstring PublicNetwork(const std::wstring& adapterGuid) {
    std::wstring found;
    GUID want{};
    if (adapterGuid.empty() || FAILED(CLSIDFromString(adapterGuid.c_str(), &want))) return found;
    INetworkListManager* nlm = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager, (void**)&nlm)))
        return found;
    IEnumNetworkConnections* list = nullptr;
    if (SUCCEEDED(nlm->GetNetworkConnections(&list)) && list) {
        INetworkConnection* c = nullptr;
        while (list->Next(1, &c, nullptr) == S_OK && c) {
            GUID id{};
            INetwork* n = nullptr;
            if (SUCCEEDED(c->GetAdapterId(&id)) && IsEqualGUID(id, want) && SUCCEEDED(c->GetNetwork(&n)) && n) {
                NLM_NETWORK_CATEGORY cat = NLM_NETWORK_CATEGORY_PRIVATE;
                BSTR name = nullptr;
                if (SUCCEEDED(n->GetCategory(&cat)) && cat == NLM_NETWORK_CATEGORY_PUBLIC &&
                    SUCCEEDED(n->GetName(&name)) && name) {
                    found = name;
                    SysFreeString(name);
                }
                n->Release();
            }
            c->Release();
        }
        list->Release();
    }
    nlm->Release();
    return found;
}

std::string NewToken() {
    unsigned char b[12];
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return Token(b, sizeof(b));
}

void PutText(const std::wstring& t) {
    if (!OpenClipboard(g_app)) return;
    EmptyClipboard();
    HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (m) {
        memcpy(GlobalLock(m), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(m);
        if (!SetClipboardData(CF_UNICODETEXT, m)) GlobalFree(m);
    }
    CloseClipboard();
}

PhoneCard::View MakeView();

std::wstring DevicesFile() {
    PWSTR p = nullptr;
    std::wstring dir;
    // проверка сервера (--phone-serve) без своих запомненных телефонов: папка из RADIAL_PHONE_DIR
    wchar_t test[MAX_PATH];
    if (GetEnvironmentVariableW(L"RADIAL_PHONE_LOCAL", nullptr, 0) > 0 &&
        GetEnvironmentVariableW(L"RADIAL_PHONE_DIR", test, MAX_PATH) - 1u < MAX_PATH - 1u)
        return std::wstring(test) + L"/phone-devices.tsv";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir + L"\\MoonDesk\\phone-devices.tsv";
}

// Файл запомненных телефонов - под DPAPI (расшифрует только эта учётная запись Windows): ключ в нём - пропуск к
// буферу, файлам и пульту. Старый файл открытым текстом переписывается при первом чтении. Пишут поток сервера
// («Запомнить», «заходил») и основной («Забыть», забыть давних) - под своим замком.
SRWLOCK g_devLock = SRWLOCK_INIT;
std::vector<DeviceRec> g_devCache;
ULONGLONG g_devAt = 0;               // когда читан файл (0 - перечитать)
std::vector<std::string> g_devSeen;  // ключи, которые заходили после последней записи
ULONGLONG g_devFlushAt = 0;
constexpr char kDevMagic[] = "MoonDesk-DPAPI-1\n";

std::string Dpapi(const std::string& data, bool protect) {
    static const char kEntropy[] = "MoonDesk phone-devices";
    DATA_BLOB in{(DWORD)data.size(), (BYTE*)data.data()}, out{}, ent{sizeof(kEntropy) - 1, (BYTE*)kEntropy};
    BOOL ok = protect
                  ? CryptProtectData(&in, L"MoonDesk phones", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                  : CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    std::string r;
    if (ok && out.pbData) r.assign((const char*)out.pbData, out.cbData);
    if (out.pbData) {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    return r;
}

// под g_devLock; legacy - файл был открытым текстом
std::vector<DeviceRec> ReadDevices(bool* legacy) {
    if (legacy) *legacy = false;
    HANDLE f = CreateFileW(DevicesFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string u(std::min<DWORD>(GetFileSize(f, nullptr), 1 << 20), '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    size_t m = sizeof(kDevMagic) - 1;
    if (u.compare(0, m, kDevMagic) == 0) {
        std::string plain = Dpapi(u.substr(m), false);
        std::vector<DeviceRec> list = ParseDevices(plain);
        SecureZeroMemory(plain.data(), plain.size());
        return list;
    }
    if (legacy) *legacy = !u.empty();
    return ParseDevices(u);
}

// под g_devLock: целиком, через временный файл (оборванная запись не потеряет остальные телефоны)
bool WriteDevices(const std::vector<DeviceRec>& list) {
    std::wstring path = DevicesFile(), tmp = path + L".tmp";
    g_devAt = 0;
    if (list.empty()) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    std::string plain = DevicesText(list), blob = Dpapi(plain, true);
    SecureZeroMemory(plain.data(), plain.size());
    if (blob.empty()) return false;
    blob.insert(0, kDevMagic);
    SHCreateDirectoryExW(nullptr, ParentDir(path).c_str(), nullptr);
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, blob.data(), (DWORD)blob.size(), &put, nullptr) && put == blob.size();
    CloseHandle(f);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Список из файла (не чаще раза в 2 с); в основном потоке: старый формат переписывается, давние забываются
std::vector<DeviceRec> DeviceRecs() {
    std::vector<std::wstring> gone;
    bool migrated = false;
    std::vector<DeviceRec> list;
    AcquireSRWLockExclusive(&g_devLock);
    ULONGLONG now = GetTickCount64();
    if (!g_devAt || now - g_devAt > 2000) {
        bool legacy = false;
        g_devCache = ReadDevices(&legacy);
        long long t = (long long)time(nullptr);
        size_t was = g_devCache.size();
        for (auto it = g_devCache.begin(); it != g_devCache.end();)
            if (DeviceStale(*it, t)) {
                gone.push_back(it->name.empty() ? L"телефон" : it->name);
                it = g_devCache.erase(it);
            } else {
                ++it;
            }
        if (legacy || g_devCache.size() != was) migrated = WriteDevices(g_devCache) && legacy;
        g_devAt = GetTickCount64();
    }
    list = g_devCache;
    ReleaseSRWLockExclusive(&g_devLock);
    if (migrated) autos::Note(L"Телефон рядом: ключи телефонов перенесены под защиту Windows (DPAPI)");
    for (const std::wstring& n : gone)
        autos::Note(L"Телефон рядом: забыт «" + n + L"» - не заходил " + std::to_wstring(kDeviceDays) + L" дней");
    return list;
}

std::vector<std::string> LoadDevices() {
    std::vector<std::string> out;
    for (const DeviceRec& d : DeviceRecs()) out.push_back(d.key);
    return out;
}

// «заходил» - в файл раз в 10 минут, а не на каждый запрос
void FlushSeen(bool now) {
    ULONGLONG t = GetTickCount64();
    if (!now && t - g_devFlushAt < 600000) return;
    g_devFlushAt = t;
    AcquireSRWLockExclusive(&g_devLock);
    if (!g_devSeen.empty()) {
        std::vector<DeviceRec> list = ReadDevices(nullptr);
        long long unix = (long long)time(nullptr);
        for (DeviceRec& d : list)
            if (std::find(g_devSeen.begin(), g_devSeen.end(), d.key) != g_devSeen.end()) d.seen = unix;
        if (WriteDevices(list)) g_devSeen.clear();
    }
    ReleaseSRWLockExclusive(&g_devLock);
}

void RefreshCfg() {
    ULONGLONG now = GetTickCount64();
    if (!g_cfg || now - g_cfgAt < 3000) return;
    g_cfgAt = now;
    std::wstring spec = g_cfg->phoneShare.empty() ? L"{music}|{videos}|{downloads}|{pictures}" : g_cfg->phoneShare;
    std::vector<std::wstring> roots;
    for (const std::wstring& part : Split(spec, L'|')) {
        std::wstring p = ExpandPath(part);
        while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
        DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) roots.push_back(p);
    }
    AcquireSRWLockExclusive(&g_cfgLock);
    g_roots = roots;
    ReleaseSRWLockExclusive(&g_cfgLock);
    g_input = g_cfg->phoneInput;
    g_screen = g_cfg->phoneScreen;
    g_files = g_cfg->phoneFiles;
}

void Rate() {  // скорость за последнюю секунду - на карточку
    ULONGLONG now = GetTickCount64();
    if (now - g_rateAt < 1000) return;
    double sec = g_rateAt ? (now - g_rateAt) / 1000.0 : 1;
    g_rateAt = now;
    long long sent = g_sent, recv = g_recv;
    double up = (sent - g_sentWas) / sec, down = (recv - g_recvWas) / sec;
    g_sentWas = sent, g_recvWas = recv;
    long long inLeft = 0;
    {
        Lock l;
        for (const auto& [id, in] : S().incoming) inLeft += in.total - in.have;
    }
    std::wstring r = up >= down ? RateText(L"Отдаю", up, g_sendLeft) : RateText(L"Принимаю", down, inLeft);
    if (r != g_rate) {
        g_rate = r;
        if (Card().Visible()) Card().Update(MakeView());
    }
}

// картинка из буфера ПК -> PNG (для телефона); пусто - картинки нет
std::string ClipPng() {
    std::string out;
    if (!IsClipboardFormatAvailable(CF_BITMAP) || !OpenClipboard(g_app)) return out;
    HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (hb) {
        Gdiplus::Bitmap bmp(hb, nullptr);
        if (bmp.GetLastStatus() == Gdiplus::Ok && bmp.GetWidth() * bmp.GetHeight() <= 40000000u) {
            IStream* st = nullptr;
            CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) && bmp.Save(st, &png, nullptr) == Gdiplus::Ok) {
                HGLOBAL g = nullptr;
                GetHGlobalFromStream(st, &g);
                STATSTG ss{};
                st->Stat(&ss, STATFLAG_NONAME);
                if (void* p = GlobalLock(g)) {
                    out.assign((const char*)p, (size_t)ss.cbSize.QuadPart);
                    GlobalUnlock(g);
                }
            }
            if (st) st->Release();
        }
    }
    CloseClipboard();
    return out;
}

// картинка с телефона -> в буфер ПК (как рисунок и как PNG для программ, которые его понимают)
bool PutImage(const std::string& bytes) {
    IStream* st = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    if (!st) return false;
    bool ok = false;
    {
        Gdiplus::Bitmap bmp(st);
        HBITMAP hb = nullptr;
        if (bmp.GetLastStatus() == Gdiplus::Ok &&
            bmp.GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &hb) == Gdiplus::Ok) {
            if (OpenClipboard(g_app)) {
                EmptyClipboard();
                ok = SetClipboardData(CF_BITMAP, hb) != nullptr;
                if (!ok) DeleteObject(hb);
                bool isPng = bytes.size() > 8 && bytes.compare(1, 3, "PNG") == 0;
                if (isPng) {
                    static UINT pngFmt = RegisterClipboardFormatW(L"PNG");
                    HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
                    if (m) {
                        memcpy(GlobalLock(m), bytes.data(), bytes.size());
                        GlobalUnlock(m);
                        if (!SetClipboardData(pngFmt, m)) GlobalFree(m);
                    }
                }
                CloseClipboard();
            } else {
                DeleteObject(hb);
            }
        }
    }
    st->Release();
    return ok;
}

// папка, брошенная на телефон: все файлы внутри (без скрытых и ссылок), имена - путь внутри папки
void ExpandDir(const std::wstring& dir, const std::wstring& rel, std::vector<Offered>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." ||
            (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ExpandDir(dir + L"\\" + n, rel + L"\\" + n, out);
        } else if (out.size() < 20000) {
            Offered o;
            o.path = dir + L"\\" + n;
            o.name = rel + L"\\" + n;
            o.size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            out.push_back(o);
        }
    } while (out.size() < 20000 && FindNextFileW(h, &fd));
    FindClose(h);
}

PhoneCard::View MakeView() {
    PhoneCard::View v;
    Lock l;
    Session& s = S();
    v.pageMode = !s.oneTime;
    v.connected = s.seen != 0;
    v.lan = g_ip != 0;
    v.bluetooth = g_bluetooth;
    if (g_ip) v.url = L"http://" + IpText(g_ip) + L":" + std::to_wstring(g_port) + L"/" + Widen(s.token) + L"/";
    int done = 0;
    for (const Offered& f : s.files) {
        v.bytes += f.size;
        done += f.done;
    }
    if (!s.files.empty()) {
        v.title = L"На телефон: " + Plural((int)s.files.size(), L"файл", L"файла", L"файлов");
        v.what = s.files.size() == 1 ? s.files[0].name + L", " + SizeText(v.bytes)
                                     : SizeText(v.bytes) + L", " + s.files[0].name + L" и др.";
    } else {
        v.title = L"Телефон рядом";
        v.what = L"Буфер, фото на ПК и пульт - на странице по QR";
    }
    if (!s.seen)
        v.status = v.lan ? L"Наведите камеру телефона на QR (телефон - в той же Wi-Fi сети)" : L"";
    else if (!s.files.empty() && done == (int)s.files.size())
        v.status = L"Всё скачано - ссылка закроется через минуту";
    else if (!s.files.empty())
        v.status = L"Телефон на связи, скачано " + std::to_wstring(done) + L" из " + std::to_wstring(s.files.size());
    else
        v.status = L"Телефон на связи. Карточку можно убрать - пульт работает";
    if (s.received) v.status += L"; принято " + std::to_wstring(s.received);
    if (!g_rate.empty()) v.status += L". " + g_rate;
    if (s.standing && s.files.empty()) {
        v.title = L"Телефон рядом (запомнен)";
        v.what = L"Запомненный телефон открывает страницу из закладки, без QR";
    }
    v.hint = !g_hint.empty() ? g_hint : g_netHint;
    return v;
}

void RefreshAddress(bool force) {
    ULONGLONG now = GetTickCount64();
    if (!force && now - g_addrAt < 2000) return;
    g_addrAt = now;
    std::vector<Adapter> list = g_loopback ? std::vector<Adapter>() : Adapters();
    uint32_t ip = g_loopback ? 0x7F000001 : PickAddress(list);
    if (GetEnvironmentVariableW(L"RADIAL_PHONE_NOLAN", nullptr, 0) > 0) ip = 0;  // снимок «общей сети нет»
    if (ip != g_ip || force) {
        if (ip != g_ip) g_hint.clear();  // подсказка пути сделала своё
        g_ip = ip;
        g_netHint.clear();
        std::wstring guid;
        for (const Adapter& a : list)
            if (a.ip == ip) guid = a.guid;
        std::wstring pub = ip && !g_loopback ? PublicNetwork(guid) : L"";
        if (!pub.empty())
            g_netHint = L"Сеть «" + pub +
                        L"» помечена общественной - брандмауэр не пустит телефон. Параметры - "
                        L"Сеть - свойства - «Частная».";
        else if (ip && !g_fwOk)
            g_netHint = L"Windows спросит про доступ к сети: «Разрешить» для частных сетей, иначе телефон не дойдёт.";
        if (Card().Visible()) Card().Update(MakeView());
    }
}

// сертификат https на нынешний адрес (и 127.0.0.1); корневой создаётся при первой передаче
void RefreshCert() {
    if (!g_tlsPort || g_ip == g_certIp) return;
    g_certIp = g_ip;
    std::vector<uint32_t> ips;
    if (g_ip && g_ip != 0x7F000001) ips.push_back(g_ip);
    ips.push_back(0x7F000001);
    PCCERT_CONTEXT c = cert::IssueServer(cert::DefaultDir(), ips, L"MoonDesk Phone TLS");
    if (c && TlsSetCert(c)) {
        g_certs.push_back(c);
    } else {
        if (c) CertFreeCertificateContext(c);
        autos::Note(L"Телефон рядом: сертификат https не выпущен - страница только по http");
    }
    Lock l;
    S().tlsPort = TlsReady() ? g_tlsPort : 0;
    ++S().seq;
}

bool StartSession(bool oneTime, bool standing = false) {
    if (S().on) {
        Lock l;
        if (!oneTime) S().oneTime = false;  // страницу открыли поверх передачи файлов - уже не одноразовая
        return true;
    }
    g_loopback = GetEnvironmentVariableW(L"RADIAL_PHONE_LOCAL", nullptr, 0) > 0;  // проверка без сети и брандмауэра
    int port = 0, tlsPort = 0;
    if (!ServerStart(port, tlsPort, g_loopback)) return false;
    g_port = port;
    g_tlsPort = tlsPort;
    g_certIp = ~0u;
    std::string ca;
    if (tlsPort && !cert::LoadOrCreateCa(cert::DefaultDir(), ca)) g_tlsPort = 0;
    {
        Lock l;
        Session& s = S();
        s.on = true;
        s.oneTime = oneTime;
        s.token = NewToken();
        s.files.clear();
        s.clip.clear();
        s.started = GetTickCount64();
        s.seen = s.allDone = 0;
        s.received = 0;
        s.receivedBytes = 0;
        s.standing = standing;
        s.devices = LoadDevices();
        s.clipPng.clear();
        s.clipImg = 0;
        s.caDer = ca;
        s.tlsPort = g_tlsPort;
        ++s.seq;
    }
    g_clipSeq = 0;
    g_rate.clear();
    g_bluetooth = HasBluetooth();
    g_hint.clear();
    g_fwOk = g_loopback || FirewallAllows(port);
    RefreshAddress(true);
    RefreshCert();
    MediaStart();
    autos::Note(L"Телефон рядом: передача открыта (порт " + std::to_wstring(port) +
                (g_tlsPort ? L", https " + std::to_wstring(g_tlsPort) : std::wstring()) + L")");
    return true;
}

void ShowNote(const std::wstring& title, const std::wstring& sub) {
    g_note.yesLabel.clear(), g_note.noLabel.clear();
    g_note.onYes = nullptr, g_note.onNo = nullptr;
    g_note.kind = L"-";
    g_note.Show(BoostCard::DONE, title, sub, Theme(), 6000, Toast::G_OK);
}

void ReadClipboard() {
    DWORD seq = GetClipboardSequenceNumber();
    if (seq == g_clipSeq) return;
    static UINT hidden = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    std::wstring text;
    bool skip = IsClipboardFormatAvailable(hidden) != FALSE;
    if (!skip && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (!OpenClipboard(g_app)) return;  // занят - в следующий раз
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = (const wchar_t*)GlobalLock(h)) {
                text.assign(p, wcsnlen(p, 20000));
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    g_clipSeq = seq;
    if (!secrets::Kind(text).empty()) text.clear();  // токены и пароли на телефон не уходят
    std::string png = skip ? std::string() : ClipPng();
    Lock l;
    if (text != S().clip) {
        S().clip = text;
        ++S().seq;
    }
    if (png != S().clipPng) {
        S().clipPng = std::move(png);
        S().clipImg = S().clipPng.empty() ? 0 : S().clipImg + 1;
        ++S().seq;
    }
}

}  // namespace

namespace detail {

Session& S() {
    static Session s;
    return s;
}

void Post(Event e, const std::wstring& text) {
    if (g_app) PostMessageW(g_app, WM_PHONE, e, text.empty() ? 0 : (LPARAM) new std::wstring(text));
}

void PostNotice(const Notice& n) {
    auto* p = new Notice(n);
    if (!g_app || !PostMessageW(g_app, WM_PHONE, EV_NOTICE, (LPARAM)p)) delete p;
}

void PostBytes(Event e, std::string bytes) {
    auto* p = new std::string(std::move(bytes));
    if (!g_app || !PostMessageW(g_app, WM_PHONE, e, (LPARAM)p)) delete p;
}

std::vector<std::wstring> ShareRoots() {
    AcquireSRWLockShared(&g_cfgLock);
    std::vector<std::wstring> r = g_roots;
    ReleaseSRWLockShared(&g_cfgLock);
    return r;
}

bool InputAllowed() { return g_input; }

bool ScreenAllowed() { return g_screen && !g_screenStopped; }

bool FilesAllowed() { return g_files; }

bool CallMain(const std::string& op, std::string& out) {
    if (!g_app) return false;
    CallReq req{op, "", false};
    DWORD_PTR res = 0;
    SendMessageTimeoutW(g_app, WM_PHONE, EV_CALL, (LPARAM)&req, SMTO_BLOCK | SMTO_ABORTIFHUNG, 5000, &res);
    out = req.out;
    return req.ok;
}

std::string NewDevice(const std::wstring& name) {
    unsigned char b[16];
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    DeviceRec d;
    d.key = Token(b, sizeof(b));
    d.name = CleanLine(name, 60);
    d.added = d.seen = (long long)time(nullptr);
    AcquireSRWLockExclusive(&g_devLock);
    std::vector<DeviceRec> list = ReadDevices(nullptr);
    list.push_back(d);
    bool ok = WriteDevices(list);
    ReleaseSRWLockExclusive(&g_devLock);
    if (!ok) return "";
    Lock l;
    S().devices.push_back(d.key);
    return d.key;
}

void DeviceSeen(int index) {
    std::string key;
    {
        Lock l;
        if (index < 0 || index >= (int)S().devices.size()) return;
        key = S().devices[index];
    }
    AcquireSRWLockExclusive(&g_devLock);
    if (std::find(g_devSeen.begin(), g_devSeen.end(), key) == g_devSeen.end()) g_devSeen.push_back(key);
    ReleaseSRWLockExclusive(&g_devLock);
}

float Command(const std::string& c) {
    if (!g_app) return -1;
    Cmd cmd{c};
    DWORD_PTR res = 0;
    SendMessageTimeoutW(g_app, WM_PHONE, EV_CMD, (LPARAM)&cmd, SMTO_BLOCK | SMTO_ABORTIFHUNG, 3000, &res);
    return cmd.vol;
}

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring PhotosDir() {
    std::wstring p = Known(FOLDERID_Pictures);
    return p.empty() ? L"" : p + L"\\Телефон";
}

std::wstring DownloadsDir() { return Known(FOLDERID_Downloads); }

void CopyUrl() {
    PhoneCard::View v = MakeView();
    if (!v.url.empty()) PutText(v.url);
}

void RunWay(Way w) {
    if (w == WAY_PHONE_HOTSPOT) {
        ShellExecuteW(nullptr, L"open", L"ms-availablenetworks:", nullptr, nullptr, SW_SHOWNORMAL);
        g_hint = L"На телефоне включите точку доступа, ноутбук подключите к ней - QR появится сам.";
    } else if (w == WAY_PC_HOTSPOT) {
        ShellExecuteW(nullptr, L"open", L"ms-settings:network-mobilehotspot", nullptr, nullptr, SW_SHOWNORMAL);
        g_hint = L"Включите «Мобильный хот-спот», подключите к нему телефон - QR появится сам.";
    } else if (w == WAY_BLUETOOTH) {
        std::wstring paths;
        {
            Lock l;
            for (const Offered& f : S().files) paths += (paths.empty() ? L"\"" : L" \"") + f.path + L"\"";
        }
        PutText(paths);
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        ShellExecuteW(nullptr, L"open", (std::wstring(sys) + L"\\fsquirt.exe").c_str(), L"-send", nullptr,
                      SW_SHOWNORMAL);
        g_hint = L"В окне Bluetooth выберите телефон, потом «Обзор» - пути файлов уже в буфере (Ctrl+V).";
    }
    Card().Update(MakeView());
}

void StopSession(const wchar_t* why) {
    if (!S().on) return;
    {
        Lock l;
        S().on = false;
    }
    ServerStop();
    for (PCCERT_CONTEXT c : g_certs) CertFreeCertificateContext(c);
    g_certs.clear();
    CamStop();
    MediaStop();
    int got = 0, done = 0, total = 0;
    {
        Lock l;
        got = S().received;
        total = (int)S().files.size();
        for (const Offered& f : S().files) done += f.done;
        S().files.clear();
        S().clip.clear();
        S().token.clear();
        S().clipPng.clear();
        S().standing = false;
        for (const auto& [id, in] : S().incoming)  // недокачанное с телефона - прочь
            if (!in.tmp.empty()) DeleteFileW(in.tmp.c_str());
        S().incoming.clear();
    }
    g_screenStopped = false;
    Watching(false);
    Card().Hide();
    g_hint.clear();
    std::wstring sum =
        L"скачано " + std::to_wstring(done) + L" из " + std::to_wstring(total) + L", принято " + std::to_wstring(got);
    autos::Note(L"Телефон рядом: передача закрыта (" + std::wstring(why) + L"; " + sum + L")");
    if (total && done == total) ShowNote(L"Передано на телефон", Plural(total, L"файл", L"файла", L"файлов"));
}

}  // namespace detail

void Init(HINSTANCE inst, HWND app, const Config* cfg) {
    g_app = app;
    g_cfg = cfg;
    Card().Create(inst);
    g_note.Create(inst);
    g_watchCard.Create(inst);
    g_alert.Create(inst);
}

void Offer(const std::vector<std::wstring>& files) {
    std::vector<Offered> add;
    for (const std::wstring& p : files) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) continue;
        if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {  // папка целиком: файлы внутри, имена с подпапками
            ExpandDir(p, p.substr(p.find_last_of(L'\\') + 1), add);
            continue;
        }
        Offered o;
        o.path = p;
        o.name = p.substr(p.find_last_of(L'\\') + 1);
        o.size = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        add.push_back(o);
    }
    if (add.empty()) {
        ShowNote(L"На телефон нечего передать", L"Папки пустые или файлы скрыты");
        return;
    }
    if (!StartSession(true)) {
        ShowNote(L"Не открыть передачу", L"Порты 8780-8799 заняты");
        return;
    }
    {
        Lock l;
        for (Offered& o : add) {
            bool dup = false;
            for (const Offered& f : S().files) dup = dup || f.path == o.path;
            if (!dup) S().files.push_back(o);
        }
        S().allDone = 0;
        ++S().seq;
    }
    Card().Show(MakeView(), Theme());
}

void OpenPage() {
    if (!StartSession(false)) {
        ShowNote(L"Не открыть страницу телефона", L"Порты 8780-8799 заняты");
        return;
    }
    Card().Show(MakeView(), Theme());
}

void Stop() { StopSession(L"остановлено"); }

bool Active() { return S().on; }

int Devices() { return (int)DeviceRecs().size(); }

std::vector<KnownPhone> KnownPhones() {
    std::vector<KnownPhone> out;
    for (const DeviceRec& d : DeviceRecs()) out.push_back({d.key, d.name, d.added, d.seen});
    return out;
}

void ForgetPhone(const std::string& key) {
    std::wstring name;
    bool left = false;
    AcquireSRWLockExclusive(&g_devLock);
    std::vector<DeviceRec> list = ReadDevices(nullptr);
    for (auto it = list.begin(); it != list.end();)
        if (it->key == key) {
            name = it->name;
            it = list.erase(it);
        } else {
            ++it;
        }
    WriteDevices(list);
    left = !list.empty();
    g_devSeen.erase(std::remove(g_devSeen.begin(), g_devSeen.end(), key), g_devSeen.end());
    ReleaseSRWLockExclusive(&g_devLock);
    {
        Lock l;
        S().devices.erase(std::remove(S().devices.begin(), S().devices.end(), key), S().devices.end());
    }
    autos::Note(L"Телефон рядом: забыт «" + (name.empty() ? std::wstring(L"телефон") : name) + L"»");
    if (!left && S().on && S().standing) StopSession(L"телефоны забыты");
}

void Find() {
    if (!S().on) {
        ShowNote(L"Телефон не на связи", L"Найти можно, пока на телефоне открыта страница «Компьютер рядом»");
        return;
    }
    {
        Lock l;
        ++S().findSeq;
        ++S().seq;
    }
    autos::Note(L"Телефон рядом: найти телефон");
    ShowNote(L"Телефон звенит", L"Если страница на нём открыта");
}

void SendFiles(const std::vector<std::wstring>& files) { Offer(files); }

void SendClipboard() {
    std::vector<std::wstring> files;
    std::wstring text;
    if (OpenClipboard(g_app)) {
        if (HANDLE d = GetClipboardData(CF_HDROP)) {  // скопированное в Проводнике
            UINT n = DragQueryFileW((HDROP)d, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t p[MAX_PATH * 4];
                if (DragQueryFileW((HDROP)d, i, p, MAX_PATH * 4)) files.push_back(p);
            }
        } else if (HANDLE t = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* w = (const wchar_t*)GlobalLock(t)) {
                text = Trim(std::wstring(w).substr(0, 4096));
                GlobalUnlock(t);
            }
        }
        CloseClipboard();
    }
    if (!files.empty()) return SendFiles(files);
    std::string link = ShareLink("", Narrow(text));
    if (link.empty()) {
        ShowNote(L"На телефон нечего отправить", text.empty() ? L"Скопируйте файлы в Проводнике или ссылку"
                                                              : L"Текст и так виден на телефоне во вкладке «Буфер»");
        return;
    }
    if (!S().on) OpenPage();
    if (!S().on) return;
    {
        Lock l;
        S().links.insert(S().links.begin(), Widen(link));
        if (S().links.size() > 10) S().links.pop_back();
        ++S().seq;
    }
    autos::Note(L"Телефон рядом: ссылка на телефон " + Widen(link));
    ShowNote(L"Ссылка - на телефон", Widen(link));
}

void SetRunner(void (*run)(const std::wstring&)) { g_runner = run; }

void EnsureSendTo() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    std::wstring lnk = dir + L"\\Телефон (RadialMenu).lnk";
    if (dir.empty() || GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(exe);
    sl->SetArguments(L"--to-phone");
    sl->SetIconLocation(exe, 0);
    sl->SetDescription(L"Передать на телефон (RadialMenu, «Телефон рядом»)");
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
}

bool PostToRunning(HWND app, const std::vector<std::wstring>& files) {
    std::string u;
    for (const std::wstring& f : files) u += Narrow(f) + "\n";
    std::wstring path = OutboxFile();
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, u.data(), (DWORD)u.size(), &put, nullptr) && put == u.size();
    CloseHandle(h);
    return ok && PostMessageW(app, WM_PHONE, EV_OUTBOX, 0);
}

void Forget() {
    AcquireSRWLockExclusive(&g_devLock);
    WriteDevices({});
    g_devSeen.clear();
    ReleaseSRWLockExclusive(&g_devLock);
    {
        Lock l;
        S().devices.clear();
    }
    autos::Note(L"Телефон рядом: запомненные телефоны забыты");
    if (S().on && S().standing) StopSession(L"телефоны забыты");
}

std::wstring Url() { return S().on ? MakeView().url : L""; }

std::wstring SecureUrl() {
    if (!S().on || !g_ip) return L"";
    Lock l;
    if (!S().tlsPort) return L"";
    return L"https://" + IpText(g_ip) + L":" + std::to_wstring(S().tlsPort) + L"/" + Widen(S().token) + L"/";
}

std::vector<Adapter> LocalAdapters() { return Adapters(); }

std::wstring Diag() {
    std::wstring out;
    std::vector<Adapter> list = Adapters();
    for (const Adapter& a : list)
        out += a.name + L"  " + IpText(a.ip) + L"  тип " + std::to_wstring(a.type) + (a.virt ? L"  виртуальный" : L"") +
               L"\n";
    uint32_t ip = PickAddress(list);
    std::wstring guid;
    for (const Adapter& a : list)
        if (a.ip == ip) guid = a.guid;
    out += L"для телефона: " + (ip ? IpText(ip) : L"нет общей сети") + L"\n";
    std::wstring pub = PublicNetwork(guid);
    out += L"профиль сети: " + (pub.empty() ? L"не общественная" : L"общественная («" + pub + L"»)") + L"\n";
    out += L"Bluetooth: " + std::wstring(HasBluetooth() ? L"есть" : L"нет") + L"\n";
    out += L"брандмауэр пускает на 8780: " + std::wstring(FirewallAllows(8780) ? L"да" : L"нет") + L"\n";
    std::wstring ca = cert::DefaultDir() + L"\\phone-ca.cer";
    out += L"корневой сертификат https: " +
           std::wstring(GetFileAttributesW(ca.c_str()) != INVALID_FILE_ATTRIBUTES ? ca : L"ещё не создан") + L"\n";
    // настоящая веб-камера Windows: функция есть с Windows 11, но ей нужен свой COM-источник кадров (см. README)
    HMODULE sg = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    bool vcam = sg && GetProcAddress(sg, "MFCreateVirtualCamera");
    if (sg) FreeLibrary(sg);
    out += L"MFCreateVirtualCamera: " + std::wstring(vcam ? L"есть" : L"нет") + L"\n";
    return out;
}

std::wstring Status() {
    if (!S().on) return L"";
    Lock l;
    return S().seen ? L"Телефон на связи" : L"Телефон: ждём по QR";
}

void Tick() {
    Card().Tick();
    g_note.Tick();
    g_watchCard.Tick();
    g_alert.Tick();
    RefreshCfg();
    FlushSeen(false);
    ULONGLONG now = GetTickCount64();
    // запомненный телефон: сервер ждёт его всё время, пока ноутбук в своей (частной) сети
    if (g_cfg && g_cfg->phoneAlways && now - g_standAt > 30000) {
        g_standAt = now;
        std::vector<Adapter> list = Adapters();
        uint32_t ip = PickAddress(list);
        std::wstring guid;
        for (const Adapter& a : list)
            if (a.ip == ip) guid = a.guid;
        bool home = ip && PublicNetwork(guid).empty() && !LoadDevices().empty();
        bool standing;
        {
            Lock l;
            standing = S().on && S().standing;
        }
        if (home && !S().on) StartSession(false, true);
        if (!home && standing) StopSession(L"не своя сеть");
    }
    if (!S().on) return;
    bool standing;
    {
        Lock l;
        standing = S().standing;
    }
    if (Card().stopAsked.exchange(false) && !standing) return StopSession(L"карточку закрыли");
    ReadClipboard();
    Rate();
    ULONGLONG shot = g_screenAt, tick = GetTickCount64();  // кадр могли взять уже после `now` - сравниваем со свежим
    Watching(g_screen && !g_screenStopped && shot && tick >= shot && tick - shot < 4000);
    if (Card().Visible()) RefreshAddress(false);
    RefreshCert();  // адрес поменялся - новый сертификат
    bool oneTime, allDone;
    ULONGLONG last, seen;
    {
        Lock l;
        oneTime = S().oneTime;
        allDone = S().allDone && now - S().allDone > 60000;
        seen = S().seen;
        last = std::max(S().started, S().seen);
    }
    if (oneTime && allDone) return StopSession(L"всё скачано");
    ULONGLONG idle = !seen ? 10 * 60000ull : oneTime ? 15 * 60000ull : 30 * 60000ull;
    if (!standing && now - last > idle)
        return StopSession(seen ? L"телефон давно не заходил" : L"никто не подключился");
    if (g_batch && now - g_batchAt > 4000) {
        std::wstring last1 = g_batchLast, dir = ParentDir(last1);
        int n = g_batch;
        g_batch = 0;
        g_note.yesLabel = L"Показать";
        g_note.noLabel = L"Закрыть";
        g_note.onYes = [last1] {
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + last1 + L"\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
        };
        g_note.onNo = nullptr;
        g_note.kind = L"-";
        g_note.Show(BoostCard::OFFER, L"С телефона: " + Plural(n, L"файл", L"файла", L"файлов"), L"В папке " + dir,
                    Theme(), 12000, Toast::G_OK);
    }
}

namespace {

LRESULT CALLBACK FrameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(RGB(220, 50, 60));
        FillRect((HDC)wp, &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// рамка по краям основного монитора: видно, что экран смотрят с телефона
void WatchFrame(bool on) {
    if (on == (g_watchFrame[0] != nullptr)) return;
    if (!on) {
        for (HWND& w : g_watchFrame) {
            if (w) DestroyWindow(w);
            w = nullptr;
        }
        return;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = FrameProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MoonRadialPhoneWatch";
        RegisterClassExW(&wc);
        reg = true;
    }
    RECT m{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    int t = 4;
    RECT f[4] = {{m.left, m.top, m.right, m.top + t},
                 {m.left, m.bottom - t, m.right, m.bottom},
                 {m.left, m.top, m.left + t, m.bottom},
                 {m.right - t, m.top, m.right, m.bottom}};
    for (int i = 0; i < 4; ++i) {
        g_watchFrame[i] =
            CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                            L"MoonRadialPhoneWatch", L"", WS_POPUP, f[i].left, f[i].top, f[i].right - f[i].left,
                            f[i].bottom - f[i].top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(g_watchFrame[i], SW_SHOWNOACTIVATE);
    }
}

void Watching(bool on) {  // телефон смотрит экран: рамка и карточка со «Стоп», в журнал - начало и конец
    if (on == g_watching) return;
    g_watching = on;
    WatchFrame(on);
    autos::Note(on ? L"Телефон рядом: телефон смотрит экран ПК" : L"Телефон рядом: экран больше не смотрят");
    if (on) {
        g_watchCard.kind = L"-";
        g_watchCard.yesLabel = L"Стоп";
        g_watchCard.noLabel = L"Скрыть";
        g_watchCard.onYes = [] {
            g_screenStopped = true;  // до конца этой передачи
            Watching(false);
            autos::Note(L"Телефон рядом: показ экрана остановлен на ПК");
        };
        g_watchCard.onNo = nullptr;
        g_watchCard.Show(BoostCard::OFFER, L"Телефон смотрит экран",
                         L"Красная рамка - пока смотрит. Стоп - выключить "
                         L"до конца этой передачи.",
                         Theme(), 0, Toast::G_WARN);
    } else {
        g_watchCard.Hide();
    }
}

std::string WinsJson() {
    std::string j = "[";
    bool first = true;
    auto add = [&](const Item& it, const std::wstring& app) {
        if (!it.win || !IsWindow(it.win)) return;
        std::wstring t = winlist::Title(it.win);
        j += std::string(first ? "" : ",") + "{\"h\":\"" + std::to_string((unsigned long long)(UINT_PTR)it.win) +
             "\",\"t\":" + Json(t.empty() ? it.name : t) + ",\"a\":" + Json(app) +
             ",\"min\":" + (IsIconic(it.win) ? "true" : "false") + "}";
        first = false;
    };
    for (const Item& root : winlist::Build(40)) {
        if (root.children.empty())
            add(root, root.name);
        else
            for (const Item& c : root.children) add(c, root.name);
    }
    return j + "]";
}

bool IsAction(const Item& it) { return it.path.rfind(L"action:", 0) == 0 || macro::Is(it.path); }

// пункт кольца по номеру: «r.3.1» - правое кольцо, пункт 3, в нём пункт 1 (l - левое)
const Item* RingItem(const std::string& id) {
    if (!g_cfg || id.size() < 3 || (id[0] != 'r' && id[0] != 'l') || id[1] != '.') return nullptr;
    const std::vector<Item>* list = id[0] == 'r' ? &g_cfg->right : &g_cfg->left;
    const Item* it = nullptr;
    for (size_t at = 2; at <= id.size();) {
        size_t e = id.find('.', at);
        if (e == std::string::npos) e = id.size();
        std::string num = id.substr(at, e - at);
        if (num.empty() || num.size() > 4 || num.find_first_not_of("0123456789") != std::string::npos) return nullptr;
        size_t n = (size_t)atoi(num.c_str());
        if (n >= list->size()) return nullptr;
        it = &(*list)[n];
        list = &it->children;
        at = e + 1;
    }
    return it;
}

// пункты для телефона: программы, папки, файлы, действия и группы (подкольца из radial.cfg); служебные - мимо
std::string RingJson(const std::vector<Item>& list, const std::string& prefix, int depth) {
    std::string j = "[";
    bool first = true;
    for (size_t i = 0; i < list.size(); ++i) {
        const Item& it = list[i];
        if (it.special || it.name.empty()) continue;
        std::string id = prefix + "." + std::to_string(i);
        bool act = IsAction(it), grp = !it.children.empty() && !it.dir && depth < 2;
        if (!act && !grp && it.full.empty()) continue;
        bool icon = !it.iconFull.empty() || (!act && !it.full.empty());
        j += std::string(first ? "" : ",") + "{\"id\":\"" + id + "\",\"n\":" + phone::Json(it.name) + ",\"k\":\"" +
             (grp      ? "grp"
              : act    ? "act"
              : it.dir ? "dir"
                       : "app") +
             "\",\"i\":" + (icon ? "true" : "false");
        if (grp) j += ",\"c\":" + RingJson(it.children, id, depth + 1);
        j += "}";
        first = false;
    }
    return j + "]";
}

void DoCall(CallReq& c) {
    if (c.op == "wins") {
        c.out = WinsJson();
        c.ok = true;
    } else if (c.op.rfind("win:", 0) == 0) {
        size_t a = c.op.find(':', 4);
        std::string what = c.op.substr(4, a - 4);
        HWND w = (HWND)(UINT_PTR)strtoull(c.op.c_str() + a + 1, nullptr, 10);
        // только видимые окна верхнего уровня из списка Alt+Tab - не служебные и не чужие внутренние
        if (!IsWindow(w) || !IsWindowVisible(w) || GetAncestor(w, GA_ROOT) != w) return;
        if (what == "front") {
            INPUT in[2]{};  // фоновая программа выводит окно вперёд только после «нажатия» клавиши
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
            winlist::Activate(w);
        } else if (what == "min") {
            ShowWindowAsync(w, SW_MINIMIZE);
        } else if (what == "close") {
            winlist::Close(w);
        }
        autos::Note(L"Телефон рядом: окно «" + winlist::Title(w) + L"» - " + Widen(what));
        c.ok = true;
    } else if (c.op.rfind("open:", 0) == 0) {
        std::wstring p = Widen(c.op.substr(5));
        c.ok = (INT_PTR)ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, ParentDir(p).c_str(), SW_SHOWNORMAL) > 32;
    } else if (c.op == "ring") {
        c.out = "{\"r\":" + RingJson(g_cfg ? g_cfg->right : std::vector<Item>(), "r", 0) +
                ",\"l\":" + RingJson(g_cfg ? g_cfg->left : std::vector<Item>(), "l", 0) + "}";
        c.ok = true;
    } else if (c.op.rfind("ringicon:", 0) == 0) {
        const Item* it = RingItem(c.op.substr(9));
        if (!it) return;
        // свой значок пункта, если файл на месте, иначе - значок самой программы или папки
        bool own = !it->iconFull.empty() && GetFileAttributesW(it->iconFull.c_str()) != INVALID_FILE_ATTRIBUTES;
        c.out = Narrow(own ? it->iconFull : IsAction(*it) ? std::wstring() : it->full);
        c.ok = true;
    } else if (c.op.rfind("ringrun:", 0) == 0) {  // тот же путь, что щелчок в кольце
        const Item* it = RingItem(c.op.substr(8));
        if (!it || it->special || (!it->children.empty() && !it->dir)) return;
        if (IsAction(*it)) {
            if (!g_runner) return;
            g_runner(macro::Is(it->path) ? it->path : it->path.substr(7));
        } else if (!it->full.empty()) {
            AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, nullptr, it->full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            return;
        }
        autos::Note(L"Телефон рядом: из кольца - " + it->name);
        c.ok = true;
    }
}

// уведомление с телефона: в журнал - только от кого (сам текст остаётся в карточке)
void ShowNotice(const Notice& n) {
    if (n.kind == "end") {
        if (g_callShown && g_alert.Visible()) g_alert.Hide();
        g_callShown = false;
        return;
    }
    g_alert.yesLabel.clear(), g_alert.noLabel.clear();
    g_alert.onYes = nullptr, g_alert.onNo = nullptr;
    g_alert.kind = L"-";
    g_callShown = n.kind == "call";
    std::wstring who = n.title.empty() ? n.number : n.title;
    if (n.kind == "call" || n.kind == "missed") {
        std::wstring sub = who == n.number ? n.app : n.number;
        autos::Note(L"Телефон рядом: " + std::wstring(n.kind == "call" ? L"звонок " : L"пропущенный ") + who);
        if (n.kind == "call")
            g_alert.Show(BoostCard::DONE, L"Звонит " + who, sub, Theme(), 45000, Toast::G_INFO);
        else
            g_alert.Show(BoostCard::DONE, L"Пропущенный звонок: " + who, sub, Theme(), 30000, Toast::G_WARN);
        return;
    }
    std::wstring title = n.kind == "sms"                       ? L"SMS: " + who
                         : n.title.empty() || n.title == n.app ? n.app
                         : n.app.empty()                       ? n.title
                                                               : n.app + L": " + n.title;
    autos::Note(L"Телефон рядом: " + std::wstring(n.kind == "sms" ? L"SMS от " + who : L"уведомление " + n.app));
    std::wstring code = SmsCode(n.title + L" " + n.text);
    if (!code.empty()) {  // код подтверждения - одной кнопкой в буфер
        g_alert.yesLabel = L"Скопировать " + code;
        g_alert.noLabel = L"Закрыть";
        g_alert.onYes = [code] {
            PutText(code);
            g_clipSeq = GetClipboardSequenceNumber();
        };
        g_alert.Show(BoostCard::OFFER, title, n.text, Theme(), 30000, Toast::G_INFO);
        return;
    }
    g_alert.Show(BoostCard::DONE, title, n.text, Theme(), n.kind == "sms" ? 15000 : 8000, Toast::G_INFO);
}

}  // namespace

void OnMessage(WPARAM wp, LPARAM lp) {
    if (wp == EV_NOTICE) {
        std::unique_ptr<Notice> n((Notice*)lp);
        if (n && S().on) ShowNotice(*n);
        return;
    }
    if (wp == EV_CAM) {  // кадр камеры телефона - в окно
        CamShow();
        return;
    }
    if (wp == EV_OUTBOX) {  // «Отправить - На телефон»: файлы от второй копии
        std::wstring f = OutboxFile();
        HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        std::string u;
        if (h != INVALID_HANDLE_VALUE) {
            u.resize(std::min<DWORD>(GetFileSize(h, nullptr), 1 << 20));
            DWORD got = 0;
            ReadFile(h, u.data(), (DWORD)u.size(), &got, nullptr);
            u.resize(got);
            CloseHandle(h);
            DeleteFileW(f.c_str());
        }
        std::vector<std::wstring> files;
        for (const std::wstring& line : Split(Widen(u), L'\n'))
            if (!Trim(line).empty()) files.push_back(Trim(line));
        if (!files.empty()) SendFiles(files);
        return;
    }
    if (wp == EV_CALL) {  // из потока сервера через SendMessage: lp - CallReq на его стеке
        DoCall(*(CallReq*)lp);
        return;
    }
    if (wp == EV_CMD) {  // из потока сервера через SendMessage: lp - Cmd на его стеке
        Cmd& c = *(Cmd*)lp;
        autos::Note(L"Телефон рядом: пульт - " + Widen(c.c));
        if (c.c == "vol+" || c.c == "vol-")
            c.vol = vol::AddMaster(c.c == "vol+" ? 0.06f : -0.06f);
        else if (c.c == "mute")
            corners::Run(L"mute");
        else if (c.c == "play")
            corners::Run(L"play");
        else if (c.c == "next")
            corners::Run(L"next");
        else if (c.c == "prev") {
            INPUT in[2]{};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = VK_MEDIA_PREV_TRACK;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
        } else if (c.c == "screen")
            corners::Run(L"screen_off");
        else if (c.c == "lock")
            corners::Run(L"lock");
        else if (c.c == "sleep") {
            bg::Run([] {
                Sleep(1500);  // ответ телефону должен успеть уйти
                using Suspend = BOOLEAN(WINAPI*)(BOOLEAN, BOOLEAN, BOOLEAN);
                if (HMODULE pp = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                    if (auto f = (Suspend)(void*)GetProcAddress(pp, "SetSuspendState")) f(FALSE, FALSE, FALSE);
            });
        }
        return;
    }
    if (wp == EV_CLIPIMG) {  // картинка с телефона: lp - std::string с байтами
        std::unique_ptr<std::string> bytes((std::string*)lp);
        if (bytes && PutImage(*bytes)) {
            g_clipSeq = GetClipboardSequenceNumber();
            ShowNote(L"Картинка с телефона - в буфере", L"Ctrl+V - вставить");
        } else {
            ShowNote(L"Картинка не прочиталась", L"Формат, который Windows не знает (HEIC?)");
        }
        return;
    }
    std::unique_ptr<std::wstring> text((std::wstring*)lp);
    if (wp == EV_OCR) {  // распознали скан
        std::wstring scan = g_lastScan;
        g_note.kind = L"-";
        g_note.yesLabel = L"Показать скан";
        g_note.noLabel = L"Закрыть";
        g_note.onYes = [scan] {
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + scan + L"\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
        };
        g_note.onNo = nullptr;
        if (text && !text->empty()) {
            PutText(*text);
            g_clipSeq = GetClipboardSequenceNumber();
            std::wstring t = *text;
            if (t.size() > 120) t = t.substr(0, 119) + L"…";
            g_note.Show(BoostCard::OFFER, L"Текст скана - в буфере", t, Theme(), 15000, Toast::G_OK);
        } else {
            g_note.Show(BoostCard::OFFER, L"Скан сохранён, текста не нашлось", ParentDir(scan), Theme(), 12000,
                        Toast::G_INFO);
        }
        return;
    }
    if (!S().on) return;
    if (wp == EV_NOTE && text) {
        autos::Note(L"Телефон рядом: " + *text);
    } else if (wp == EV_OPEN && text) {  // ссылка с телефона
        ShellExecuteW(nullptr, L"open", text->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        autos::Note(L"Телефон рядом: открыта ссылка " + *text);
        ShowNote(L"Открыто с телефона", *text);
    } else if (wp == EV_SCAN && text) {  // фото документа - распознать текст в фоне
        g_lastScan = *text;
        autos::Note(L"Телефон рядом: скан " + *text);
        std::wstring path = *text;
        HWND app = g_app;
        bg::Run([path, app] {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            std::vector<uint8_t> bytes;
            if (f != INVALID_HANDLE_VALUE) {
                bytes.resize(std::min<DWORD>(GetFileSize(f, nullptr), 60u << 20));
                DWORD got = 0;
                ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr);
                bytes.resize(got);
                CloseHandle(f);
            }
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::wstring t = bytes.empty() ? L"" : ocr::Recognize(bytes);
            CoUninitialize();
            PostMessageW(app, WM_PHONE, EV_OCR, t.empty() ? 0 : (LPARAM) new std::wstring(t));
        });
    } else if (wp == EV_PAIRED) {
        autos::Note(L"Телефон рядом: запомнен телефон " + (text ? *text : std::wstring()));
        ShowNote(L"Телефон запомнен", g_cfg && g_cfg->phoneAlways
                                          ? L"Страница из закладки откроется, пока ноутбук в своей сети"
                                          : L"Чтобы открывать без QR, включите «Ждать запомненный телефон» в студии");
    }
    if (wp == EV_CLIP && text) {
        PutText(*text);
        g_clipSeq = GetClipboardSequenceNumber();
        {
            Lock l;
            S().clip = *text;
            ++S().seq;
        }
        std::wstring t = *text;
        if (t.size() > 80) t = t.substr(0, 79) + L"…";
        ShowNote(L"С телефона - в буфере", t);
    } else if (wp == EV_RECEIVED && text) {
        ++g_batch;
        g_batchLast = *text;
        g_batchAt = GetTickCount64();
        autos::Note(L"Телефон рядом: принят " + *text);
    } else if (wp == EV_FILE_DONE && text) {
        autos::Note(L"Телефон рядом: скачан " + *text);
    }
    if (Card().Visible()) Card().Update(MakeView());
}

void Shutdown() {
    StopSession(L"выход");
    FlushSeen(true);
    Card().Destroy();
    g_note.Destroy();
    g_alert.Destroy();
}

}  // namespace phone
