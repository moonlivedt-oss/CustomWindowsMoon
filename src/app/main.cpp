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

#include "app/autostart.h"
#include "app/devcli.h"
#include "app/netui.h"
#include "app/palette.h"
#include "app/worksets.h"
#include "desk/cliphist.h"
#include "desk/layout.h"
#include "app/autos.h"
#include "app/toolsui.h"
#include "core/bg.h"
#include "core/crash.h"
#include "core/edgecheck.h"
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
    CMD_EXIT,
    CMD_RESTORE_ALL
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
ModeFx g_modefx;  // анимация включения и выключения игрового режима и Unreal
BoostCard g_boostCard;
BoostPick g_pick;        // выбор по программам: закрыть, усыпить, оставить
BoostCard g_gbCard;      // новости Boost во время игры и работы: видеокарта, шейдеры, сборки, итог
std::wstring g_pickFor;  // чей профиль выбора: exe игры, редактора или «вручную»
ULONGLONG g_avHintAt = 0;
BoostCard g_edgeCard;           // что мешает кольцам у края (CursorWrap)
BoostCard g_mcCard, g_mcCard2;  // Minecraft: падение / AlwaysPreTouch и подкачка
struct McGame {
    bool on = false;
    DWORD pid = 0;
    mc::Instance inst;
} g_mcGame;
std::function<void()> g_afterBoost;       // запуск сборки после «Применить» в карточке выбора
ULONGLONG g_offerQuietUntil = 0;          // только что освобождали память перед запуском - при старте не спрашивать
std::wstring g_pendingAction;             // действие из кольца (action:...) - после того, как кольцо погаснет
constexpr UINT_PTR kActionTimer = 5;      // когда предложить исключения антивируса для Unreal (0 - не надо)
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
bool g_restored = false;  // «Вернуть всё как было»: рамки окон на паузе до следующего перечитывания настроек
bool g_altTab = false;    // за это нажатие Alt был Tab: идёт переключение окон, кольцо у края не открываем

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
    autos::Tick();
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

void RunAfterBoost() {
    if (!g_afterBoost) return;
    auto f = std::move(g_afterBoost);
    g_afterBoost = nullptr;
    f();
}

// Что сделать по выбору в карточке: закрыть (boost::Free) и усыпить
// (gboost::Soften) - в потоке.
struct BoostJob {
    std::vector<boost::Group> close;
    std::vector<DWORD> sleep;
    int sleepApps = 0;
};

void BoostWork(const BoostJob* job) {
    std::wstring dir = BoostDir();
    double before = boost::AvailMb();
    auto* r = new boost::Result;
    if (!job->close.empty()) *r = boost::Free(boost::Load(dir), dir, job->close);
    if (!job->sleep.empty()) {
        gboost::Soften(job->sleep);
        r->slept = job->sleepApps;
        Sleep(400);
    }
    r->freedMb = std::max(0.0, boost::AvailMb() - before);
    PostMessageW(g_wnd, WM_BOOST_DONE, 0, (LPARAM)r);
}

void StartBoost(BoostJob* job) {
    if (g_boostBusy) {
        delete job;
        return;
    }
    g_boostBusy = true;
    g_boostCard.Show(BoostCard::WORKING, L"Освобождаю память...",
                     job->close.empty() ? L"Усыпляю: память программ уходит в подкачку, "
                                          L"процессор - в экономию."
                                        : L"Программы закрываются мягко, как крестиком; кто не "
                                          L"закрылся - через пару "
                                          L"секунд добивается.",
                     CurrentTheme(g_cfg));
    std::shared_ptr<BoostJob> j(job);
    if (!bg::Run([j] { BoostWork(j.get()); })) g_boostBusy = false;
}

std::wstring GameExe() {
    for (DWORD pid : g_game.GamePids()) {
        std::wstring p = gm::ProcessPath(pid);
        if (!p.empty()) return p;
    }
    return L"";
}

// Путь к запущенному Unreal Editor (для списка исключений антивируса).
std::wstring EditorExe() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    PROCESSENTRY32W pe{sizeof(pe)};
    std::wstring out;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && out.empty(); ok = Process32NextW(snap, &pe))
        if (gm::MatchAny(pe.szExeFile, g_cfg.devApps)) out = gm::ProcessPath(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

// Карточка «освободить память»: при запуске игры или редактора (если памяти
// мало по minFreePercent и есть что закрыть) или вручную из трея, угла, студии.
// why - к чему готовимся: «перед игрой», «для Unreal Editor». Что не предлагать
// закрыть: сама игра и редактор.
std::vector<std::wstring> KeepApps() {
    std::vector<std::wstring> keep;
    if (g_game.On())
        for (DWORD pid : g_game.GamePids()) {
            std::wstring full = gm::ProcessPath(pid);
            if (!full.empty()) keep.push_back(gm::FileName(full));
        }
    if (g_dev.On()) keep.push_back(L"UnrealEditor.exe");
    return keep;
}

// Что делать с программой, пока для игры нет своего выбора: WebView держит
// поиск Windows - оставить, мессенджеры с голосом - усыпить (закрытие оборвёт
// звонок), остальное - закрыть, как раньше.
int DefaultAct(const std::wstring& key) {
    if (key.find(L"webview") != std::wstring::npos) return gboost::ACT_KEEP;
    for (const wchar_t* m : {L"discord", L"telegram", L"whatsapp", L"viber", L"skype"})
        if (key == m) return gboost::ACT_SLEEP;
    return gboost::ACT_CLOSE;
}

void ApplyPick(const std::vector<BoostPick::Row>& rows) {
    std::map<std::wstring, int> acts;
    auto* job = new BoostJob;
    for (const BoostPick::Row& r : rows) {
        acts[r.key] = r.act;
        for (const boost::Group& g : g_boostGroups) {
            if (Lower(g.name) != r.key) continue;
            if (r.act == gboost::ACT_CLOSE) job->close.push_back(g);
            if (r.act == gboost::ACT_SLEEP) {
                job->sleep.insert(job->sleep.end(), g.pids.begin(), g.pids.end());
                job->sleepApps++;
            }
        }
    }
    gboost::SaveProfile(g_pickFor, acts);
    if (job->close.empty() && job->sleep.empty()) {
        delete job;
        g_boostCard.Show(BoostCard::DONE, L"Ничего не трогаю", L"Выбор запомнен.", CurrentTheme(g_cfg), 3500);
        RunAfterBoost();
        return;
    }
    StartBoost(job);
}

void OfferBoost(bool manual, const std::wstring& why = L"перед игрой") {
    if (g_boostBusy) return;
    if (!manual && GetTickCount64() < g_offerQuietUntil) return;  // память освободили перед запуском сборки
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
    // профиль: для игры - по имени её exe, для редактора - по его имени
    std::wstring who;
    if (manual) {
        g_pickFor = L"вручную";
    } else if (g_game.On() && g_mcGame.on) {
        g_pickFor = L"mc:" + (g_mcGame.inst.id.empty() ? std::wstring(L"minecraft") : Lower(g_mcGame.inst.id));
        who = g_game.Reason();
    } else if (g_game.On()) {
        std::wstring exe = GameExe();
        g_pickFor = exe.empty() ? g_game.Reason() : gm::FileName(exe);
        who = g_game.Reason();
    } else {
        g_pickFor = L"unrealeditor";
        who = g_dev.Reason();
    }
    std::map<std::wstring, int> prof = gboost::Profile(g_pickFor);
    std::vector<BoostPick::Row> rows;
    bool remembered = false;
    for (const boost::Group& g : g_boostGroups) {
        BoostPick::Row r{g.title.empty() ? g.name : g.title, Lower(g.name), g.mb, DefaultAct(Lower(g.name))};
        auto it = prof.find(r.key);
        if (it != prof.end()) r.act = it->second, remembered = true;
        rows.push_back(r);
    }
    g_pick.onApply = [](const std::vector<BoostPick::Row>& r) { ApplyPick(r); };
    g_pick.onSkip = nullptr;
    g_pick.Show(manual ? L"Освободить память?" : L"Освободить память " + why + L"?",
                L"Сейчас свободно " + Size(avail) +
                    (remembered && !who.empty() ? L". Выбор - как в прошлый раз для " + who + L"."
                                                : L". «Усыпить» - программа остаётся открытой, но не мешает."),
                rows, t);
}

void BoostDone(boost::Result* r) {
    g_boostBusy = false;
    for (const auto& p : r->reopen)
        if (std::find(g_boostClosed.begin(), g_boostClosed.end(), p) == g_boostClosed.end()) g_boostClosed.push_back(p);
    std::wstring what;
    if (r->closed) what += L"Закрыто: " + std::to_wstring(r->closed);
    if (r->slept) what += (what.empty() ? L"Усыплено: " : L", усыплено: ") + std::to_wstring(r->slept);
    g_boostCard.Show(BoostCard::DONE, L"Освобождено " + Size(r->freedMb),
                     what + L". Свободно " + Size(boost::AvailMb()) +
                         (g_cfg.boostRestore && !r->reopen.empty() ? L". После игры закрытое можно будет вернуть."
                          : r->slept                               ? L". Усыплённые проснутся после игры."
                                                                   : L"."),
                     CurrentTheme(g_cfg));
    delete r;
    RunAfterBoost();
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

// Игра закрылась: приоритеты и таймер назад, ещё одна очистка, предложить
// вернуть закрытое.
void BoostAfterGame() {
    boost::Settings s = boost::Load(BoostDir());
    boost::GameTweaks(s, false);
    gboost::Restore(L'g');
    if (!g_dev.On()) gboost::Restore(L's');  // усыплённые для редактора спят, пока он открыт
    gboost::Quiet(false);
    if (g_cfg.boostAfter && s.purge) boost::RunPurge(s);
    OfferRestore();
}

std::wstring Duration(ULONGLONG ms) {
    int m = (int)(ms / 60000);
    wchar_t b[32];
    if (m >= 60)
        swprintf(b, 32, L"%d ч %d мин", m / 60, m % 60);
    else
        swprintf(b, 32, L"%d мин", std::max(1, m));
    return b;
}

std::wstring MinSec(double sec) {
    int s = (int)std::lround(sec);
    wchar_t b[32];
    if (s < 60)
        swprintf(b, 32, L"%d с", s);
    else
        swprintf(b, 32, L"%d:%02d", s / 60, s % 60);
    return b;
}

// Итог игры (или долгой работы в Unreal, если памяти не хватало): строка в
// историю и карточка.
void SessionSummary(const gboost::Session& s, bool save = true) {
    if (save) gboost::SaveSession(s);
    if (!g_cfg.boostSummary || s.ms < 3 * 60000 || s.samples < 6) return;
    bool tight = s.stallSamples >= 3 || s.pageGrowMb > 512;
    if (!s.game && !tight) return;  // в редакторе говорим только о проблемах
    // в карточку влезает четыре строки: сначала цифры, потом вывод
    std::wstring sub = L"Память до " + Size(s.peakUsedMb) + L" из " + Size(s.totalMb);
    if (s.peakVramMb > 1 && s.vramMb > 0) sub += L", видео до " + Size(s.peakVramMb) + L" из " + Size(s.vramMb);
    sub += L".";
    if (tight)
        sub += L" Не хватало: подкачка +" + Size(s.pageGrowMb) + L", диск мешал " +
               std::to_wstring(s.stallSamples * 5) + L" с - в следующий раз усыпляй больше.";
    else
        sub += L" Памяти хватало.";
    if (s.purges) sub += L" Кэш чистился " + Plural(s.purges, L"раз", L"раза", L"раз") + L".";
    g_gbCard.Show(BoostCard::DONE, (s.game ? L"Сыграно " : L"В Unreal ") + Duration(s.ms) + L": " + s.name, sub,
                  CurrentTheme(g_cfg), 16000, tight ? Toast::G_WARN : Toast::G_INFO);
}

bool WallpaperPaused(bool cfgFlag) { return cfgFlag && !gm::WallpaperEngine().empty(); }

void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

// Один раз: исключения антивируса для папок Unreal - сборка и шейдеры заметно
// быстрее.
void ShowAvHint() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\av_hint.done", ad, MAX_PATH);
    if (Exists(ad) || !g_dev.On()) return;
    if (HANDLE f = CreateFileW(ad, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr); f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    std::wstring list = gboost::UnrealExclusions(EditorExe());
    g_gbCard.yesLabel = L"Скопировать список";
    g_gbCard.noLabel = L"Не нужно";
    g_gbCard.onNo = nullptr;
    g_gbCard.onYes = [list] {
        CopyText(list);
        g_gbCard.Show(BoostCard::DONE, L"Список в буфере",
                      L"Kaspersky: Настройки - Угрозы и исключения - Управление "
                      L"исключениями - Добавить. "
                      L"Папки и процессы - по одной строке.",
                      CurrentTheme(g_cfg), 16000);
    };
    g_gbCard.Show(BoostCard::OFFER, L"Ускорить сборки Unreal?",
                  L"Антивирус проверяет каждый файл компилятора и шейдеров. "
                  L"Исключения для папок движка, проектов и "
                  L"кэша ускоряют сборку. Список соберу, добавишь сам.",
                  CurrentTheme(g_cfg), 30000, Toast::G_INFO);
}

// Новости слежки Boost (поток gboost): видеокарта, видеопамять, шейдеры,
// сборки.
void OnBoostNote(const gboost::Note& n) {
    ThemeInfo t = CurrentTheme(g_cfg);
    switch (n.ev) {
        case gboost::EV_GPU_WRONG: {
            if (!g_cfg.boostGpu) return;
            size_t bar = n.sub.find(L'|');
            // «NVIDIA GeForce RTX 5050 Laptop GPU» -> «RTX 5050»: в карточку влезает четыре строки
            auto shortName = [](std::wstring g) {
                for (const wchar_t* cut :
                     {L"NVIDIA GeForce ", L"AMD Radeon(TM) ", L"(R)", L"(TM)", L" Laptop GPU", L" Graphics"})
                    for (size_t at; (at = g.find(cut)) != std::wstring::npos;) g.erase(at, wcslen(cut));
                return Trim(g);
            };
            std::wstring ig = shortName(n.sub.substr(0, bar)),
                         dg = bar == std::wstring::npos ? L"" : shortName(n.sub.substr(bar + 1));
            std::wstring exe = n.text;
            g_gbCard.yesLabel = L"Включить";
            g_gbCard.noLabel = L"Не нужно";
            g_gbCard.onNo = nullptr;
            g_gbCard.onYes = [exe, dg] {
                bool ok = gboost::PreferDiscrete(exe);
                g_gbCard.Show(BoostCard::DONE, ok ? L"Готово" : L"Не получилось",
                              ok ? L"При следующем запуске игра пойдёт на " + dg + L"."
                                 : L"Windows не дала записать настройку видеокарты.",
                              CurrentTheme(g_cfg));
            };
            g_gbCard.Show(BoostCard::OFFER, L"Игра на встроенной видеокарте",
                          L"«" + gm::FileName(exe) + L"» рисует на " + ig + L", а " + dg +
                              L" простаивает. «Включить» - со следующего запуска игра пойдёт на " + dg + L".",
                          t, 30000, Toast::G_WARN);
            return;
        }
        case gboost::EV_VRAM_HIGH:
            if (!g_cfg.devWatch) return;
            g_gbCard.Show(BoostCard::DONE, L"Видеопамять: " + Size(n.a) + L" из " + Size(n.b),
                          L"Unreal близок к «Out of video memory». Помогут Scalability пониже "
                          L"(Settings - Engine "
                          L"Scalability), один вьюпорт вместо двух и закрытое видео в браузере.",
                          t, 20000, Toast::G_WARN);
            return;
        case gboost::EV_SHADERS_START: {
            if (!g_cfg.devWatch) return;
            int eco = 0;
            if (g_cfg.boostEco)
                eco = gboost::BackgroundEco({L"UnrealEditor", L"ShaderCompileWorker", L"UnrealBuildTool", L"UbaHost",
                                             L"dotnet", L"cl", L"link"},
                                            boost::Load(BoostDir()).guarded);
            g_gbCard.Show(BoostCard::DONE, L"Компилируются шейдеры",
                          L"Работают " + Plural((int)n.a, L"процесс", L"процесса", L"процессов") +
                              (eco ? L", фон приглушён до конца." : L".") + L" Сообщу, когда закончится.",
                          t, 7000, Toast::G_INFO);
            return;
        }
        case gboost::EV_SHADERS_DONE:
            gboost::Restore(L'c');
            if (!g_cfg.devWatch || n.a < 15) return;
            g_gbCard.Show(BoostCard::DONE, L"Шейдеры готовы за " + MinSec(n.a),
                          L"Больше всего работало " + Plural((int)n.b, L"процесс", L"процесса", L"процессов") + L".", t,
                          7000, Toast::G_OK);
            return;
        case gboost::EV_BUILD_DONE:
            if (!g_cfg.devWatch) return;
            if (n.b > 0)
                g_gbCard.Show(BoostCard::DONE, L"Сборка готова за " + MinSec(n.a), n.sub, t, 7000, Toast::G_OK);
            else
                g_gbCard.Show(BoostCard::DONE, L"Сборка не удалась",
                              n.text.empty() ? L"Подробности - в окне вывода." : n.text, t, 20000, Toast::G_FAIL);
            return;
        case gboost::EV_PURGED:
            return;  // тихо: итог игры скажет, сколько раз
    }
}

// Для проверки без игры: карточки на выдуманных данных.
void BoostDemo(int what) {
    ThemeInfo t = CurrentTheme(g_cfg);
    if (what == 0) {
        g_pick.onApply = [](const std::vector<BoostPick::Row>&) {};
        g_pick.Show(L"Освободить память перед игрой?", L"Сейчас свободно 3.1 ГБ. Выбор - как в прошлый раз для NFS14.",
                    {{L"Google Chrome", L"chrome", 1840, 0},
                     {L"Discord", L"discord", 620, 1},
                     {L"Telegram Desktop", L"telegram", 310, 1},
                     {L"Microsoft Edge WebView2", L"msedgewebview2", 280, 2},
                     {L"Yandex.Disk", L"yandexdisk2", 120, 0}},
                    t);
    } else if (what == 1) {
        gboost::Session s;
        s.name = L"NFS14";
        s.ms = 102 * 60000;
        s.samples = 1200;
        s.peakUsedMb = 13500;
        s.totalMb = 15800;
        s.pageGrowMb = 1300;
        s.stallSamples = 7;
        s.peakVramMb = 7100;
        s.vramMb = 8188;
        s.purges = 3;
        bool was = g_cfg.boostSummary;
        g_cfg.boostSummary = true;
        g_gbCard.Show(BoostCard::DONE, L"", L"", t);  // сброс подписей
        SessionSummary(s, false);                     // выдуманная сессия - не в историю
        g_cfg.boostSummary = was;
    } else {
        auto* n = new gboost::Note{gboost::EV_GPU_WRONG, 40, 0, L"D:\\Games\\NFS14\\NFS14.exe",
                                   L"Intel(R) UHD Graphics|NVIDIA GeForce RTX 5050 Laptop GPU"};
        PostMessageW(g_wnd, gboost::WM_GBOOST, n->ev, (LPARAM)n);
    }
}

// ---------------- Minecraft ----------------

// Отметки «не предлагать снова» в %APPDATA%\MoonDesk\mc.ini: pretouch-no:<id>,
// pagefile-said (день).
std::wstring McIni() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\mc.ini", p, MAX_PATH);
    return p;
}
std::wstring McGet(const std::wstring& key) {
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"mc", key.c_str(), L"", buf, 256, McIni().c_str());
    return buf;
}
void McPut(const std::wstring& key, const std::wstring& v) {
    WritePrivateProfileStringW(L"mc", key.c_str(), v.c_str(), McIni().c_str());
}
int Today() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 864000000000ull);
}

// Игра началась: если это Minecraft - слежка за логом и кучей, Prism - усыпить.
// true - Prism усыплён.
bool McBegin() {
    g_mcGame = McGame{};
    for (DWORD pid : g_game.GamePids()) {
        std::wstring path = gm::ProcessPath(pid), id;
        int xmx = 0;
        if (path.empty() || !mc::IsMinecraft(pid, path, id, xmx)) continue;
        g_mcGame.on = true;
        g_mcGame.pid = pid;
        if (id.empty() || !mc::FindInstance(id, g_mcGame.inst)) {
            wchar_t d[MAX_PATH];
            ExpandEnvironmentStringsW(L"%APPDATA%\\.minecraft", d, MAX_PATH);
            g_mcGame.inst.name = L"Minecraft";
            g_mcGame.inst.game = d;
        }
        if (xmx) g_mcGame.inst.maxMb = xmx;  // в командной строке - то, с чем игра запущена на самом деле
        mc::StartWatch(pid, path, g_mcGame.inst);
        break;
    }
    if (!g_mcGame.on || !g_cfg.boostEco) return false;
    std::vector<DWORD> prism = mc::PrismPids();
    if (prism.empty()) return false;
    gboost::Soften(prism);  // лаунчер во время игры не нужен; проснётся после неё
    return true;
}

// Игра кончилась: падение, итог, AlwaysPreTouch, подкачка.
void McEnd(const gboost::Session& s) {
    mc::Stats st = mc::StopWatch();
    mc::Instance in = g_mcGame.inst;
    g_mcGame = McGame{};
    gboost::SaveSession(s);
    ThemeInfo t = CurrentTheme(g_cfg);
    // 1. упала - важнее всего
    mc::Crash c;
    bool crashed = mc::NewCrash(in.game, st.startWall, c);
    if (crashed || st.oom) {
        std::wstring sub;
        if (crashed) sub = c.what + L".";
        if (!c.mods.empty()) sub += (c.jvm ? L" Где: " : L" Подозрение: ") + c.mods + L".";
        if (st.oom) sub += L" Java не хватило памяти (OutOfMemoryError).";
        std::wstring file = c.file;
        g_mcCard.yesLabel = crashed ? L"Открыть отчёт" : L"Понятно";
        g_mcCard.noLabel = L"Закрыть";
        g_mcCard.onNo = nullptr;
        g_mcCard.onYes = [file] {
            if (!file.empty()) ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        g_mcCard.Show(BoostCard::OFFER,
                      (c.jvm                ? L"Java упала: "
                       : st.oom && !crashed ? L"Не хватило памяти: "
                                            : L"Minecraft упал: ") +
                          in.name,
                      Trim(sub), t, 45000, Toast::G_FAIL);
    }
    // 2. итог
    if (g_cfg.boostSummary && s.ms >= 3 * 60000) {
        std::wstring sub;
        if (st.peakHeapMb > 0 && st.xmxMb > 0)
            sub = L"Куча Java до " + Size(st.peakHeapMb) + L" из " + Size(st.xmxMb) + L", память до " +
                  Size(s.peakUsedMb) + L" из " + Size(s.totalMb) + L".";
        else
            sub = L"Память до " + Size(s.peakUsedMb) + L" из " + Size(s.totalMb) + L".";
        if (st.lags) {
            int sec = (int)std::lround(st.lagMs / 1000);
            sub += L" Мир не успевал " + Plural(st.lags, L"раз", L"раза", L"раз") +
                   (sec > 0 ? L" (" + std::to_wstring(sec) + L" с)" : L"") + L".";
        }
        bool tight = s.stallSamples >= 3 || s.pageGrowMb > 512;
        if (tight)
            sub +=
                L" Подкачка +" + Size(s.pageGrowMb) + L", диск мешал " + std::to_wstring(s.stallSamples * 5) + L" с.";
        else if (!st.lags)
            sub += L" Памяти хватало.";
        g_gbCard.Show(BoostCard::DONE, L"Сыграно " + Duration(s.ms) + L": " + in.name, sub, t, 16000,
                      tight || st.lags > 20 ? Toast::G_WARN : Toast::G_INFO);
    }
    if (crashed) return;
    // 3. AlwaysPreTouch держит всю кучу с первой секунды, а сборке столько не
    // нужно
    if (in.preTouch && !in.id.empty() && st.heapSamples >= 2 && st.peakHeapMb > 0 && in.maxMb > 0 &&
        st.peakHeapMb < in.maxMb * 0.7 && McGet(L"pretouch-no:" + in.id).empty()) {
        std::wstring id = in.id, name = in.name;
        double spare = in.maxMb - st.peakHeapMb;
        g_mcCard2.yesLabel = L"Убрать флаг";
        g_mcCard2.noLabel = L"Оставить";
        g_mcCard2.onNo = [id] { McPut(L"pretouch-no:" + id, L"1"); };
        g_mcCard2.onYes = [id, name] {
            std::wstring err;
            bool ok = mc::RemovePreTouch(id, err);
            g_mcCard2.Show(BoostCard::DONE, ok ? L"AlwaysPreTouch убран: " + name : L"Не получилось",
                           ok ? L"Со следующего запуска Java будет занимать память по мере "
                                L"надобности. Копия настроек - instance.cfg.moon-backup."
                              : err,
                           CurrentTheme(g_cfg), 12000, ok ? Toast::G_OK : Toast::G_WARN);
        };
        g_mcCard2.Show(BoostCard::OFFER, L"Убрать AlwaysPreTouch у " + name + L"?",
                       L"Сборке хватало " + Size(st.peakHeapMb) + L" из " + Size(in.maxMb) +
                           L", а флаг забирает всё с первой секунды. Без него "
                           L"Windows останется ещё ~" +
                           Size(spare) + L".",
                       t, 40000, Toast::G_INFO);
        return;
    }
    // 4. подкачка «по выбору системы» росла прямо в игре - раз в неделю
    // предложить постоянный размер
    int said = _wtoi(McGet(L"pagefile-said").c_str());
    if (s.pageGrowMb > 512 && mc::PagefileAuto() && (!said || Today() - said >= 7)) {
        McPut(L"pagefile-said", std::to_wstring(Today()));
        g_mcCard2.yesLabel = L"Открыть";
        g_mcCard2.noLabel = L"Позже";
        g_mcCard2.onNo = nullptr;
        g_mcCard2.onYes = [] {
            ShellExecuteW(nullptr, nullptr, L"SystemPropertiesPerformance.exe", nullptr, nullptr, SW_SHOWNORMAL);
        };
        g_mcCard2.Show(BoostCard::OFFER, L"Подкачка росла прямо в игре",
                       L"+" + Size(s.pageGrowMb) +
                           L" за игру, отсюда фризы. Дополнительно - Виртуальная "
                           L"память - Изменить: на SSD размер "
                           L"16384 МБ, исходный = максимальный.",
                       t, 40000, Toast::G_INFO);
    }
}

std::wstring McNote(const mc::Instance& in) {
    std::wstring n = in.maxMb ? L"Память " + Size(in.maxMb) : L"Память по умолчанию";
    if (in.mods) n += L", " + Plural(in.mods, L"мод", L"мода", L"модов");
    if (in.preTouch) n += L", AlwaysPreTouch";
    return n + L". Сначала освобожу память, потом запущу.";
}

void McLaunchNow(const std::wstring& id, const std::wstring& name) {
    g_offerQuietUntil = GetTickCount64() + 180000;  // игра стартует - второй раз не спрашивать
    if (mc::Launch(id))
        g_mcCard.Show(BoostCard::DONE, L"Запускаю " + name, L"Prism откроется свёрнутым.", CurrentTheme(g_cfg), 4000,
                      Toast::G_OK);
    else
        g_mcCard.Show(BoostCard::DONE, L"Prism не найден",
                      L"Ищу prismlauncher.exe в "
                      L"%LOCALAPPDATA%\\Programs\\PrismLauncher и Program Files.",
                      CurrentTheme(g_cfg), 9000, Toast::G_FAIL);
}

// Сборка из кольца: сначала память (до запуска - пока AlwaysPreTouch ничего не
// забрал), потом запуск.
void McLaunch(const std::wstring& id) {
    mc::Instance in;
    if (!mc::FindInstance(id, in)) return;
    if (g_mcGame.on) {
        g_mcCard.Show(BoostCard::DONE, L"Minecraft уже запущен", g_mcGame.inst.name, CurrentTheme(g_cfg), 4000,
                      Toast::G_WARN);
        return;
    }
    if (g_boostBusy) return;
    std::wstring name = in.name;
    boost::Settings s = boost::Load(BoostDir());
    double avail = boost::AvailMb(), need = (in.maxMb ? in.maxMb : 4096) + 2500.0;  // куча, сама игра и запас
    if (!s.ok || avail >= need) return McLaunchNow(id, name);
    g_boostGroups = boost::Scan(s);
    if (g_boostGroups.empty()) return McLaunchNow(id, name);
    g_pickFor = L"mc:" + Lower(id);
    std::map<std::wstring, int> prof = gboost::Profile(g_pickFor);
    std::vector<BoostPick::Row> rows;
    for (const boost::Group& g : g_boostGroups) {
        BoostPick::Row r{g.title.empty() ? g.name : g.title, Lower(g.name), g.mb, DefaultAct(Lower(g.name))};
        auto it = prof.find(r.key);
        if (it != prof.end()) r.act = it->second;
        rows.push_back(r);
    }
    g_afterBoost = [id, name] { McLaunchNow(id, name); };
    g_pick.onApply = [](const std::vector<BoostPick::Row>& r) { ApplyPick(r); };
    g_pick.onSkip = [] { RunAfterBoost(); };  // «Не сейчас» - всё равно запустить
    g_pick.Show(L"Освободить память для " + name + L"?",
                L"Сборке нужно " + Size(in.maxMb ? in.maxMb : 4096) + L" сразу при запуске, свободно " + Size(avail) +
                    L". Потом запущу её.",
                rows, CurrentTheme(g_cfg));
}

void McAction(const std::wstring& key) {
    if (key.rfind(L"mc:", 0) == 0) return McLaunch(key.substr(3));
    if (key == L"mc-prism") {
        std::wstring exe = mc::PrismExe();
        if (!exe.empty()) ShellExecuteW(nullptr, nullptr, exe.c_str(), nullptr, ParentDir(exe).c_str(), SW_SHOWNORMAL);
    }
}

// Кольцо сборок у курсора: последние запущенные первыми.
void ShowMcRing() {
    if (g_menu.IsOpen()) return;
    std::vector<Item> ring;
    for (const mc::Instance& in : mc::Instances()) {
        if (ring.size() >= 9) break;
        Item it;
        it.name = in.name;
        it.iconFull = in.icon.empty() ? ExeDir() + L"\\icons\\game.png" : in.icon;
        it.special = 3;
        it.path = L"mc:" + in.id;
        it.note = McNote(in);
        ring.push_back(it);
    }
    Item prism;
    prism.name = L"Prism";
    prism.iconFull = ExeDir() + L"\\icons\\apps.png";
    prism.special = 3;
    prism.path = L"mc-prism";
    prism.note = L"Открыть лаунчер, ничего не запуская";
    ring.push_back(prism);
    g_menu.onNet = [](const std::wstring& key) { McAction(key); };
    g_menu.netStatus = [] { return L"Свободно " + Size(boost::AvailMb()); };
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g_menu.ShowNet(&g_cfg, std::move(ring), pt, mi.rcWork, MonitorDpi(mon));
}

// Для проверки без игры: карточки Minecraft на выдуманных данных.
void McDemo(int what) {
    ThemeInfo t = CurrentTheme(g_cfg);
    if (what == 0) {
        ShowMcRing();
    } else if (what == 1) {
        g_mcCard.yesLabel = L"Открыть отчёт";
        g_mcCard.noLabel = L"Закрыть";
        g_mcCard.onYes = nullptr;
        g_mcCard.onNo = nullptr;
        g_mcCard.Show(BoostCard::OFFER, L"Minecraft упал: Beyond Depth",
                      L"Ticking entity: java.lang.NullPointerException. "
                      L"Подозрение: Create (create), Version: 0.5.1.",
                      t, 20000, Toast::G_FAIL);
    } else if (what == 2) {
        g_mcCard2.yesLabel = L"Убрать флаг";
        g_mcCard2.noLabel = L"Оставить";
        g_mcCard2.onYes = nullptr;
        g_mcCard2.onNo = nullptr;
        g_mcCard2.Show(BoostCard::OFFER, L"Убрать AlwaysPreTouch у Beyond Depth?",
                       L"Сборке хватало 5.1 ГБ из 7.9 ГБ, а флаг забирает всё с "
                       L"первой секунды. Без него Windows "
                       L"останется ещё ~2.8 ГБ.",
                       t, 20000, Toast::G_INFO);
    } else {
        g_gbCard.Show(BoostCard::DONE, L"Сыграно 2 ч 5 мин: Beyond Depth",
                      L"Куча Java до 5.1 ГБ из 7.9 ГБ, память до 14.2 ГБ из 15.4 "
                      L"ГБ. Мир не успевал 14 раз (38 с). "
                      L"Подкачка +1.6 ГБ, диск мешал 55 с.",
                      t, 20000, Toast::G_WARN);
    }
}

// Режим разработки включился или выключился: обои и память, красота и перехват
// мыши не трогаются.
void OnDevMode(bool on) {
    UpdateTrayTip();
    autos::OnModes(g_game.On(), on);  // питание: в Unreal - производительность
    toolsui::OnModes(g_game.On(), on);
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
            gboost::Quiet(true);
            if (gboost::QuietOn()) chips.push_back(L"Уведомления молчат");
        }
        if (McBegin()) chips.push_back(L"Prism усыплён");
        bool battery = tools::OnBattery();
        if (g_cfg.autoPower) chips.push_back(battery ? L"От батареи: баланс" : L"Питание: максимум");
        gboost::SetGame(true, g_game.GamePids(), GameExe(), g_game.Reason(), g_cfg.boostIslc);
        if (g_cfg.boostOffer) OfferBoost(false);
        if (battery)
            g_gbCard.Show(BoostCard::DONE, L"Игра от батареи",
                          L"Процессор и видеокарта будут работать медленнее, а заряд "
                          L"уйдёт за час-полтора. С зарядкой "
                          L"режим питания сам станет «максимум».",
                          CurrentTheme(g_cfg), 12000, Toast::G_WARN);
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
        if (g_cfg.gameWallpaper) chips.push_back(L"Обои снова идут");
        if (g_cfg.boostEco) chips.push_back(L"Приоритеты как были");
        if (quiet) chips.push_back(L"Уведомления включены");
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
    for (const crash::Report& r : rs)
        autos::Note(L"Программа падала " + r.when + (r.what.empty() ? L"" : L": " + r.what) + L". Дамп: " + r.dump);
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
                     (last.what.empty() ? L"" : last.what + L". ") + L"Дамп: " + dump + L" (хранятся последние " +
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
    mc::StopWatch();
    netui::Shutdown();
    toolsui::Shutdown();
    cliphist::Stop();
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
        case WM_QUERYENDSESSION:
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

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    crash::Install();  // первым делом: упадёт хоть что-то дальше - останется дамп
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
    gboost::Start(g_wnd);
    g_game.recognize = [](DWORD pid, const std::wstring& path) { return mc::GameName(pid, path); };
    corners::SetMinecraft(ShowMcRing);
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
        [](int, const std::wstring& focus) {
            OpenEditor(g_inst, g_cfgPath, [] { Reload(); }, ShowFromStudio);
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
    cliphist::Start(inst, &g_cfg);
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
