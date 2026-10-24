// Radial Menu: Alt + курсор к левому или правому краю экрана открывает кольцо папок.
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include "app/app_internal.h"
#include "app/autostart.h"
#include "app/devcli.h"
#include "app/netui.h"
#include "app/afterwork.h"
#include "app/focus.h"
#include "app/palette.h"
#include "app/resume.h"
#include "app/worksets.h"
#include "desk/appvol.h"
#include "desk/cardlog.h"
#include "desk/cliphist.h"
#include "desk/layout.h"
#include "app/autos.h"
#include "app/toolsui.h"
#include "core/bg.h"
#include "core/crash.h"
#include "core/edgecheck.h"
#include "core/elevate.h"
#include "core/mem.h"
#include "core/config.h"
#include "core/files.h"
#include "core/fx.h"
#include "core/llmouse.h"
#include "core/text.h"
#include "core/themes.h"
#include "core/night.h"
#include "core/perf.h"
#include "desk/battery.h"
#include "desk/boost.h"
#include "desk/boostcard.h"
#include "desk/dropundo.h"
#include "desk/gamepad.h"
#include "desk/recent.h"
#include "desk/monitors.h"
#include "app/rules.h"
#include "desk/boostpick.h"
#include "desk/corners.h"
#include "desk/cursor.h"
#include "desk/deskclick.h"
#include "desk/devmode.h"
#include "desk/frame.h"
#include "desk/gameboost.h"
#include "desk/gamemode.h"
#include "desk/hexwave.h"
#include "desk/lockscreen.h"
#include "desk/minecraft.h"
#include "desk/modefx.h"
#include "desk/snip.h"
#include "desk/widget.h"
#include "desk/winfx.h"
#include "gfx/accent.h"
#include "gfx/sound.h"
#include "menu/menu.h"
#include "net/watch.h"
#include "studio/studio.h"

namespace app {

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
ModeFx g_modefx;  // анимация включения и выключения игрового режима и Unreal
BoostCard g_boostCard;
BoostPick g_pick;        // выбор по программам: закрыть, усыпить, оставить
BoostCard g_gbCard;      // новости Boost во время игры и работы: видеокарта, шейдеры, сборки, итог
std::wstring g_pickFor;  // чей профиль выбора: exe игры, редактора или «вручную»
ULONGLONG g_avHintAt = 0;
BoostCard g_dropCard;           // «Перенесено в ... - Вернуть»
BoostCard g_edgeCard;           // что мешает кольцам у края (CursorWrap)
BoostCard g_askCard;            // «больше не показывать?» для вида карточек, трижды закрытого без действия (Л11)
BoostCard g_mcCard, g_mcCard2;  // Minecraft: падение / AlwaysPreTouch и подкачка
McGame g_mcGame;
std::function<void()> g_afterBoost;       // запуск сборки после «Применить» в карточке выбора
ULONGLONG g_offerQuietUntil = 0;          // только что освобождали память перед запуском - при старте не спрашивать
std::wstring g_pendingAction;             // действие из кольца (action:...) - после того, как кольцо погаснет
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
bool g_restored = false;       // «Вернуть всё как было»: рамки окон на паузе до следующего перечитывания настроек
std::wstring g_themeOverride;  // тема на время правила «если - то» (пусто - своя из radial.cfg)
bool g_altTab = false;         // за это нажатие Alt был Tab: идёт переключение окон, кольцо у края не открываем

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
    if (!g_themeOverride.empty()) g_cfg.theme = g_themeOverride;  // правило ещё держится
    g_cfgTime = FileTime(g_cfgPath);
    ApplyAccent();
    night::Set(night::Due(g_cfg));
    fx::SetMotion(g_cfg.motion);
    crash::SetRestart(g_cfg.crashRestart);
    Sound::Init(night::On() ? g_cfg.volume / 2 : g_cfg.volume);  // ночью звуки вдвое тише
    g_media.Apply(g_cfg);
    g_winfx.Apply(g_cfg);
    g_snip.Apply(g_cfg);
    g_deskClick.SetEnabled(g_cfg.deskDouble);
    appvol::SetEnabled(g_cfg.appVolume);
    cursor::Apply(g_cfg);
    g_menu.ResetIcons();
    if (g_wnd) {
        g_frame.Apply(g_cfg);
        if (g_restored) {  // после «Вернуть всё как было» оформление окон возвращается с новыми настройками
            g_restored = false;
            if (!g_game.On()) g_frame.Suspend(false);
        }
        g_reveal.Apply(g_cfg);
        UnregisterHotKey(g_wnd, kHotWindows);
        if (g_cfg.winRing) RegisterHotKey(g_wnd, kHotWindows, MOD_ALT | MOD_NOREPEAT, VK_OEM_3);
    }
    if (g_wnd) netui::Apply(g_cfg);
    if (g_wnd) palette::Apply();
    if (g_wnd) gamepad::Start(g_wnd, g_cfg.gamepad);
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

// Впереди переключатель окон Alt + Tab (Windows 11 - XAML-остров, Windows 10 - MultitaskingViewFrame).
bool IsTaskSwitcher() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64];
    GetClassNameW(fg, cls, 64);
    return !wcscmp(cls, L"XamlExplorerHostIslandWindow") || !wcscmp(cls, L"MultitaskingViewFrame") ||
           !wcscmp(cls, L"TaskSwitcherWnd") || !wcscmp(cls, L"ForegroundStaging");
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

void ShowAvHint();
void UpdateTrayTip();

void PollWork() {
    PERF_SCOPE("poll");
    bool alt = GetAsyncKeyState(VK_MENU) & 0x8000;
    ULONGLONG nowMs = GetTickCount64();
    if (nowMs - g_guardAt >= 1000) {
        g_guardAt = nowMs;
        g_menu.Guard();
        g_reveal.Guard();
        static std::wstring awakeWas;  // «не сплю, пока идёт cmake» - в подсказке трея
        std::wstring awake = toolsui::AwakeReason();
        if (awake != awakeWas) {
            awakeWas = awake;
            UpdateTrayTip();
        }
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
    g_winfx.SetBlocked(g_menu.IsOpen() || fx::Game());  // Alt + мышь не мешает кольцу и игре
    g_battery.Tick(g_cfg);
    g_snip.Tick();
    g_boostCard.Tick();
    g_pick.Tick();
    g_gbCard.Tick();
    if (g_avHintAt && nowMs >= g_avHintAt) {
        g_avHintAt = 0;
        ShowAvHint();
    }
    netui::Tick();
    toolsui::Tick();
    if (!g_menu.IsOpen())
        rules::Tick(g_cfg, [](const std::wstring& theme) {
            g_themeOverride = theme;
            g_cfgTime = {};  // перечитать: Reload поставит тему правила или вернёт свою и перекрасит всё
        });
    autos::Tick();
    resume::Tick();
    afterwork::Tick();
    focus::Tick();
    if (!alt) {
        g_armed = true;
        g_altTab = false;
        return;
    }
    if (GetAsyncKeyState(VK_TAB) & 0x8000) g_altTab = true;
    if (g_menu.IsOpen()) return;
    // Alt + Tab (переключатель окон) и Alt + тяга окна к краю - это не просьба о кольце
    if (g_altTab || IsTaskSwitcher() || g_winfx.Dragging()) return;
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
    // край между мониторами курсор проскакивает - там кольцо открывает задержка: 350 мс в полосе у края
    bool inner = false;
    if (!left && !right && g_cfg.innerEdges) {
        static int side = 0;
        static ULONGLONG since = 0;
        static bool used = false;  // уже открывали отсюда: снова - только когда курсор ушёл из полосы
        LONG band = 6 * (LONG)MonitorDpi(mon) / 96;
        int now = pt.x < r.left + band && MonitorFromPoint({r.left - 1, pt.y}, MONITOR_DEFAULTTONULL)  ? 1
                  : pt.x >= r.right - band && MonitorFromPoint({r.right, pt.y}, MONITOR_DEFAULTTONULL) ? 2
                                                                                                       : 0;
        if (now != side) {
            side = now;
            since = nowMs;
            used = false;
        }
        if (side && !used && nowMs - since >= 350 && !other) {
            used = true;
            inner = true;
            left = side == 1;
            right = side == 2;
        }
    }
    if (!left && !right) {
        g_armed = true;
        return;
    }
    if (!inner && (!g_armed || other)) return;
    g_armed = false;

    FILETIME ft = FileTime(g_cfgPath);
    if (CompareFileTime(&g_cfgTime, &ft) != 0) Reload();
    if (g_cfg.desktopOnly && !IsDesktopForeground()) return;
    if (g_cfg.skipFullscreen && IsFullscreenForeground(mon, r)) return;

    MaskAlt();
    g_menu.Show(&g_cfg, pt, r, left, MonitorDpi(mon));
}

// Курсор у края монитора (40 px): там углы, кольца и панель «Сейчас играет» - отклик нужен быстрый.
bool NearEdge(POINT pt, const RECT& r) {
    const LONG m = 40;
    return pt.x < r.left + m || pt.x >= r.right - m || pt.y < r.top + m || pt.y >= r.bottom - m;
}

bool NearEdge() {
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    return NearEdge(pt, mi.rcMonitor);
}

// В потоке перехвата мыши: курсор только что дошёл до края - разбудить опрос, не дожидаясь его редкого такта.
bool OnEdgeMouse(WPARAM msg, const MSLLHOOKSTRUCT& m) {
    static RECT mon{};
    static bool known = false, was = false;
    if (msg != WM_MOUSEMOVE) return false;
    if (!known || !PtInRect(&mon, m.pt)) {  // монитор спрашиваем, только когда курсор ушёл с прежнего
        MONITORINFO mi{sizeof(mi)};
        known = GetMonitorInfoW(MonitorFromPoint(m.pt, MONITOR_DEFAULTTONEAREST), &mi);
        mon = mi.rcMonitor;
    }
    bool atEdge = known && NearEdge(m.pt, mon);
    if (atEdge && !was) PostMessageW(g_wnd, WM_POLL_WAKE, 0, 0);
    was = atEdge;
    return false;
}

// Опрос раз в 20 мс стоил ~8 мс процессора в секунду всегда. В покое (Alt не нажат, курсор не у края, ничего не
// движется) хватает 250 мс: приход курсора к краю будит опрос сразу из перехвата мыши (OnEdgeMouse), а Alt без
// края ничего не открывает. Как только что-то началось - снова 20 мс.
void Poll() {
    PollWork();
    bool fast = fx::TakeActive() || (GetAsyncKeyState(VK_MENU) & 0x8000) || g_menu.IsOpen() || NearEdge();
    UINT want = fast ? 20 : 250;
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
    std::wstring awake = toolsui::AwakeReason();
    if (!awake.empty()) return L"Radial Menu: не сплю, пока идёт " + awake;
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

// Включения и выключения режимов - в журнал автоматики: видно, когда и почему режим включался, сколько шёл
// (если режим «дёргается», это сразу видно по коротким записям)
void NoteMode(const wchar_t* name, bool on, const std::wstring& reason, ULONGLONG& since) {
    ULONGLONG now = GetTickCount64();
    if (on) {
        since = now;
        autos::Note(std::wstring(name) + L" включён: " + reason);
    } else {
        ULONGLONG s = since ? (now - since) / 1000 : 0;
        std::wstring dur = s >= 120 ? std::to_wstring(s / 60) + L" мин" : std::to_wstring(s) + L" с";
        autos::Note(std::wstring(name) + L" выключен (" + reason + L", шёл " + dur + L")");
        since = 0;
    }
}
ULONGLONG g_gameSince = 0, g_devSince = 0;

// Режим разработки включился или выключился: обои и память, красота и перехват
// мыши не трогаются.
void OnDevMode(bool on) {
    NoteMode(L"Режим разработки", on, g_dev.Reason(), g_devSince);
    UpdateTrayTip();
    autos::OnModes(g_game.On(), on);  // питание: в Unreal - производительность
    toolsui::OnModes(g_game.On(), on);
    netui::Dev(on);
    gboost::SetDev(on && g_cfg.devWatch, g_dev.Reason());
    std::vector<std::wstring> chips;
    if (on) {
        if (WallpaperPaused(g_cfg.devWallpaper)) chips.push_back(L"Обои на паузе");
        if (g_cfg.devWatch) {
            chips.push_back(L"Видеопамять");
            chips.push_back(L"Шейдеры");
            chips.push_back(L"Сборки UBT");
        }
        if (g_cfg.autoPower && !tools::OnBattery()) chips.push_back(L"Питание: максимум");
        if (g_cfg.devBoost && !g_game.On()) OfferBoost(false, L"для " + g_dev.Reason());
        g_avHintAt = GetTickCount64() + 90000;
    } else {
        g_avHintAt = 0;
        gboost::Restore(L'c');
        if (!g_game.On()) gboost::Restore(L's');
        SessionSummary(gboost::TakeSession(false));
        if (g_cfg.devWallpaper) chips.push_back(L"Обои снова идут");
        chips.push_back(L"Фон как был");
        OfferRestore();
    }
    if (g_cfg.hexGlass && !g_game.On())
        g_modefx.Play(ModeFx::DEV, on, on ? L"Режим разработки: " + g_dev.Reason() : L"Режим разработки выключен",
                      chips, CurrentTheme(g_cfg));
}

// Игровой режим включился или выключился: эффекты, рамки окон и перехват мыши.
void OnGameMode(bool on) {
    NoteMode(L"Игровой режим", on, g_game.Reason(), g_gameSince);
    fx::SetGame(on);
    g_frame.Suspend(on);
    llmouse::Suspend(on);            // перехват мыши снят: каждое движение мыши не ходит через нас
    autos::OnModes(on, g_dev.On());  // питание: в игре - производительность
    toolsui::OnModes(on, g_dev.On());
    UpdateTrayTip();
    // Boost: во время игры таймер и приоритеты, при запуске - предложить
    // освободить память
    netui::Game(on, g_game.GamePids(),
                g_game.Reason());  // сеть в игре: туннель и пинг до сервера
    std::vector<std::wstring> chips;
    if (on) {
        boost::Settings s = boost::Load(BoostDir());
        boost::GameTweaks(s, true);
        chips.push_back(L"Красота выключена");
        if (WallpaperPaused(g_cfg.gameWallpaper)) chips.push_back(L"Обои на паузе");
        if (s.ok && s.timer) chips.push_back(L"Таймер 0.5 мс");
        if (g_cfg.boostEco) {
            int n = gboost::GameFirst(g_game.GamePids(), Split(g_cfg.boostKeepFast, L'|'), s.guarded);
            chips.push_back(L"Игре высокий приоритет");
            if (n) chips.push_back(L"Фон в экономии: " + std::to_wstring(n));
        }
        if (g_cfg.boostQuiet) {
            gboost::QuietFor(L'g', true);
            if (gboost::QuietOn()) chips.push_back(L"Уведомления молчат");
        }
        if (McBegin()) chips.push_back(L"Prism усыплён");
        bool battery = tools::OnBattery();
        if (g_cfg.autoPower) chips.push_back(battery ? L"От батареи: баланс" : L"Питание: максимум");
        gboost::SetGame(true, g_game.GamePids(), GameExe(), g_game.Reason(), g_cfg.boostIslc);
        if (g_cfg.boostOffer) OfferBoost(false);
        GamePrep();  // Л21: обновления, загрузки, батарея - одной карточкой
    } else {
        bool quiet = gboost::QuietOn();
        gboost::SetGame(false, {}, L"", L"", false);
        BoostAfterGame();
        gboost::Session sess = gboost::TakeSession(true);
        if (g_mcGame.on)
            McEnd(sess);
        else
            SessionSummary(sess);
        g_dev.Reapply(g_cfg);  // игра вернула обои, а редактор ещё открыт
        chips.push_back(L"Красота вернулась");
        if (g_cfg.gameWallpaper && !(g_dev.On() && g_cfg.devWallpaper)) chips.push_back(L"Обои снова идут");
        if (g_cfg.boostEco) chips.push_back(L"Приоритеты как были");
        if (quiet && !gboost::QuietOn()) chips.push_back(L"Уведомления включены");  // фокус может держать тишину
    }
    if (g_cfg.hexGlass)
        g_modefx.Play(ModeFx::GAME, on, on ? L"Игровой режим: " + g_game.Reason() : L"Игровой режим выключен", chips,
                      CurrentTheme(g_cfg));
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

// Аварийная кнопка трея: всё, что программа поменяла в системе, - назад сразу, без выхода. Режимы выключаются,
// приоритеты, тишина, курсор, прозрачность и рамки чужих окон возвращаются. Итог - карточкой.
void RestoreEverything() {
    std::vector<std::wstring> done;
    if (g_game.On()) {
        g_game.SetManual(false, g_cfg);
        done.push_back(L"игровой режим выключен");
    }
    if (g_dev.On()) {
        g_dev.SetManual(false, g_cfg);
        done.push_back(L"режим разработки выключен");
    }
    gboost::RestoreAll();
    gboost::Quiet(false);
    boost::GameTweaks(boost::Load(BoostDir()), false);
    done.push_back(L"приоритеты и уведомления");
    if (!toolsui::BalanceLowered().empty()) done.push_back(L"фоновые процессы");
    toolsui::BalanceRestoreAll();
    if (!rules::ActiveText().empty()) done.push_back(L"правило «" + rules::ActiveText() + L"»");
    rules::Release();
    g_winfx.ResetAll();
    g_frame.Suspend(true);
    g_restored = true;
    done.push_back(L"окна (прозрачность, закрепление, рамки)");
    cursor::Restore();
    done.push_back(L"курсор");
    std::wstring text;
    for (const std::wstring& d : done) text += (text.empty() ? L"" : L", ") + d;
    g_boostCard.yesLabel.clear();
    g_boostCard.noLabel.clear();
    g_boostCard.Show(BoostCard::DONE, L"Всё вернул как было",
                     text + L". Оформление окон и курсор вернутся после «Перечитать настройки» или перезапуска.",
                     CurrentTheme(g_cfg), 12000);
    autos::Note(L"Вернул всё как было: " + text);
}

void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, CMD_EDITOR, L"Редактор колец");
    AppendMenuW(m, MF_STRING, CMD_EDIT, L"Открыть radial.cfg");
    AppendMenuW(m, MF_STRING, CMD_FOLDER, L"Папка программы");
    AppendMenuW(m, MF_STRING, CMD_RELOAD, L"Перечитать настройки");
    AppendMenuW(m, MF_STRING, CMD_RESET_WINDOWS, L"Вернуть окнам прозрачность и закрепление");
    AppendMenuW(m, MF_STRING, CMD_RESTORE_ALL, L"Вернуть всё как было");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring game = L"Игровой режим";
    if (g_game.On() && !g_game.Manual()) game += L": " + g_game.Reason();
    AppendMenuW(m, MF_STRING | (g_game.On() ? MF_CHECKED : 0), CMD_GAME, game.c_str());
    std::wstring dev = L"Режим разработки";
    if (g_dev.On() && !g_dev.Manual()) dev += L": " + g_dev.Reason();
    AppendMenuW(m, MF_STRING | (g_dev.On() ? MF_CHECKED : 0), CMD_DEV, dev.c_str());
    AppendMenuW(m, MF_STRING, CMD_BOOST, L"Освободить память...");
    std::wstring fs = focus::Status();
    AppendMenuW(
        m, MF_STRING | (focus::On() ? MF_CHECKED : 0), CMD_FOCUS,
        (focus::On() ? fs + L" - закончить" : L"Фокус на " + std::to_wstring(g_cfg.focusMinutes) + L" минут").c_str());
    // Л6: сон после работы - галочка у взведённого, в подписи - чего ждём
    afterwork::Mode aw = afterwork::Armed();
    std::wstring awWhy = aw == afterwork::OFF ? L"" : L" (" + afterwork::Status() + L")";
    AppendMenuW(m, MF_STRING | (aw == afterwork::SLEEP ? MF_CHECKED : 0), CMD_AFTER_SLEEP,
                (L"Усыпить, когда всё закончится" + (aw == afterwork::SLEEP ? awWhy : L"")).c_str());
    AppendMenuW(m, MF_STRING | (aw == afterwork::SHUTDOWN ? MF_CHECKED : 0), CMD_AFTER_OFF,
                (L"Выключить, когда всё закончится" + (aw == afterwork::SHUTDOWN ? awWhy : L"")).c_str());
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

// Бросок файлов на папку кольца: Проводник мог ещё работать - ждём до 15 с, пока станет ясно, что перенесено или
// скопировано, потом карточка с «Вернуть».
struct DropMsg {
    std::vector<dropundo::Moved> done;
    std::wstring dir;
};

void OnDropped(std::vector<std::wstring> files, std::wstring dir) {
    ULONGLONG t0 = dropundo::Now();
    bg::Run([files = std::move(files), dir = std::move(dir), t0] {
        dropundo::Result r;
        for (int i = 0; i < 30 && !bg::Quitting(); ++i) {
            Sleep(i ? 500 : 300);
            r = dropundo::Resolve(files, dir, t0);
            if (!r.pending) break;
        }
        if (r.done.empty()) return;
        auto* m = new DropMsg{std::move(r.done), dir};
        if (!PostMessageW(g_wnd, WM_DROP_DONE, 0, (LPARAM)m)) delete m;
    });
}

void ShowDropped(std::unique_ptr<DropMsg> m) {
    std::wstring names;
    for (size_t i = 0; i < m->done.size() && i < 3; ++i) {
        const std::wstring& f = m->done[i].from;
        names += (i ? L", " : L"") + f.substr(f.find_last_of(L'\\') + 1);
    }
    if (m->done.size() > 3) names += L" и ещё " + std::to_wstring(m->done.size() - 3);
    std::vector<dropundo::Moved> done = m->done;
    g_dropCard.yesLabel = L"Вернуть";
    g_dropCard.noLabel = L"Оставить";
    g_dropCard.onNo = nullptr;
    g_dropCard.onYes = [done] {
        bg::Run([done] {
            std::wstring res = dropundo::Undo(done);
            autos::Note(L"Отмена броска: " + res);
        });
    };
    g_dropCard.Show(BoostCard::OFFER, dropundo::Title(m->done, m->dir), names, CurrentTheme(g_cfg), 12000);
}

// Геймпад: View на 0,6 с - целое кольцо в середине экрана, где курсор; дальше стик и кнопки - самому кольцу.
void OnPad(int action, LPARAM lp) {
    if (action == gamepad::OPEN) {
        if (g_menu.IsOpen()) return;
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        POINT mid{(mi.rcWork.left + mi.rcWork.right) / 2, (mi.rcWork.top + mi.rcWork.bottom) / 2};
        SetCursorPos(mid.x, mid.y);
        g_menu.ShowAround(&g_cfg, mid, mi.rcWork, MonitorDpi(mon));
    } else if (action == gamepad::MOVE) {
        g_menu.PadStick((int)LOWORD(lp) - 32768, (int)HIWORD(lp) - 32768);
    } else {
        g_menu.PadKey(action);
    }
}

// Кольца у края не откроются (PowerToys CursorWrap): карточка при запуске, не чаще раза в неделю.
void WarnEdgeConflict() {
    std::wstring why = edgecheck::Conflict();
    if (why.empty()) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    DWORD day = (DWORD)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 864000000000ull);
    DWORD last = 0, size = sizeof(last);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\MoonRadial", L"EdgeWarnDay", RRF_RT_REG_DWORD, nullptr, &last, &size);
    if (last && day - last < 7) return;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\MoonRadial", L"EdgeWarnDay", REG_DWORD, &day, sizeof(day));
    g_edgeCard.yesLabel.clear();
    g_edgeCard.noLabel.clear();
    g_edgeCard.Show(BoostCard::DONE, L"Кольца у края не откроются", why, CurrentTheme(g_cfg), 20000, Toast::G_WARN);
}

// Прошлый запуск упал: строка в журнал автоматики и карточка с папкой дампа (core/crash). Раз на каждый дамп.
void ReportCrashes() {
    std::vector<crash::Report> rs = crash::Unreported(crash::Dir(), true);
    if (rs.empty()) return;
    // где именно - по карте символов той сборки (cmake/symbols.cmake), если она есть
    auto where = [](const crash::Report& r) {
        std::wstring fn = crash::Decode(r.what);
        return r.what.empty() ? std::wstring() : r.what + (fn.empty() ? L"" : L", функция " + fn);
    };
    for (const crash::Report& r : rs)
        autos::Note(L"Программа падала " + r.when + (r.what.empty() ? L"" : L": " + where(r)) + L". Дамп: " + r.dump);
    const crash::Report& last = rs.back();
    std::wstring dump = last.dump;
    g_boostCard.yesLabel = L"Папка";
    g_boostCard.noLabel = L"Понятно";
    g_boostCard.onYes = [dump] {
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + dump + L"\"").c_str(), nullptr,
                      SW_SHOWNORMAL);
    };
    g_boostCard.onNo = nullptr;
    g_boostCard.Show(BoostCard::OFFER,
                     rs.size() == 1 ? L"Программа падала " + last.when
                                    : L"Программа падала " + std::to_wstring(rs.size()) + L" раза",
                     (last.what.empty() ? L"" : where(last) + L". ") + L"Дамп: " + dump + L" (хранятся последние " +
                         std::to_wstring(crash::kKeep) + L").",
                     CurrentTheme(g_cfg), 30000, Toast::G_WARN);
}

// Вернуть всё, что программа меняла в системе, и остановить потоки. Зовётся при выходе и при выключении / выходе из
// сеанса (WM_ENDSESSION: WM_DESTROY тогда не приходит, а тишина, приоритеты и прозрачность иначе ждали бы следующего
// запуска). Второй вызов ничего не делает.
void ShutdownAll() {
    static bool done = false;
    if (done) return;
    done = true;
    // выход или конец сеанса - не авария: что бы ни случилось дальше, новую копию не поднимать
    crash::SetRestart(false);
    WTSUnRegisterSessionNotification(g_wnd);
    if (g_displayNotify) UnregisterPowerSettingNotification(g_displayNotify);
    if (g_acdcNotify) UnregisterPowerSettingNotification(g_acdcNotify);
    g_wave.Destroy();
    g_modefx.Destroy();
    gboost::Stop();
    gboost::RestoreAll();
    gboost::Quiet(false);
    g_boostCard.Destroy();
    g_pick.Destroy();
    g_gbCard.Destroy();
    g_mcCard.Destroy();
    g_mcCard2.Destroy();
    g_edgeCard.Destroy();
    g_askCard.Destroy();
    resume::Shutdown();
    afterwork::Shutdown();
    focus::Shutdown();
    g_dropCard.Destroy();
    mc::StopWatch();
    netui::Shutdown();
    toolsui::Shutdown();
    cliphist::Stop();
    appvol::Stop();
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
    gamepad::Stop();
    rules::Release();
    g_snip.Destroy();
    cursor::Restore();
    RemoveTray();
    bg::Quit(1500);  // фоновые задачи видят Quitting() и не пишут в уже убранное; дольше выход не ждёт
}

// Отладочные сообщения (WM_APP + 50..58, 66..68: кольца, портал, «выключить экран», карточки на выдуманных данных)
// принимаются только при RADIAL_DEV=1 или файле dev.on рядом с exe: иначе их мог бы послать любой процесс.
bool IsDevMsg(UINT msg) {
    return (msg >= WM_APP + 50 && msg <= WM_APP + 58) || (msg >= WM_APP + 66 && msg <= WM_APP + 68);
}

bool DevMsgsOn() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("RADIAL_DEV");
        on = (e && *e == '1') || GetFileAttributesW((ExeDir() + L"\\dev.on").c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    return on == 1;
}

LRESULT CALLBACK AppProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (IsDevMsg(msg) && !DevMsgsOn()) return 0;
    switch (msg) {
        case WM_TIMER:
            if (wp == kPollTimer) Poll();
            if (wp == kActionTimer) {
                KillTimer(h, kActionTimer);
                corners::Run(g_pendingAction);
            }
            if (wp == kAutostartTimer) {
                KillTimer(h, kAutostartTimer);
                AutostartRefresh();
                ReportCrashes();
                WarnEdgeConflict();
                // готовы ли задачи с правами - узнать заранее в фоне: кольцо и студия берут готовый ответ
                elevate::TaskReadyCached(elevate::TASK_NET);
                elevate::TaskReadyCached(elevate::TASK_PURGE);
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
            if (wp == palette::kHotId) palette::Show();
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
                case CMD_RESTORE_ALL:
                    RestoreEverything();
                    break;
                case CMD_BOOST:
                    OfferBoost(true);
                    break;
                case CMD_FOCUS:
                    focus::On() ? focus::Stop() : focus::Start(0);
                    break;
                case CMD_AFTER_SLEEP:
                    afterwork::Arm(afterwork::Armed() == afterwork::SLEEP ? afterwork::OFF : afterwork::SLEEP);
                    break;
                case CMD_AFTER_OFF:
                    afterwork::Arm(afterwork::Armed() == afterwork::SHUTDOWN ? afterwork::OFF : afterwork::SHUTDOWN);
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
        case gamepad::WM_PAD:
            OnPad((int)wp, lp);
            return 0;
        case WM_POLL_WAKE:
            if (g_pollMs > 20) Poll();
            return 0;
        case WM_DROP_DONE:
            ShowDropped(std::unique_ptr<DropMsg>(reinterpret_cast<DropMsg*>(lp)));
            return 0;
        case WM_BOOST_DONE:
            BoostDone(reinterpret_cast<boost::Result*>(lp));
            return 0;
        case gboost::WM_GBOOST: {
            std::unique_ptr<gboost::Note> n(reinterpret_cast<gboost::Note*>(lp));
            OnBoostNote(*n);
            return 0;
        }
        case WM_APP + 66: {  // для проверки: анимация режима (0/1 - игра вкл/выкл, 2/3 - Unreal вкл/выкл)
            bool on = !(wp & 1);
            if (wp < 2)
                g_modefx.Play(
                    ModeFx::GAME, on, on ? L"Игровой режим: NFS14" : L"Игровой режим выключен",
                    on ? std::vector<std::wstring>{L"Красота выключена", L"Обои на паузе", L"Таймер 0.5 мс",
                                                   L"Игре высокий приоритет", L"Фон в экономии: 4",
                                                   L"Уведомления молчат"}
                       : std::vector<std::wstring>{L"Красота вернулась", L"Обои снова идут", L"Приоритеты как были"},
                    CurrentTheme(g_cfg));
            else
                g_modefx.Play(
                    ModeFx::DEV, on, on ? L"Режим разработки: Unreal Editor" : L"Режим разработки выключен",
                    on ? std::vector<std::wstring>{L"Обои на паузе", L"Видеопамять", L"Шейдеры", L"Сборки UBT"}
                       : std::vector<std::wstring>{L"Обои снова идут", L"Фон как был"},
                    CurrentTheme(g_cfg));
            return 0;
        }
        case WM_APP + 68:  // для проверки: Minecraft (0 - кольцо, 1 - падение, 2 - AlwaysPreTouch, 3 - итог)
            McDemo((int)wp);
            return 0;
        case WM_APP + 67:  // для проверки: карточки Boost на выдуманных данных (0 - выбор, 1 - итог, 2 - видеокарта)
            BoostDemo((int)wp);
            return 0;
        case WM_WTSSESSION_CHANGE:
            g_reveal.OnSession(wp);
            if (wp == WTS_SESSION_UNLOCK) netui::Wake();
            if (wp == WTS_SESSION_LOCK) resume::OnLock();  // Л4: запомнить, что открыто
            if (wp == WTS_SESSION_UNLOCK) resume::OnUnlock();
            return 0;
        case WM_GAMEPREP:
            GamePrepShow(lp);
            return 0;
        case resume::WM_RESUME:
            resume::OnMessage(wp, lp);
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
                if (wp == PBT_APMSUSPEND) resume::OnLock();
                if (wp == PBT_APMRESUMESUSPEND) resume::OnUnlock();
            }
            return TRUE;
        case WM_QUERYENDSESSION:
            resume::SaveNow();  // Л4: выключают - запомнить, что было открыто
            return TRUE;
        case WM_ENDSESSION:
            if (wp) ShutdownAll();  // после ответа процесс завершат без WM_DESTROY
            return 0;
        case WM_DESTROY:
            ShutdownAll();
            PostQuitMessage(0);
            return 0;
    }
    if (msg == g_taskbarCreated) {
        AddTray();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace app

using namespace app;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    crash::Install();  // первым делом: упадёт хоть что-то дальше - останется дамп
    if (int code = 0; devcli::Run(code)) return code;
    bool wantEditor = devcli::HasFlag(L"--editor");
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MoonRadialMenu");
    // ответ запоминаем сразу: HasFlag разбирает командную строку и затирает GetLastError - так вторая копия
    // считала, что первой нет, и запускалась рядом (два кольца, два перехвата мыши)
    bool running = GetLastError() == ERROR_ALREADY_EXISTS;
    // перезапуск после падения: старая копия ещё умирает и держит мьютекс - он освободится, когда её не станет
    if (running && mutex && devcli::HasFlag(L"--after-crash")) {
        DWORD w = WaitForSingleObject(mutex, 15000);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) running = false;
    }
    if (running) {
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
    gboost::Recover();  // приоритеты, экономия и тишина, оставшиеся от упавшего прошлого раза
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
            g_modefx.Play(ModeFx::GAME, true, L"Игровой режим: так это выглядит",
                          {L"Красота выключена", L"Обои на паузе", L"Игре высокий приоритет"}, CurrentTheme(g_cfg));
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
    llmouse::Set(llmouse::EDGE, OnEdgeMouse);
    fx::SetWaker([] {
        if (g_pollMs > 20) PostMessageW(g_wnd, WM_POLL_WAKE, 0, 0);
    });
    llmouse::Need(llmouse::EDGE, true);
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
    g_modefx.Create(inst);
    g_boostCard.Create(inst);
    g_pick.Create(inst);
    g_gbCard.Create(inst);
    g_mcCard.Create(inst);
    g_mcCard2.Create(inst);
    g_askCard.Create(inst);
    // Л11: вид карточек трижды подряд закрыт без действия - один раз спросить; заглушённое - в вечерний дайджест
    cardlog::SetDigest([](const std::wstring& title, const std::wstring& text) { return autos::Digest(title, text); });
    cardlog::SetAsk([](const std::wstring& kind) {
        if (fx::Game()) return;  // в игре не спрашиваем: третий раз ещё будет
        g_askCard.kind = L"-";
        g_askCard.yesLabel = L"Не показывать";
        g_askCard.noLabel = L"Оставить";
        g_askCard.onYes = [kind] { cardlog::Mute(kind, true); };
        g_askCard.onNo = nullptr;
        g_askCard.Show(BoostCard::OFFER, L"«" + kind + L"» - больше не показывать?",
                       L"Трижды подряд закрыта без действия. Такие карточки будут ждать в студии (Состояние - "
                       L"пропущенные) и в вечернем дайджесте; вернуть можно там же.",
                       CurrentTheme(g_cfg), 20000);
    });
    gboost::Start(g_wnd);
    g_game.recognize = [](DWORD pid, const std::wstring& path) { return mc::GameName(pid, path); };
    corners::SetMinecraft(ShowMcRing);
    g_menu.onDropped = OnDropped;
    g_menu.onAction = [](const std::wstring& a) {
        g_pendingAction = a;
        SetTimer(g_wnd, kActionTimer, 260, nullptr);  // кольцо успевает погаснуть
    };
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
        [](int page, const std::wstring& focus) {
            OpenEditor(g_inst, g_cfgPath, [] { Reload(); }, ShowFromStudio);
            if (page >= 0)
                EditorGoPage(page);
            else
                EditorGoState(focus);
        });
    corners::SetTools([] {
        if (g_menu.IsOpen()) return;
        toolsui::ShowRing(g_menu);
    });
    palette::Init(g_wnd, &g_cfg, &g_menu, [](int page) {
        OpenEditor(g_inst, g_cfgPath, [] { Reload(); }, ShowFromStudio);
        EditorGoPage(page);
    });
    palette::Apply();
    gamepad::Start(g_wnd, g_cfg.gamepad);
    cliphist::Start(inst, &g_cfg);
    appvol::Start(g_cfg.appVolume);     // Л36: громкость по программам
    resume::Init(inst, g_wnd, &g_cfg);  // Л4: продолжить, где остановился
    afterwork::Init(inst, &g_cfg);      // Л6: сон после работы
    focus::Init(inst, &g_cfg);          // Л8, Л34: фокус и шахматы без отвлечений
    g_menu.focusStatus = [] { return focus::Status(); };
    palette::AddSource([](std::vector<Item>& out) {
        Item it;
        it.name = focus::On() ? L"Фокус: закончить" : L"Фокус на " + std::to_wstring(g_cfg.focusMinutes) + L" минут";
        it.path = L"f|toggle";
        it.special = 3;
        out.push_back(it);
    });
    palette::AddRunner(L"f|", [](const std::wstring&) { focus::On() ? focus::Stop() : focus::Start(0); });
    palette::AddSource([](std::vector<Item>& out) {  // Л20: «Запустить: shop» - VS Code, сервер, браузер
        for (const std::wstring& d : toolsui::LaunchableProjects()) {
            Item it;
            it.name = L"Запустить: " + d.substr(d.find_last_of(L'\\') + 1);
            it.path = L"l|" + d;
            it.special = 3;
            out.push_back(it);
        }
    });
    palette::AddRunner(L"l|", [](const std::wstring& dir) { toolsui::LaunchProject(dir); });
    corners::SetClip([] { cliphist::ShowRing(g_menu); });
    palette::AddSource([](std::vector<Item>& out) {
        for (Item& it : cliphist::Items(L"b|")) {
            it.name = L"Буфер: " + it.name;
            out.push_back(std::move(it));
        }
    });
    palette::AddRunner(L"b|", [](const std::wstring& n) { cliphist::Use(_wtoi(n.c_str())); });
    layout::Init(&g_cfg);
    corners::SetLayout([] { layout::ShowRing(g_menu); });
    palette::AddSource([](std::vector<Item>& out) {
        for (Item& it : layout::Items(L"z|")) out.push_back(std::move(it));
    });
    palette::AddRunner(L"z|", [](const std::wstring& key) { layout::Run(key); });
    recent::Init(&g_cfg);
    monitors::Init(&g_cfg);
    corners::SetMonitors([] { monitors::ShowRing(g_menu); });
    corners::SetRecent([] { recent::ShowRing(g_menu); });
    palette::AddSource([](std::vector<Item>& out) {
        for (Item& it : recent::Items(L"r|")) out.push_back(std::move(it));
    });
    palette::AddRunner(L"r|", [](const std::wstring& path) { recent::Open(path); });
    worksets::Init(g_wnd, &g_cfg);
    corners::SetSets([] { worksets::ShowRing(g_menu); });
    palette::AddSource([](std::vector<Item>& out) {
        for (Item& it : worksets::Items(L"w|")) out.push_back(std::move(it));
    });
    palette::AddRunner(L"w|", [](const std::wstring& key) { worksets::Run(key); });
    // вошли в Windows только что: стол открывается порталом
    if (g_cfg.bootPortal && !wantEditor && JustLoggedOn()) SetTimer(g_wnd, kIntroTimer, 400, nullptr);
    g_game.onChange = OnGameMode;
    g_dev.onChange = OnDevMode;
    g_dev.gameOn = [] { return g_game.On() && g_cfg.gameWallpaper; };
    g_game.wallpaperHeld = [] { return g_dev.On() && g_cfg.devWallpaper; };
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
    OleUninitialize();
    ReleaseMutex(mutex);
    // Всё своё уже убрано (ShutdownAll). Дальше C++ разрушал бы глобальные объекты - кольцо, кэши значков, карточки с
    // картинками GDI+ - уже после GdiplusShutdown и рядом с ещё живыми фоновыми потоками: выход падал в gdiplus.dll,
    // а перезапуск после падения поднимал новую копию. Память и дескрипторы всё равно вернёт система.
    (void)gdipToken;
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
