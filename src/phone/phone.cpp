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

#include <algorithm>
#include <memory>

#include "app/autos.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/secrets.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "desk/corners.h"
#include "desk/volume.h"
#include "phone/phone_internal.h"

namespace phone {

using namespace detail;

namespace {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
BoostCard g_note;  // итог: «Передано на телефон», «С телефона: 5 фото - показать»
int g_port = 0;
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

bool StartSession(bool oneTime) {
    if (S().on) {
        Lock l;
        if (!oneTime) S().oneTime = false;  // страницу открыли поверх передачи файлов - уже не одноразовая
        return true;
    }
    g_loopback = GetEnvironmentVariableW(L"RADIAL_PHONE_LOCAL", nullptr, 0) > 0;  // проверка без сети и брандмауэра
    int port = 0;
    if (!ServerStart(port, g_loopback)) return false;
    g_port = port;
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
        s.bad = s.received = 0;
        s.receivedBytes = 0;
        ++s.seq;
    }
    g_clipSeq = 0;
    g_bluetooth = HasBluetooth();
    g_hint.clear();
    g_fwOk = g_loopback || FirewallAllows(port);
    RefreshAddress(true);
    autos::Note(L"Телефон рядом: передача открыта (порт " + std::to_wstring(port) + L")");
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
    Lock l;
    if (text != S().clip) {
        S().clip = text;
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
    int got = 0, done = 0, total = 0;
    {
        Lock l;
        got = S().received;
        total = (int)S().files.size();
        for (const Offered& f : S().files) done += f.done;
        S().files.clear();
        S().clip.clear();
        S().token.clear();
    }
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
}

void Offer(const std::vector<std::wstring>& files) {
    std::vector<Offered> add;
    int dirs = 0;
    for (const std::wstring& p : files) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) continue;
        if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++dirs;
            continue;
        }
        Offered o;
        o.path = p;
        o.name = p.substr(p.find_last_of(L'\\') + 1);
        o.size = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        add.push_back(o);
    }
    if (add.empty()) {
        ShowNote(L"На телефон - только файлы", dirs ? L"Папку целиком не передать: бросьте файлы из неё" : L"");
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

std::wstring Url() { return S().on ? MakeView().url : L""; }

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
    if (!S().on) return;
    ULONGLONG now = GetTickCount64();
    if (Card().stopAsked.exchange(false)) return StopSession(L"карточку закрыли");
    ReadClipboard();
    if (Card().Visible()) RefreshAddress(false);
    bool oneTime, allDone, tooBad;
    ULONGLONG last, seen;
    {
        Lock l;
        oneTime = S().oneTime;
        allDone = S().allDone && now - S().allDone > 60000;
        tooBad = S().bad >= 20;
        seen = S().seen;
        last = std::max(S().started, S().seen);
    }
    if (oneTime && allDone) return StopSession(L"всё скачано");
    if (tooBad) return StopSession(L"чужие запросы с неверным ключом");
    ULONGLONG idle = !seen ? 10 * 60000ull : oneTime ? 15 * 60000ull : 30 * 60000ull;
    if (now - last > idle) return StopSession(seen ? L"телефон давно не заходил" : L"никто не подключился");
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

void OnMessage(WPARAM wp, LPARAM lp) {
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
    std::unique_ptr<std::wstring> text((std::wstring*)lp);
    if (!S().on) return;
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
    Card().Destroy();
    g_note.Destroy();
}

}  // namespace phone
