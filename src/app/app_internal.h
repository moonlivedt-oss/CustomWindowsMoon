#pragma once
// Общее для main.cpp (окно программы, опрос, трей, режимы) и вынесенных из него частей: boostflow.cpp (Boost:
// карточки перед игрой и после, итог сессии, новости во время игры) и mcflow.cpp (сборки Minecraft: запуск, падения,
// AlwaysPreTouch, подкачка). Глобальные - в namespace app, определены в main.cpp.
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "desk/battery.h"
#include "desk/boost.h"
#include "desk/boostcard.h"
#include "desk/boostpick.h"
#include "desk/corners.h"
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
#include "menu/menu.h"

namespace app {

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
    CMD_RESTORE_ALL,
    CMD_AFTER_SLEEP,  // Л6: усыпить, когда всё закончится (второй раз - отменить)
    CMD_AFTER_OFF     // Л6: выключить, когда всё закончится
};
constexpr UINT WM_BOOST_DONE = WM_APP + 60;  // поток Boost закончил: lParam - boost::Result*
constexpr UINT WM_DROP_DONE = WM_APP + 71;   // бросок на папку кольца разобран: lParam - DropMsg*
constexpr UINT WM_POLL_WAKE = WM_APP + 73;   // курсор дошёл до края или что-то началось - опрос снова частый

extern HINSTANCE g_inst;
extern HWND g_wnd;
extern UINT g_taskbarCreated;
extern RadialMenu g_menu;
extern corners::HotCorners g_corners;
extern MediaWidget g_media;
extern WindowFx g_winfx;
extern DeskClick g_deskClick;
extern BatteryGlow g_battery;
extern SnipCard g_snip;
extern GameMode g_game;
extern DevMode g_dev;
extern WindowFrame g_frame;
extern lockscreen::Reveal g_reveal;
extern HexWave g_wave;
extern ModeFx g_modefx;  // анимация включения и выключения игрового режима и Unreal
extern BoostCard g_boostCard;
extern BoostPick g_pick;        // выбор по программам: закрыть, усыпить, оставить
extern BoostCard g_gbCard;      // новости Boost во время игры и работы: видеокарта, шейдеры, сборки, итог
extern std::wstring g_pickFor;  // чей профиль выбора: exe игры, редактора или «вручную»
extern ULONGLONG g_avHintAt;
extern BoostCard g_dropCard;           // «Перенесено в ... - Вернуть»
extern BoostCard g_edgeCard;           // что мешает кольцам у края (CursorWrap)
extern BoostCard g_mcCard, g_mcCard2;  // Minecraft: падение / AlwaysPreTouch и подкачка
struct McGame {
    bool on = false;
    DWORD pid = 0;
    mc::Instance inst;
};
extern McGame g_mcGame;
extern std::function<void()> g_afterBoost;  // запуск сборки после «Применить» в карточке выбора
extern ULONGLONG g_offerQuietUntil;         // только что освобождали память перед запуском - при старте не спрашивать
extern std::wstring g_pendingAction;        // действие из кольца (action:...) - после того, как кольцо погаснет
constexpr UINT_PTR kActionTimer = 5;        // когда предложить исключения антивируса для Unreal (0 - не надо)
extern std::vector<boost::Group> g_boostGroups;  // что предложили закрыть
extern std::vector<std::wstring> g_boostClosed;  // что закрыли - вернуть после игры
extern bool g_boostBusy;
extern HPOWERNOTIFY g_displayNotify, g_acdcNotify;
extern ULONGLONG g_cfgCheckAt;
extern ULONGLONG g_guardAt;  // когда последний раз проверяли, не висят ли скрытые окна
extern UINT g_pollMs;
extern Config g_cfg;
extern std::wstring g_cfgPath;
extern FILETIME g_cfgTime;
extern bool g_armed;
extern bool g_restored;  // «Вернуть всё как было»: рамки окон на паузе до следующего перечитывания настроек
extern std::wstring g_themeOverride;  // тема на время правила «если - то» (пусто - своя из radial.cfg)
extern bool g_altTab;                 // за это нажатие Alt был Tab: идёт переключение окон, кольцо у края не открываем

// main.cpp
std::wstring BoostDir();
void UpdateTrayTip();
void MaskAlt();
UINT MonitorDpi(HMONITOR m);

// boostflow.cpp
std::wstring Size(double mb);
std::wstring Names(const std::vector<std::wstring>& names);
void RunAfterBoost();
std::wstring GameExe();
std::wstring EditorExe();
std::vector<std::wstring> KeepApps();
int DefaultAct(const std::wstring& key);
void ApplyPick(const std::vector<BoostPick::Row>& rows);
void OfferBoost(bool manual, const std::wstring& why = L"перед игрой");
void BoostDone(boost::Result* r);
void OfferRestore();
void BoostAfterGame();
std::wstring Duration(ULONGLONG ms);
std::wstring MinSec(double sec);
void SessionSummary(const gboost::Session& s, bool save = true);
void CopyText(const std::wstring& s);
void ShowAvHint();
bool WallpaperPaused(bool cfgFlag);
void OnBoostNote(const gboost::Note& n);
void BoostDemo(int what);

// mcflow.cpp
std::wstring McIni();
std::wstring McGet(const std::wstring& key);
void McPut(const std::wstring& key, const std::wstring& v);
int Today();
bool McBegin();
void McEnd(const gboost::Session& s);
std::wstring McNote(const mc::Instance& in);
void McLaunchNow(const std::wstring& id, const std::wstring& name);
void McLaunch(const std::wstring& id);
void McAction(const std::wstring& key);
void ShowMcRing();
void McDemo(int what);

}  // namespace app
