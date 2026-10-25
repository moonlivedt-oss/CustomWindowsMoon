// «Не спать» на время: кольцо выбора, состояние сна основного потока, значок в трее, слежка за загрузкой.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>

#include "app/autos.h"
#include "core/config.h"
#include "core/files.h"
#include "core/text.h"
#include "life/awake.h"
#include "menu/menu.h"

namespace awake {

namespace {

constexpr wchar_t kClass[] = L"MoonRadialAwake";
constexpr UINT WM_AWAKE_TRAY = WM_APP + 1;
constexpr ULONGLONG kMax = 12 * 3600 * 1000ull;

HWND g_wnd = nullptr;
HICON g_icon = nullptr;
const Config* g_cfg = nullptr;
RadialMenu* g_menu = nullptr;
Kind g_kind = OFF;
ULONGLONG g_until = 0, g_start = 0, g_checkAt = 0, g_tipAt = 0;
DoneWatch g_watch;
ULONG64 g_rxWas = 0;
ULONGLONG g_rxAt = 0;
bool g_tray = false;

Item Act(const std::wstring& name, const std::wstring& key, const std::wstring& note) {
    Item it;
    it.name = name;
    it.special = 3;
    it.path = key;
    it.note = note;
    it.iconFull = ExeDir() + L"\\icons\\tools-wait.png";
    if (!Exists(it.iconFull)) it.iconFull = ExeDir() + L"\\icons\\more.png";
    return it;
}

std::wstring Downloads() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

bool PartialInDownloads() {
    std::wstring dir = Downloads();
    if (dir.empty()) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !PartialDownload(fd.cFileName)) continue;
        ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (now - t < 2 * 60 * 10000000ull) found = true;  // брошенные недокачки не в счёт
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

ULONG64 RxBytes() {  // принято всеми настоящими адаптерами
    MIB_IF_TABLE2* t = nullptr;
    if (GetIfTable2(&t) != NO_ERROR || !t) return 0;
    ULONG64 sum = 0;
    for (ULONG i = 0; i < t->NumEntries; ++i) {
        const MIB_IF_ROW2& r = t->Table[i];
        if (r.OperStatus == IfOperStatusUp && r.InterfaceAndOperStatusFlags.HardwareInterface &&
            (r.Type == IF_TYPE_IEEE80211 || r.Type == IF_TYPE_ETHERNET_CSMACD))
            sum += r.InOctets;
    }
    FreeMibTable(t);
    return sum;
}

bool Busy(ULONGLONG now) {
    ULONG64 rx = RxBytes();
    bool net = false;
    if (g_rxAt && now > g_rxAt && rx >= g_rxWas) net = (rx - g_rxWas) * 1000 / (now - g_rxAt) > 150 * 1024;
    g_rxWas = rx, g_rxAt = now;
    return net || PartialInDownloads();
}

void Tray(bool show) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = 1;
    if (!show) {
        if (g_tray) Shell_NotifyIconW(NIM_DELETE, &nid);
        g_tray = false;
        return;
    }
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_AWAKE_TRAY;
    nid.hIcon = g_icon ? g_icon : LoadIconW(nullptr, IDI_INFORMATION);
    wcsncpy_s(nid.szTip, (Status() + L" - щелчок: изменить").c_str(), _TRUNCATE);
    Shell_NotifyIconW(g_tray ? NIM_MODIFY : NIM_ADD, &nid);
    g_tray = true;
}

void Set(Kind k, ULONGLONG ms) {
    ULONGLONG now = GetTickCount64();
    g_kind = k;
    g_start = now;
    g_until = k == TIMED ? now + ms : now + kMax;
    if (k == DOWNLOAD) {
        g_watch.Start(now);
        g_rxAt = 0;
        Busy(now);
    }
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    Tray(true);
    autos::Note(L"Не спать: " + Status());
}

void Run(const std::wstring& key) {
    if (key == L"off") return Stop();
    if (key == L"dl") return Set(DOWNLOAD, 0);
    if (key == L"forever") return Set(FOREVER, 0);
    Set(TIMED, (ULONGLONG)_wtoi(key.c_str()) * 60000ull);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_AWAKE_TRAY && (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP)) {
        ShowRing();
        return 0;
    }
    static UINT created = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == created && g_tray) {  // Проводник перезапустился - значок заново
        g_tray = false;
        Tray(true);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HICON MakeIcon() {
    std::wstring f = ExeDir() + L"\\icons\\tools-wait.png";
    Gdiplus::Bitmap src(f.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return nullptr;
    int s = GetSystemMetrics(SM_CXSMICON) * 2;
    Gdiplus::Bitmap bmp(s, s, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.DrawImage(&src, 0, 0, s, s);
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg, RadialMenu* menu) {
    g_cfg = cfg;
    g_menu = menu;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, kClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
}

std::vector<Item> RingItems() {
    std::vector<Item> ring = {Act(L"Час", L"60", L"Не засыпать час, потом - как обычно"),
                              Act(L"Два часа", L"120", L"Не засыпать два часа"),
                              Act(L"Четыре часа", L"240", L"Не засыпать четыре часа"),
                              Act(L"До конца загрузки", L"dl", L"Пока что-то качается; 5 минут тихо - сон разрешён"),
                              Act(L"Пока не выключу", L"forever", L"Не дольше 12 часов")};
    if (g_kind != OFF) ring.push_back(Act(L"Выключить", L"off", L"Сон - как в настройках Windows"));
    return ring;
}

void ShowRing() {
    if (!g_menu || g_menu->IsOpen()) return;
    std::vector<Item> ring = RingItems();
    g_menu->onNet = [](const std::wstring& key) { Run(key); };
    std::wstring status = g_kind != OFF ? Status() : L"Не спать: экран гаснуть может, компьютер - не засыпает";
    g_menu->netStatus = [status] { return status; };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForSystem();
    g_menu->ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

void Tick() {
    if (g_kind == OFF) return;
    if (!g_icon) g_icon = MakeIcon();
    ULONGLONG now = GetTickCount64();
    if (now >= g_until) return Stop();
    if (g_kind == DOWNLOAD && now - g_checkAt >= 20000) {
        g_checkAt = now;
        if (g_watch.Feed(now, Busy(now))) {
            autos::Note(g_watch.SeenBusy() ? L"Не спать: загрузка закончилась" : L"Не спать: ничего не качается");
            return Stop();
        }
    }
    if (now - g_tipAt >= 30000) {  // остаток в подсказке значка
        g_tipAt = now;
        Tray(true);
    }
}

void Stop() {
    if (g_kind == OFF) return;
    g_kind = OFF;
    SetThreadExecutionState(ES_CONTINUOUS);
    Tray(false);
    autos::Note(L"Не спать: выключено, сон - как обычно");
}

bool On() { return g_kind != OFF; }

std::wstring Status() {
    switch (g_kind) {
        case TIMED: {
            ULONGLONG now = GetTickCount64();
            return L"Не сплю: " + Left(g_until > now ? g_until - now : 0);
        }
        case DOWNLOAD:
            return L"Не сплю до конца загрузки";
        case FOREVER:
            return L"Не сплю, пока не выключите";
        default:
            return L"";
    }
}

void Shutdown() {
    Stop();
    if (g_icon) DestroyIcon(g_icon);
    g_icon = nullptr;
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
}

}  // namespace awake
