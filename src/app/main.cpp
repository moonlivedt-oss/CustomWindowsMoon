// Radial Menu: Alt + курсор к левому или правому краю экрана открывает кольцо папок.
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <memory>
#include <string>

#include "app/autostart.h"
#include "app/devcli.h"
#include "app/netui.h"
#include "app/autos.h"
#include "app/toolsui.h"
#include "core/mem.h"
#include "core/config.h"
#include "core/files.h"
#include "core/fx.h"
#include "core/llmouse.h"
#include "core/themes.h"
#include "core/night.h"
#include "core/perf.h"
#include "desk/battery.h"
#include "desk/boost.h"
#include "desk/boostcard.h"
#include "desk/corners.h"
#include "desk/cursor.h"
#include "desk/deskclick.h"
#include "desk/devmode.h"
#include "desk/frame.h"
#include "desk/gamemode.h"
#include "desk/hexwave.h"
#include "desk/lockscreen.h"
#include "desk/snip.h"
#include "desk/widget.h"
#include "desk/winfx.h"
#include "gfx/accent.h"
#include "gfx/sound.h"
#include "menu/menu.h"
#include "net/watch.h"
#include "studio/studio.h"

namespace {

constexpr wchar_t kClass[] = L"MoonRadialMenuApp";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kPollTimer = 1, kWarmTimer = 2, kIntroTimer = 3, kAutostartTimer = 4;
constexpr UINT kTrayId = 1;
constexpr int kHotWindows = 1;  // Alt + ` - кольцо окон
enum Cmd {
    CMD_EDITOR = 100,
    CMD_EDIT,
    CMD_FOLDER,
    CMD_RELOAD,
    CMD_AUTOSTART,
    CMD_RESET_WINDOWS,
    CMD_GAME,
    CMD_DEV,
    CMD_BOOST,
    CMD_EXIT
};
constexpr UINT WM_BOOST_DONE = WM_APP + 60;  // поток Boost закончил: lParam - boost::Result*

HINSTANCE g_inst;
HWND g_wnd;
UINT g_taskbarCreated;
RadialMenu g_menu;
corners::HotCorners g_corners;
MediaWidget g_media;
WindowFx g_winfx;
DeskClick g_deskClick;
BatteryGlow g_battery;
SnipCard g_snip;
GameMode g_game;
DevMode g_dev;
WindowFrame g_frame;
lockscreen::Reveal g_reveal;
HexWave g_wave;
BoostCard g_boostCard;
std::vector<boost::Group> g_boostGroups;  // что предложили закрыть
std::vector<std::wstring> g_boostClosed;  // что закрыли - вернуть после игры
bool g_boostBusy = false;
HPOWERNOTIFY g_displayNotify = nullptr, g_acdcNotify = nullptr;
ULONGLONG g_cfgCheckAt = 0;
ULONGLONG g_guardAt = 0;  // когда последний раз проверяли, не висят ли скрытые окна
UINT g_pollMs = 20;
Config g_cfg;
std::wstring g_cfgPath;
FILETIME g_cfgTime{};
bool g_armed = true;

FILETIME FileTime(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d);
    return d.ftLastWriteTime;
}

void ApplyAccent() {
    COLORREF c;
    g_cfg.accent = g_cfg.accentAuto && WallpaperAccent(c) ? c : g_cfg.accentManual;
}

void Reload() {
    if (g_menu.IsOpen()) return;  // меню держит указатели на старые пункты
    Config c;
    if (LoadConfig(g_cfgPath, c)) g_cfg = std::move(c);
    g_cfgTime = FileTime(g_cfgPath);
    ApplyAccent();
    night::Set(night::Due(g_cfg));
    fx::SetMotion(g_cfg.motion);
    Sound::Init(night::On() ? g_cfg.volume / 2 : g_cfg.volume);  // ночью звуки вдвое тише
    g_media.Apply(g_cfg);
    g_winfx.Apply(g_cfg);
    g_snip.Apply(g_cfg);
    g_deskClick.SetEnabled(g_cfg.deskDouble);
    cursor::Apply(g_cfg);
    g_menu.ResetIcons();
    if (g_wnd) {
        g_frame.Apply(g_cfg);
        g_reveal.Apply(g_cfg);
        UnregisterHotKey(g_wnd, kHotWindows);
        if (g_cfg.winRing) RegisterHotKey(g_wnd, kHotWindows, MOD_ALT | MOD_NOREPEAT, VK_OEM_3);
    }
    if (g_wnd) netui::Apply(g_cfg);
    if (g_wnd) SetTimer(g_wnd, kWarmTimer, 500, nullptr);  // прогреть новые значки
}

UINT MonitorDpi(HMONITOR m) {
    UINT x = 96, y = 96;
    if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y))) x = 96;
    return x;
}

bool IsDesktopForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;
    wchar_t cls[64];
    GetClassNameW(fg, cls, 64);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
}

// Полноэкранное окно (игра, видео) - меню не мешает.
bool IsFullscreenForeground(HMONITOR mon, const RECT& mr) { return corners::FullscreenOn(mon, mr); }

// Пустое нажатие, чтобы отпущенный Alt не открыл меню окна под курсором.
void MaskAlt() {
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;  // не назначенная клавиша
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void PollWork() {
    bool alt = GetAsyncKeyState(VK_MENU) & 0x8000;
    ULONGLONG nowMs = GetTickCount64();
    if (nowMs - g_guardAt >= 1000) {
        g_guardAt = nowMs;
        g_menu.Guard();
        g_reveal.Guard();
    }
    // radial.cfg мог поменяться не через студию: раз в секунду проверяем, чтобы углы и плашка подхватили правку
    if (nowMs - g_cfgCheckAt >= 1000 && !g_menu.IsOpen()) {
        g_cfgCheckAt = nowMs;
        FILETIME ft = FileTime(g_cfgPath);
        if (CompareFileTime(&g_cfgTime, &ft) != 0) Reload();
        // ночной режим включается и выключается по часам
        if (night::Due(g_cfg) != night::On()) {
            night::Set(!night::On());
            Sound::Init(night::On() ? g_cfg.volume / 2 : g_cfg.volume);
        }
    }
    g_corners.Tick(g_cfg, alt || g_menu.IsOpen());
    g_game.Tick(g_cfg);
    g_dev.Tick(g_cfg);
    g_media.Tick(g_cfg, alt || g_menu.IsOpen() || fx::Game());
    g_winfx.Tick();
    g_battery.Tick(g_cfg);
    g_snip.Tick();
    g_boostCard.Tick();
    netui::Tick();
    toolsui::Tick();
    autos::Tick();
    if (!alt) {
        g_armed = true;
        return;
    }
    if (g_menu.IsOpen()) return;
    // во время перетаскивания файла Shift и Ctrl выбирают перенос или копию, им не мешаем
    bool dragging = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
    bool other = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) ||
                 (!dragging && ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT)) & 0x8000));

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& r = mi.rcMonitor;
    // край считается только внешний: за ним не должно быть другого монитора
    bool left = pt.x <= r.left && !MonitorFromPoint({pt.x - 1, pt.y}, MONITOR_DEFAULTTONULL);
    bool right = pt.x >= r.right - 1 && !MonitorFromPoint({pt.x + 1, pt.y}, MONITOR_DEFAULTTONULL);
    if (!left && !right) {
        g_armed = true;
        return;
    }
    if (!g_armed || other) return;
    g_armed = false;

    FILETIME ft = FileTime(g_cfgPath);
    if (CompareFileTime(&g_cfgTime, &ft) != 0) Reload();
    if (g_cfg.desktopOnly && !IsDesktopForeground()) return;
    if (g_cfg.skipFullscreen && IsFullscreenForeground(mon, r)) return;

    MaskAlt();
    g_menu.Show(&g_cfg, pt, r, left, MonitorDpi(mon));
}

// Курсор у края монитора (40 px): там углы, кольца и панель «Сейчас играет» - отклик нужен быстрый.
bool NearEdge() {
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& r = mi.rcMonitor;
    const LONG m = 40;
    return pt.x < r.left + m || pt.x >= r.right - m || pt.y < r.top + m || pt.y >= r.bottom - m;
}

// Опрос раз в 20 мс стоил ~8 мс процессора в секунду всегда. В покое (Alt не нажат, курсор не у края, ничего не
// движется) хватает 100 мс; как только что-то началось - снова 20 мс.
void Poll() {
    PollWork();
    bool fast = fx::TakeActive() || (GetAsyncKeyState(VK_MENU) & 0x8000) || g_menu.IsOpen() || NearEdge();
    UINT want = fast ? 20 : 100;
    if (want != g_pollMs) {
        g_pollMs = want;
        SetTimer(g_wnd, kPollTimer, want, nullptr);
    }
}

// Кольцо открытых окон вокруг курсора (Alt + ` или горячий угол).
void ShowWindowsRing() {
    if (g_menu.IsOpen()) return;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (g_cfg.skipFullscreen && IsFullscreenForeground(mon, mi.rcMonitor)) return;
    g_menu.ShowWindows(&g_cfg, pt, mi.rcWork, MonitorDpi(mon));
}

// «Показать меню» из студии: кольцо у края того монитора, где открыта студия.
void ShowFromStudio(bool left, HWND studio) {
    if (g_menu.IsOpen()) return;
    Reload();
    HMONITOR mon = MonitorFromWindow(studio, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& r = mi.rcMonitor;
    POINT pt{left ? r.left : r.right - 1, (r.top + r.bottom) / 2};
    g_menu.Show(&g_cfg, pt, r, left, MonitorDpi(mon), true);
}

std::wstring TrayTip() {
    if (g_game.On()) return L"Radial Menu: игровой режим (" + g_game.Reason() + L")";
    if (g_dev.On()) return L"Radial Menu: режим разработки (" + g_dev.Reason() + L")";
    return L"Radial Menu: Alt + край экрана";
}

void UpdateTrayTip() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, TrayTip().c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::wstring BoostDir() { return boost::Dir(g_cfg.boostDir); }

std::wstring Size(double mb) {
    wchar_t b[32];
    if (mb >= 1024)
        swprintf(b, 32, L"%.1f ГБ", mb / 1024);
    else
        swprintf(b, 32, L"%d МБ", (int)mb);
    return b;
}

// «Discord, Telegram, Edge WebView и ещё 3»
std::wstring Names(const std::vector<std::wstring>& names) {
    std::wstring s;
    size_t shown = std::min<size_t>(names.size(), 3);
    for (size_t i = 0; i < shown; ++i) s += (i ? L", " : L"") + names[i];
    if (names.size() > shown) s += L" и ещё " + std::to_wstring(names.size() - shown);
    return s;
}

DWORD WINAPI BoostThread(void* p) {
    std::unique_ptr<std::vector<boost::Group>> groups((std::vector<boost::Group>*)p);
    std::wstring dir = BoostDir();
    auto* r = new boost::Result(boost::Free(boost::Load(dir), dir, *groups));
    PostMessageW(g_wnd, WM_BOOST_DONE, 0, (LPARAM)r);
    return 0;
}

void StartBoost() {
    if (g_boostBusy || g_boostGroups.empty()) return;
    g_boostBusy = true;
    g_boostCard.Show(BoostCard::WORKING, L"Освобождаю память...",
                     L"Программы закрываются мягко, как крестиком; кто не закрылся - через пару секунд добивается.",
                     CurrentTheme(g_cfg));
    auto* groups = new std::vector<boost::Group>(g_boostGroups);
    if (HANDLE th = CreateThread(nullptr, 0, BoostThread, groups, 0, nullptr)) {
        CloseHandle(th);
    } else {
        delete groups;
        g_boostBusy = false;
    }
}

// Карточка «освободить память»: при запуске игры или редактора (если памяти мало по minFreePercent и есть
// что закрыть) или вручную из трея, угла, студии. why - к чему готовимся: «перед игрой», «для Unreal Editor».
// Что не предлагать закрыть: сама игра и редактор.
std::vector<std::wstring> KeepApps() {
    std::vector<std::wstring> keep;
    if (g_game.On())
        for (DWORD pid : g_game.GamePids())
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t p[MAX_PATH];
                DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, p, &n)) {
                    std::wstring full(p, n);
                    keep.push_back(full.substr(full.find_last_of(L'\\') + 1));
                }
                CloseHandle(h);
            }
    if (g_dev.On()) keep.push_back(L"UnrealEditor.exe");
    return keep;
}

void OfferBoost(bool manual, const std::wstring& why = L"перед игрой") {
    if (g_boostBusy) return;
    boost::Settings s = boost::Load(BoostDir());
    ThemeInfo t = CurrentTheme(g_cfg);
    if (!s.ok) {
        // Boost не настроен: свои силы - самые тяжёлые программы и мягкое «Закрыть»
        toolsui::MemoryOffer(manual ? L"" : why, KeepApps(), manual);
        return;
    }
    int used = 0;
    double avail = boost::AvailMb(&used);
    if (!manual && s.minFreePercent > 0 && 100 - used >= s.minFreePercent) return;  // памяти и так хватает
    g_boostGroups = boost::Scan(s);
    double mb = boost::TotalMb(g_boostGroups);
    if (g_boostGroups.empty() || (!manual && mb < 150)) {
        // в списках Boost закрывать нечего - может, тяжёлое что-то другое
        toolsui::MemoryOffer(manual ? L"" : why, KeepApps(), manual);
        return;
    }
    std::vector<std::wstring> names;
    for (const auto& g : g_boostGroups) names.push_back(g.title.empty() ? g.name : g.title);
    g_boostCard.onYes = [] { StartBoost(); };
    g_boostCard.Show(BoostCard::OFFER, L"Освободить ~" + Size(mb) + (manual ? L"" : L" " + why + L"?"),
                     Names(names) + L". Сейчас свободно " + Size(avail) + L".", t);
}

void BoostDone(boost::Result* r) {
    g_boostBusy = false;
    for (const auto& p : r->reopen)
        if (std::find(g_boostClosed.begin(), g_boostClosed.end(), p) == g_boostClosed.end()) g_boostClosed.push_back(p);
    g_boostCard.Show(BoostCard::DONE, L"Освобождено " + Size(r->freedMb),
                     L"Закрыто программ: " + std::to_wstring(r->closed) + L". Свободно " + Size(boost::AvailMb()) +
                         (g_cfg.boostRestore && !r->reopen.empty() ? L". После игры их можно будет вернуть." : L"."),
                     CurrentTheme(g_cfg));
    delete r;
}

// Предложить вернуть то, что Boost закрыл перед игрой или редактором.
void OfferRestore() {
    if (!g_cfg.boostRestore || g_boostClosed.empty() || g_game.On() || g_dev.On()) return;
    std::vector<std::wstring> names;
    for (const auto& p : g_boostClosed) {
        std::wstring n = p.substr(p.find_last_of(L'\\') + 1);
        names.push_back(n.size() > 4 ? n.substr(0, n.size() - 4) : n);
    }
    g_boostCard.onYes = [] {
        boost::Reopen(g_boostClosed);
        boost::ClearSession(BoostDir());
        g_boostClosed.clear();
        g_boostCard.Show(BoostCard::DONE, L"Программы открываются", L"Они запускаются свёрнутыми.",
                         CurrentTheme(g_cfg));
    };
    g_boostCard.Show(BoostCard::RESTORE, L"Вернуть закрытые программы?", Names(names), CurrentTheme(g_cfg));
}

// Игра закрылась: приоритеты и таймер назад, ещё одна очистка, предложить вернуть закрытое.
void BoostAfterGame() {
    boost::Settings s = boost::Load(BoostDir());
    boost::GameTweaks(s, false);
    if (g_cfg.boostAfter && s.purge) boost::RunPurge(s);
    OfferRestore();
}

// Режим разработки включился или выключился: обои и память, красота и перехват мыши не трогаются.
void OnDevMode(bool on) {
    UpdateTrayTip();
    autos::OnModes(g_game.On(), on);  // питание: в Unreal - производительность
    toolsui::OnModes(g_game.On(), on);
    if (on && g_cfg.devBoost && !g_game.On()) OfferBoost(false, L"для " + g_dev.Reason());
    if (!on) OfferRestore();
    if (g_cfg.hexGlass && !g_game.On())
        g_wave.Play(on, on ? L"Режим разработки: " + g_dev.Reason() : L"Режим разработки выключен",
                    CurrentTheme(g_cfg));
}

// Игровой режим включился или выключился: эффекты, рамки окон и перехват мыши.
void OnGameMode(bool on) {
    fx::SetGame(on);
    g_frame.Suspend(on);
    llmouse::Suspend(on);            // перехват мыши снят: каждое движение мыши не ходит через нас
    autos::OnModes(on, g_dev.On());  // питание: в игре - производительность
    toolsui::OnModes(on, g_dev.On());
    UpdateTrayTip();
    // Boost: во время игры таймер и приоритеты, при запуске - предложить освободить память
    netui::Game(on, g_game.GamePids(), g_game.Reason());  // сеть в игре: туннель и пинг до сервера
    if (on) {
        boost::GameTweaks(boost::Load(BoostDir()), true);
        if (g_cfg.boostOffer) OfferBoost(false);
    } else {
        BoostAfterGame();
        g_dev.Reapply(g_cfg);  // игра вернула обои, а редактор ещё открыт
    }
    // волна сот по краям экрана - видно, что режим переключился
    if (g_cfg.hexGlass)
        g_wave.Play(on, on ? L"Игровой режим: " + g_game.Reason() : L"Игровой режим выключен", CurrentTheme(g_cfg));
}

// Программу запустили вместе со входом в Windows: Проводник стартовал меньше полутора минут назад.
bool JustLoggedOn() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetShellWindow(), &pid);
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!p) return false;
    FILETIME created, a, b, c, now;
    bool ok = GetProcessTimes(p, &created, &a, &b, &c);
    CloseHandle(p);
    GetSystemTimeAsFileTime(&now);
    ULONGLONG t0 = (ULONGLONG)created.dwHighDateTime << 32 | created.dwLowDateTime,
              t1 = (ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime;
    return ok && t1 > t0 && (t1 - t0) / 10000 < 90000;
}

void AddTray() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_SHARED);  // один раз, без утечки на TaskbarCreated
    wcsncpy_s(nid.szTip, TrayTip().c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTray() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, CMD_EDITOR, L"Редактор колец");
    AppendMenuW(m, MF_STRING, CMD_EDIT, L"Открыть radial.cfg");
    AppendMenuW(m, MF_STRING, CMD_FOLDER, L"Папка программы");
    AppendMenuW(m, MF_STRING, CMD_RELOAD, L"Перечитать настройки");
    AppendMenuW(m, MF_STRING, CMD_RESET_WINDOWS, L"Вернуть окнам прозрачность и закрепление");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring game = L"Игровой режим";
    if (g_game.On() && !g_game.Manual()) game += L": " + g_game.Reason();
    AppendMenuW(m, MF_STRING | (g_game.On() ? MF_CHECKED : 0), CMD_GAME, game.c_str());
    std::wstring dev = L"Режим разработки";
    if (g_dev.On() && !g_dev.Manual()) dev += L": " + g_dev.Reason();
    AppendMenuW(m, MF_STRING | (g_dev.On() ? MF_CHECKED : 0), CMD_DEV, dev.c_str());
    AppendMenuW(m, MF_STRING, CMD_BOOST, L"Освободить память...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (AutostartOn() ? MF_CHECKED : 0), CMD_AUTOSTART, L"Запускать вместе с Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Выход");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

LRESULT CALLBACK AppProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kPollTimer) Poll();
            if (wp == kAutostartTimer) {
                KillTimer(h, kAutostartTimer);
                AutostartRefresh();
            }
            if (wp == kIntroTimer) {
                KillTimer(h, kIntroTimer);
                g_reveal.Intro();
            }
            if (wp == kWarmTimer) {
                // значки грузятся порциями по 8 мс раз в 30 мс: программа не замирает
                POINT o{0, 0};
                if (g_menu.IsOpen()) return 0;
                if (g_menu.Prewarm(&g_cfg, MonitorDpi(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY)))) {
                    KillTimer(h, kWarmTimer);
                    mem::TrimSoon(500);
                } else {
                    SetTimer(h, kWarmTimer, 30, nullptr);
                }
            }
            return 0;
        case WM_HOTKEY:
            if (wp == kHotWindows) ShowWindowsRing();
            return 0;
        case WM_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP) TrayMenu();
            if (LOWORD(lp) == WM_LBUTTONDBLCLK) PostMessageW(h, WM_COMMAND, CMD_EDITOR, 0);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case CMD_EDITOR:
                    OpenEditor(g_inst, g_cfgPath, [] { Reload(); }, ShowFromStudio);
                    break;
                case CMD_EDIT:
                    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g_cfgPath + L"\"").c_str(), nullptr,
                                  SW_SHOWNORMAL);
                    break;
                case CMD_FOLDER:
                    ShellExecuteW(nullptr, nullptr, ExeDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case CMD_RELOAD:
                    Reload();
                    break;
                case CMD_RESET_WINDOWS:
                    g_winfx.ResetAll();
                    break;
                case CMD_BOOST:
                    OfferBoost(true);
                    break;
                case CMD_GAME:
                    g_game.SetManual(!g_game.On(), g_cfg);  // выключили посреди игры - до её выхода
                    break;
                case CMD_DEV:
                    g_dev.SetManual(!g_dev.On(), g_cfg);  // выключили при открытом редакторе - до его закрытия
                    break;
                case CMD_AUTOSTART:
                    SetAutostart(!AutostartOn());
                    break;
                case CMD_EXIT:
                    DestroyWindow(h);
                    break;
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETDESKWALLPAPER && g_cfg.accentAuto) ApplyAccent();
            return 0;
        case WM_BOOST_DONE:
            BoostDone(reinterpret_cast<boost::Result*>(lp));
            return 0;
        case WM_WTSSESSION_CHANGE:
            g_reveal.OnSession(wp);
            if (wp == WTS_SESSION_UNLOCK) netui::Wake();
            return 0;
        case netui::WM_NET_EVENT:
            netui::OnEvent(lp);
            return 0;
        case netui::WM_NET_JOB:
            netui::OnJob(lp);
            return 0;
        case toolsui::WM_TOOLS_JOB:
            toolsui::OnJob(wp, lp);
            return 0;
        case autos::WM_AUTO_JOB:
            autos::OnJob(wp, lp);
            return 0;
        case WM_APP + 54:  // для проверки: кольцо утилит у курсора
            corners::Run(L"tools");
            return 0;
        case WM_APP + 50:  // для проверки: выполнить действие угла «выключить экран»
            corners::Run(L"screen_off");
            return 0;
        case WM_APP + 51:  // для проверки: портал, как при входе после включения
            g_reveal.Intro();
            return 0;
        case WM_APP + 52:  // для проверки: кольцо сети у курсора, как из угла
            corners::Run(L"net");
            return 0;
        case WM_APP + 53: {  // для проверки: карточка обрыва, как от сторожа (причина выдуманная)
            auto* e = new net::Event;
            e->kind = net::Event::DOWN;
            e->report.fix = net::FIX_PROXY;
            e->report.fixLabel = L"Сбросить прокси";
            e->report.fixReason = L"Застрял мёртвый прокси 127.0.0.1:10809 - сброс вернёт прямое подключение.";
            netui::OnEvent((LPARAM)e);
            return 0;
        }
        case WM_APP + 55: {  // для проверки: стопка карточек - смена выхода и итог Boost
            auto* e = new net::Event;
            e->kind = net::Event::EXIT;
            e->text = L"Великобритания -> Германия (185.12.4.7)";
            netui::OnEvent((LPARAM)e);
            g_boostCard.Show(BoostCard::DONE, L"Освобождено 1.4 ГБ", L"Закрыто 3 программы, вернуть - после игры.",
                             CurrentTheme(g_cfg));
            return 0;
        }
        case WM_APP + 57:  // для проверки: студия, «Что занимает место» на C: (как клик по шкале диска)
            toolsui::Run(L"scan:C:\\");
            return 0;
        case WM_APP + 58:  // для проверки: карточки со сценой на выдуманных данных (0 - очистка, 1/2 - защита)
            toolsui::Demo((int)wp);
            return 0;
        case WM_APP + 56:  // для проверки: карточка снимка
            g_snip.Demo();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_POWERSETTINGCHANGE) {
                auto* s = reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
                if (s && IsEqualGUID(s->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) && s->DataLength >= 4)
                    g_reveal.OnDisplay(*reinterpret_cast<DWORD*>(s->Data) == 1);
                if (s && IsEqualGUID(s->PowerSetting, GUID_ACDC_POWER_SOURCE)) autos::OnPower();
            } else {
                g_reveal.OnPower(wp);
                if (wp == PBT_APMPOWERSTATUSCHANGE) autos::OnPower();  // зарядку подключили или отключили
                if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) netui::Wake();
            }
            return TRUE;
        case WM_DESTROY:
            WTSUnRegisterSessionNotification(h);
            if (g_displayNotify) UnregisterPowerSettingNotification(g_displayNotify);
            if (g_acdcNotify) UnregisterPowerSettingNotification(g_acdcNotify);
            g_wave.Destroy();
            g_boostCard.Destroy();
            netui::Shutdown();
            toolsui::Shutdown();
            autos::Shutdown();
            boost::GameTweaks(boost::Load(BoostDir()), false);
            g_frame.Destroy();
            g_dev.Shutdown();
            g_game.Shutdown();
            g_reveal.Destroy();
            g_media.Destroy();
            g_corners.Destroy();
            g_winfx.Destroy();
            g_deskClick.Destroy();
            llmouse::Shutdown();
            g_snip.Destroy();
            cursor::Restore();
            RemoveTray();
            PostQuitMessage(0);
            return 0;
    }
    if (msg == g_taskbarCreated) {
        AddTray();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    if (int code = 0; devcli::Run(code)) return code;
    bool wantEditor = devcli::HasFlag(L"--editor");
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MoonRadialMenu");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // уже запущено: попросить работающую копию открыть редактор
        if (HWND w = FindWindowW(kClass, nullptr); w && wantEditor) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(w, WM_COMMAND, CMD_EDITOR, 0);
        }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);  // OLE нужен для перетаскивания файлов на меню
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);

    g_inst = inst;
    g_game.Recover();  // если прошлый запуск упал в игровом режиме - вернуть анимации, прозрачность, обои
    g_dev.Recover();
    g_cfgPath = ExeDir() + L"\\radial.cfg";
    // первый запуск после клонирования: своих колец ещё нет - берём пример с общими папками
    if (!Exists(g_cfgPath)) CopyFileW((ExeDir() + L"\\radial.example.cfg").c_str(), g_cfgPath.c_str(), TRUE);
    g_media.Create(inst);
    g_media.onTurnOff = [] {
        g_cfg.media = false;
        SaveConfig(g_cfgPath, g_cfg);
        g_cfgTime = FileTime(g_cfgPath);
        g_media.Apply(g_cfg);
    };
    Reload();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = AppProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, kClass, L"Radial Menu", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_cfg.winRing) RegisterHotKey(g_wnd, kHotWindows, MOD_ALT | MOD_NOREPEAT, VK_OEM_3);
    corners::SetWindowsRing(ShowWindowsRing);
    mem::SetBusy([] { return g_menu.IsOpen(); });

    g_menu.Create(inst);
    g_corners.Create(inst);
    g_winfx.Create(inst);
    g_winfx.Apply(g_cfg);
    g_battery.Create(inst);
    SetEditorPreview([](int what) {
        if (what == PREVIEW_LOCK_REVEAL)
            g_reveal.Demo();
        else if (what == PREVIEW_BOOST)
            OfferBoost(true);
        else if (what == PREVIEW_GAME_WAVE)
            g_wave.Play(true, L"Игровой режим: так это выглядит", CurrentTheme(g_cfg));
        else
            g_battery.Test(what == PREVIEW_BATTERY_CHARGE);
    });
    SetEditorNetStatus([] { return netui::Status(); });
    SetEditorGameStatus([] { return g_game.On() ? L"включён (" + g_game.Reason() + L")" : std::wstring(L"выключен"); });
    SetEditorGameToggle([] { g_game.SetManual(!g_game.On(), g_cfg); });
    SetEditorDevStatus([] { return g_dev.On() ? L"включён (" + g_dev.Reason() + L")" : std::wstring(L"выключен"); });
    g_snip.Create(inst);
    g_snip.Apply(g_cfg);
    // двойной клик по пустому столу: целое кольцо вокруг курсора
    g_deskClick.onDouble = [](POINT pt) {
        if (g_menu.IsOpen()) return;
        FILETIME ft = FileTime(g_cfgPath);
        if (CompareFileTime(&g_cfgTime, &ft) != 0) Reload();
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        g_menu.ShowAround(&g_cfg, pt, mi.rcWork, MonitorDpi(mon));
    };
    g_deskClick.Create(inst);
    g_deskClick.SetEnabled(g_cfg.deskDouble);
    g_frame.skip = [](HWND w) { return g_winfx.Pinned(w); };
    g_frame.Create(inst);
    g_frame.Apply(g_cfg);
    g_reveal.Create(inst, g_wnd);
    g_reveal.Apply(g_cfg);
    WTSRegisterSessionNotification(g_wnd, NOTIFY_FOR_THIS_SESSION);
    corners::SetLock([] { g_reveal.LockWithCurtain(); });
    corners::SetScreenOff([] { return g_reveal.ScreenOff(); });
    g_displayNotify = RegisterPowerSettingNotification(g_wnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    // зарядку подключили или отключили - режим питания (автоматика)
    g_acdcNotify = RegisterPowerSettingNotification(g_wnd, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_wave.Create(inst);
    g_boostCard.Create(inst);
    corners::SetBoost([] { OfferBoost(true); });
    netui::Init(inst, g_wnd, &g_cfg, &g_boostCard, [] {
        OpenEditor(g_inst, g_cfgPath, [] { Reload(); }, ShowFromStudio);
        EditorGoNet();
    });
    corners::SetNet([] {
        if (g_menu.IsOpen()) return;
        netui::ShowRing(g_menu);
    });
    toolsui::Init(inst, g_wnd, &g_cfg, &g_boostCard, [](int page, const std::wstring& scan) {
        OpenEditor(g_inst, g_cfgPath, [] { Reload(); }, ShowFromStudio);
        EditorGoTools(page, scan);
    });
    autos::Init(
        inst, g_wnd, &g_cfg, g_cfgPath, [] { Reload(); },
        [](int, const std::wstring& focus) {
            OpenEditor(g_inst, g_cfgPath, [] { Reload(); }, ShowFromStudio);
            EditorGoState(focus);
        });
    corners::SetTools([] {
        if (g_menu.IsOpen()) return;
        toolsui::ShowRing(g_menu);
    });
    // вошли в Windows только что: стол открывается порталом
    if (g_cfg.bootPortal && !wantEditor && JustLoggedOn()) SetTimer(g_wnd, kIntroTimer, 400, nullptr);
    g_game.onChange = OnGameMode;
    g_dev.onChange = OnDevMode;
    g_dev.gameOn = [] { return g_game.On() && g_cfg.gameWallpaper; };
    AddTray();
    SetTimer(g_wnd, kPollTimer, 20, nullptr);
    SetTimer(g_wnd, kWarmTimer, 1500, nullptr);
    SetTimer(g_wnd, kAutostartTimer, 8000, nullptr);  // планировщик трогаем, когда вход уже прошёл
    if (wantEditor) PostMessageW(g_wnd, WM_COMMAND, CMD_EDITOR, 0);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (EditorPreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    perf::Flush();
    Gdiplus::GdiplusShutdown(gdipToken);
    OleUninitialize();
    ReleaseMutex(mutex);
    return 0;
}
