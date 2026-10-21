#pragma once
// Внутреннее устройство Radial Studio: идентификаторы элементов, класс окна.
#include <atomic>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "desk/lockscreen.h"
#include "net/net.h"
#include "tools/healthscore.h"
#include "tools/tools.h"
#include "core/config.h"
#include "core/themes.h"
#include "ui/ui.h"
#include "gfx/scene.h"
#include "app/toolsui.h"

namespace studio {

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::RectF;

inline constexpr wchar_t kClass[] = L"MoonRadialStudio";
inline constexpr UINT WM_RUN = WM_APP + 40;
inline constexpr UINT_PTR kSaveTimer = 1;

enum Page {
    PAGE_RINGS,
    PAGE_THEMES,
    PAGE_BEHAVIOR,
    PAGE_DESK,
    PAGE_SYSTEM,
    PAGE_STYLE,
    PAGE_NET,
    PAGE_TOOLS,
    PAGE_HOME,
    PAGE_KEYS,
    PAGE_STATE,
    PAGE_AUTO,
    PAGE_APPS,  // Кольца / Программы: кольца программ и рабочие наборы
    PAGE_AIDS   // Настройки / Помощники: палитра, буфер, раскладки, росчерк, сторожа
};
enum Overlay { OV_NONE, OV_TOUR, OV_JOURNAL, OV_LIBRARY, OV_MORE };

// идентификаторы элементов: постоянные и диапазоны для списков
enum Id {
    ID_RAIL = 100,  // +страница
    ID_TOUR_BTN = 120,
    ID_SIDE = 130,
    ID_ADD_SECTOR,
    ID_PROBLEM_FIND,
    ID_PROBLEM_HIDE,
    ID_SHOW_MENU,
    ID_OPEN,
    ID_MORE,
    ID_TABS,
    ID_NAME,
    ID_PATH,
    ID_PICK_FOLDER,
    ID_PICK_FILE,
    ID_EXPAND,
    ID_FILES,
    ID_FIX_THIS,
    ID_ADD_CHILD,
    ID_ICON_FILE,
    ID_ICON_LIB,
    ID_ICON_SYS,
    ID_EMPTY_ADD,
    ID_UNDO,
    ID_JOURNAL,
    ID_OV_BG,
    ID_TOUR_NEXT,
    ID_TOUR_BACK,
    ID_TOUR_SKIP,
    ID_LIB_CLOSE,
    ID_DESKTOP_ONLY,
    ID_SKIP_FULL,
    ID_INNER_EDGE,
    ID_BLUR,
    ID_SCALE,
    ID_MAXCH_MINUS,
    ID_MAXCH_PLUS,
    ID_SOUNDS,
    ID_VOLUME,
    ID_TERMINAL,
    ID_AUTOSTART,
    ID_SETTINGS_EXPORT,
    ID_SETTINGS_IMPORT,
    ID_OPEN_CFG,
    ID_OPEN_DIR,
    ID_LIST_SCROLL,
    ID_MAIN_SCROLL,
    ID_LIB_CATS_SCROLL,
    ID_LIB_GRID_SCROLL,
    ID_JOURNAL_SCROLL,
    ID_PAGE_SCROLL,
    ID_RENAME,
    ID_TOAST_UNDO,
    ID_CORNER_DELAY,
    ID_MEDIA,
    ID_MEDIA_TOAST,
    ID_WINFX,
    ID_SYS_ICONS,
    ID_SYS_ICONS_APPLY,
    ID_SYS_ICONS_BACK,
    ID_SYS_NOTIFY,
    ID_SYS_ALL,
    ID_SYS_EXPLORER,
    ID_SYS_START,
    ID_SYS_CURSOR_SIZE,
    ID_SYS_BATTERY,
    ID_SYS_BATTERY_LOW,
    ID_SYS_BATTERY_TEST,
    ID_SYS_BATTERY_WAVE,
    ID_SYS_NIGHT,
    ID_SYS_NIGHT_FROM_M,
    ID_SYS_NIGHT_FROM_P,
    ID_SYS_NIGHT_TO_M,
    ID_SYS_NIGHT_TO_P,
    ID_SYS_DESK_DOUBLE,
    ID_SYS_WIN_RING,
    ID_SYS_SNIP,
    ID_STYLE_MOTION,
    ID_STYLE_FRAME,
    ID_STYLE_CAPTIONS,
    ID_GAME_AUTO,
    ID_GAME_ANIM,
    ID_GAME_TRANSP,
    ID_GAME_WALL,
    ID_GAME_LIST,
    ID_DEV_AUTO,
    ID_DEV_WALL,
    ID_DEV_BOOST,
    ID_DEV_LIST,
    ID_LOCK_APPLY,
    ID_LOCK_PICK,
    ID_LOCK_WALL,
    ID_LOCK_REVEAL,
    ID_LOCK_DEMO,
    ID_LOCK_ANIM,
    ID_LOCK_PATTERN,
    ID_HEX_GLASS,
    ID_HEX_BOOT,
    ID_HEX_WAKE,
    ID_HEX_WAVE,
    ID_BOOST_OFFER,
    ID_BOOST_RESTORE,
    ID_BOOST_AFTER,
    ID_BOOST_TASK,
    ID_BOOST_NOW,
    ID_BOOST_LIST,
    ID_BOOST_DIR,
    ID_BOOST_ECO,
    ID_BOOST_QUIET,
    ID_BOOST_ISLC,
    ID_BOOST_GPU,
    ID_BOOST_SUMMARY,
    ID_DEV_WATCH,
    ID_LOGON_AVATAR,
    ID_LOGON_AVATAR_PICK,
    ID_LOGON_AVATAR_EMBLEM,
    ID_LOGON_CRISP,
    ID_NET_CHECK,
    ID_NET_SMART,
    ID_NET_PROXY,
    ID_NET_VPN,
    ID_NET_IP,
    ID_NET_ADAPTER,
    ID_NET_FW_RESTORE,
    ID_NET_DEEP,
    ID_NET_DNS,
    ID_NET_DNS_TEST,
    ID_NET_ADMIN,
    ID_NET_WATCH,
    ID_NET_EXIT,
    ID_NET_GAME,
    ID_NET_INTERVAL,
    ID_TOOLS_TAB,
    ID_TOOLS_ADD,
    ID_TOOLS_ADD_BTN,
    ID_TOOLS_CLEAN,
    ID_TOOLS_SYSCLEAN,
    ID_TOOLS_RESTORE,
    ID_TOOLS_ENV_NAME,
    ID_TOOLS_ENV_VALUE,
    ID_TOOLS_ENV_SET,
    ID_TOOLS_HEALTH,
    ID_TOOLS_SCAN_PATH,
    ID_TOOLS_SCAN,
    ID_TOOLS_SCAN_UP,
    ID_TOOLS_MFT_SETUP,
    ID_TOOLS_RING,
    ID_TOOLS_JUNK_SCAN,
    ID_TOOLS_LOCK_PATH,
    ID_TOOLS_LOCK,
    ID_TOOLS_LOCK_CLIP,
    ID_TOOLS_SYS_TOGGLE,
    ID_STATE_REFRESH,
    ID_STATE_CLEAN,
    ID_STATE_DDC,
    ID_STATE_SYSCLEAN,
    ID_STATE_PROJ_DEL,
    ID_STATE_PROJ_IDLE,
    ID_STATE_DL_DEL,
    ID_STATE_DL_ALL,
    ID_STATE_DRIVER,
    ID_STATE_AUTOSTART,
    ID_STATE_GO_AUTO,
    ID_AUTO_CLEAN,
    ID_AUTO_CLEAN_NOW,
    ID_AUTO_DL,
    ID_AUTO_DL_DAYS,
    ID_AUTO_DL_NOW,
    ID_AUTO_POWER,
    ID_AUTO_DIGEST,
    ID_AUTO_DIGEST_HOUR,
    ID_AUTO_REPAIR,
    ID_LIVE_ROUTE,   // живые сцены (studio_live.cpp): маршрут сети
    ID_LIVE_PORTS,   // кабели портов
    ID_LIVE_START,   // дорожка входа
    ID_LIVE_FLASK,   // колба во вкладке «Процессы»
    ID_LIVE_FLASK2,  // колба на «Состоянии»
    ID_LIVE_BATTERY,
    ID_LIVE_VERS,
    ID_TOOLS_VER_CHECK,
    ID_TOOLS_SEC_KIND,    // вкладка «Защита»: фильтр по виду находок
    ID_TOOLS_SEC_RESCAN,  // «Проверить заново»
    ID_TOOLS_SEC_UNHIDE,  // «Показать скрытые»
    ID_TOOLS_SIGN,        // «Настроить подпись»
    ID_LANE_BIN,          // «Кольца»: корзина под дорожками
    ID_TAPE,              // лента правок
    ID_ICON_DOCK,         // сетка значков-дока (наведение)
    ID_FLOW_FILL,         // «Наполнить из папки»
    ID_FLOW_FILES,        // «И обычные файлы»
    ID_APP_NEW,           // «Программы»: поле своей программы
    ID_APP_NEW_BTN,
    ID_APP_PRESET = 11800,   // +i: заготовка кольца программы
    ID_APP = 12000,          // +кольцо*100 + поле (studio_apps.cpp)
    ID_AID = 16000,          // +поле: «Помощники» (studio_aids.cpp)
    ID_SET_NEW = 16490,      // «Программы»: новый рабочий набор
    ID_SET = 16500,          // +набор*100 + поле (studio_apps.cpp)
    ID_TOOLS_FIND = 18590,   // «Диски»: поиск папки по таблице MFT
    ID_TOOLS_FOUND = 18600,  // +i*2: «Посчитать» и «Открыть» у найденной папки
    ID_TOOLS_BIG = 18700,    // +i*2: «Показать» и «В Корзину» у большого файла
    ID_NET_TOP = 18900,      // «Сеть»: «Замерить» (+0), «Отрезать» у программы (+1+i), «Вернуть» (+20+i), +50 - данные Windows
    ID_TOOLS_PRIV = 19100,   // +i: переключатель приватности; +60 - «Вернуть всё как было»
    ID_TOOLS_GIANT = 18800,  // +i: действие у системного гиганта; +40 - «Точно, с правами», +41 - вернуть гибернацию
    ID_BACKUP = 7000,        // +i
    ID_NET_KILL = 8000,      // +i: «Завершить» у прокси-процесса
    ID_TOOLS_ROW = 8100,     // +запись*4: кнопки у записи PATH
    ID_TOOLS_SIZE = 8400,    // +i*2: «Внутрь» и «Открыть» в «Что занимает место»
    ID_TOOLS_JUNK = 8450,    // +i: «Очистить» у кэша
    ID_TOOLS_START = 8500,   // +i: переключатель автозагрузки
    ID_TOOLS_PORT = 8600,    // +i: «Завершить» у порта
    ID_TOOLS_LOCKER = 8700,  // +i: «Завершить» у того, кто держит файл
    ID_TOOLS_SEC = 8800,     // +i*2: «Что делать» и «Это моё» у находки «Защиты»
    ID_SECTOR = 1000,        // +i
    ID_CHILD = 2000,         // +j*8 + поле
    ID_THEME = 3000,         // +i
    ID_CUSTOM = 3100,        // +цвет
    ID_CORNER = 3200,        // +угол*4 + действие
    ID_SYS_CURSOR = 3300,    // +образец цвета
    ID_GRID_ICON = 4000,     // +i
    ID_LIB_CAT = 5000,       // +i
    ID_LIB_ICON = 6000,      // +i
    ID_MORE_ITEM = 9000,     // +пункт
    ID_RAIL_BAR = 9500,      // +страница: анимация полоски выбранного раздела
    ID_RAIL_HOVER = 9600,    // +страница: анимация наведения на раздел
    ID_SUBTAB = 9700,        // +страница: вкладка внутри группы
    ID_HOME_TILE = 9800,     // +i: предмет карты экрана «Обзора» (+50 - его анимация)

    ID_STATE_PROJ = 10000,  // +i: строка мусора проектов (выбор)
    ID_STATE_DL = 10100,    // +i: строка Загрузок (выбор)
    ID_STATE_APP = 10200,   // +i: «Закрыть» у тяжёлой программы
    ID_STATE_ANIM = 10300,  // +i: появление шкал и полос
    ID_PATH_SUGG = 10400,   // +i: подсказка похожей папки под путём
    ID_LANE_FIND = 10500,   // +край*100 + i: «найти» на карточке с пропавшим путём
    ID_LANE_MINI = 10700,   // +j: пункт раскрытой стопки
    ID_FLOW = 10800,        // +n: плашка второго кольца (щелчок - закрепить)
    ID_LANE_ANIM = 11000,   // +край*100 + i: раскрытие стопки
};

// Пружина раздела «Кольца» (studio_lanes.cpp): карточка, пункт веера, плашка второго кольца, значок дока.
// Координаты - в своей рамке (дорожек, потока, сетки): при прокрутке рамка едет целиком, пружина не тянется.
struct Mover {
    float x = 0, y = 0, vx = 0, vy = 0;
    float a = 1;              // видимость
    float pop = 0;            // подпрыгнуть: 1 - только что, тает
    float crack = 0;          // трещина пропавшего пути
    float s = 1;              // масштаб (док значков)
    float wait = 0;           // задержка перед стартом, с
    float pull = 0;           // притяжение к квадрату значка
    int ring = -1, idx = -1;  // где стоял в прошлом кадре
    bool live = false;
};
struct Crumb {  // крошка рассыпавшейся карточки
    float x, y, vx, vy, hx, hy, delay;
    BYTE col;
};
struct Sink {  // карточка, которая слиплась с другой: сжимается и тонет в ней
    Item it;
    std::wstring into;
    float x, y, t;
};

struct Problem {
    int ring, i, j;  // j = -1 - сам сектор
    std::wstring path;
    int kind = 0;  // 0 - путь пропал, 1 - ничего не открывает, 2 - раскрывается, но не папка
};

// действия над выбранным сектором: из меню по ПКМ, кнопки «…» и с клавиатуры
enum Op { OP_RENAME, OP_UP, OP_DOWN, OP_OTHER_SIDE, OP_DUPLICATE, OP_DELETE };

inline constexpr UINT_PTR kTipTimer = 2, kToastTimer = 3, kBusyTimer = 4;

struct FolderStats {
    int total = 0, dirs = 0;
};

// Сборка темы значков идёт в своём потоке; студия только смотрит на прогресс.
struct IconJob {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<int> progress{0};
    std::atomic<bool> done{false};
    bool ok = false;
    std::wstring out, message, previous;
    COLORREF body = 0, accent = 0;
    std::wstring name;
    HWND wnd = nullptr;
};

// Работа раздела «Сеть» в своём потоке: журнал и отчёт под замком, страница только читает.
struct NetJob {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<bool> done{false};
    bool ok = false, hasReport = false;
    net::Report report;
    std::vector<std::wstring> log;
    std::wstring title;
    std::function<bool(const net::Log&, NetJob&)> work;
    HWND wnd = nullptr;
};

// Работа раздела «Утилиты» в своём потоке; что успело посчитаться - под замком.
struct ToolsJob {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<bool> done{false}, cancel{false};
    std::atomic<long> progress{0};
    unsigned sizesGen = 0;                // растёт под lock при каждой правке sizes: кадр копирует только новое
    std::atomic<bool> mftReading{false};  // задача с правами читает таблицу MFT
    std::atomic<int> mftState{-1};        // tools::MftState, если подсчёт спрашивал задачу с правами
    bool ok = false, hasSizes = false, hasJunk = false, hasPhys = false;
    std::vector<tools::FolderSize> sizes;
    std::vector<tools::Junk> junk;
    std::vector<tools::Physical> phys;
    std::vector<std::wstring> log;
    std::wstring title;
    std::function<bool(const tools::Log&, ToolsJob&)> work;
    HWND wnd = nullptr;
};

// Замеры звеньев маршрута для живой сцены сети: свой поток, пока раздел «Сеть» на экране.
struct NetSampler {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<std::vector<float>> hist;  // ПК-роутер, роутер-выход, выход-DNS, DNS-интернет; мс, -1 - нет замера
    std::wstring gateway, adapter, country;
    bool tunnel = false, gwDown = false, httpDown = false;
    int gen = 0;
    std::atomic<ULONGLONG> seen{0};  // когда страница последний раз смотрела; давно - поток выходит
    std::atomic<bool> running{false};
};

// Картинка темы, которая сменяется растворением с медленным наездом (обои «Обзора», витрина «Тем»).
struct PicFade {
    ThemeInfo cur{}, prev{};
    std::wstring key;
    double at = -10;  // когда пришла новая (clock_)
};

void Preview(int what);                                // studio.h: EditorPreview
std::wstring GameStatus();                             // studio.h: SetEditorGameStatus
void GameToggle();                                     // studio.h: SetEditorGameToggle
const wchar_t* ThemeTagline(const std::wstring& key);  // настроение темы одной строкой (studio_themes.cpp)
std::wstring DevStatus();                              // studio.h: SetEditorDevStatus

class Studio {
public:
    HWND wnd = nullptr;
    HINSTANCE inst = nullptr;
    std::wstring cfgPath;
    std::function<void()> onSaved;
    std::function<void(bool, HWND)> showMenu;
    Config cfg;
    ui::Ui ui;
    double k = 1;

    void Open();
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);

private:
    // данные
    std::vector<Item>& Ring(int r) { return r == 0 ? cfg.left : cfg.right; }
    Item* Sel();
    void PushUndo();
    void Change();
    void FieldEdit(int fid);
    void Save();
    void Undo();
    void Redo();
    void Toast(const std::wstring& s);
    void DrawToast(float w, float h);
    void SectorOp(int op);
    void Backup();
    std::vector<std::wstring> Backups();
    void Restore(const std::wstring& file);
    void Log(const std::wstring& s);
    void Recheck();
    void FixProblem(const Problem& p);
    void ApplyAccent();
    FolderStats Stats(const Item& it);
    std::wstring IconFile(const Item& it) const;
    void Defer(std::function<void()> f);
    void AddPaths(const std::vector<std::wstring>& paths, bool asChildren);
    std::wstring PickPath(bool folder);
    bool PickColor(COLORREF& c);

    // рисование
    void Paint();
    // Пока что-то считается в фоне: следующий кадр через ms, а не сразу (иначе кадры идут без передышки
    // и съедают процессор, пока сканируется диск).
    void RepaintIn(UINT ms);
    void LoadPhysAsync();  // здоровье дисков (WMI) - в своём потоке: COM в окне рисования прокачивает сообщения
    void Frame(float w, float h);
    void Rail(const RectF& r);
    void StatusBar(const RectF& r);
    void RingsPage(const RectF& r);
    void SectorList(const RectF& r);
    void SectorMain(const RectF& r);
    // живой раздел «Кольца» (studio_lanes.cpp, studio_ringparts.cpp)
    void LaneTick();
    void LaneCard(const Item& it, int L, int i, const RectF& r, const std::wstring& name, float hot, bool on, int prob,
                  float crack, bool smooth);
    void LaneCrumble(int L, int i);
    void CrumbsDraw();
    void MorphGhost();
    void Tape(const RectF& r);
    bool DropOnLanes(POINT pt, const std::vector<std::wstring>& paths);
    bool LaneTarget(int& lane, int& at) const {
        lane = laneHot_, at = laneIns_;
        return laneHot_ >= 0;
    }
    struct Arrival {
        float dy = 0, veil = 0;
    };
    Arrival Arrive(int n);
    void BlockIn(int n);
    void BlockOut(const RectF& r);
    void IconDrawn(const std::wstring& file, const std::wstring& shell, const RectF& r, float alpha = 1);
    void IconSwap(const std::wstring& oldFile);
    float PathChain(float x, float y, float w);
    float RingFlow(const RectF& view, float x, float y, float w);
    float IconDock(const RectF& view, float x, float y, float w);
    void TabBasic(const RectF& view, float y, float& bottom);
    void TabRing(const RectF& view, float y, float& bottom);
    void TabIcon(const RectF& view, float y, float& bottom);
    void ThemesPage(const RectF& r);
    void ThemeHero(const RectF& hero, const ThemeInfo& t, bool trying);
    void ThemeFilm(const RectF& r);  // лента тем доком: пейзажи, за чертой «Из обоев» и «Своя палитра»
    void FilmCard(const ThemeInfo& t, const RectF& cr, bool on);
    void ThemeInAction(const RectF& r);  // кусочек студии, окно и сообщение в цветах студии (она в цветах темы)
    // картинка темы со сменой растворением; zoom - наезд новой, scale - общий масштаб (включение экрана)
    bool ThemeFade(PicFade& f, const ThemeInfo& t, const RectF& r, float radius, float zoom, float scale = 1);
    void Fading(const RectF& r, float alpha, float scale, float dy, const std::function<void()>& draw);
    // мягкая тень или свечение вокруг скруглённой рамки: размытая картинка готовится один раз
    void Halo(const RectF& r, float radius, Color col, float spread, float dy = 0);
    // кусок кадра, который меняется только со своим ключом: рисуется один раз в свою картинку, дальше копируется
    void Baked(const wchar_t* slot, const std::wstring& key, const RectF& area, Color bg,
               const std::function<void()>& draw);
    std::wstring ArtKey(const ThemeInfo& t);      // всё, от чего зависит картинка темы: для ключей Baked
    std::wstring UiKey(const UiColors& c) const;  // цвета студии одной строкой: для ключей Baked
    RectF HomeWindowRect(const RectF& scr, float taskH) const;
    ThemeInfo ThemeAs(const wchar_t* key) const;  // тема с подставленными цветами обоев и своей палитры
    ThemeInfo ShownTheme() const;                 // выбранная или примеряемая наведением
    void ThemeWave(const std::wstring& key, Gdiplus::PointF from);  // выбрать тему волной от точки
    UiColors TryoutPalette();
    void WaveGrab(float w, float h);
    void WaveComposite(float w, float h);
    void ThemePicture(const ThemeInfo& t, const RectF& r, float radius, float alpha);
    void HeroText(const RectF& hero, const ThemeInfo& t, bool trying, float alpha);
    void Scrim(const RectF& clip, float radius, const RectF& band, Color from, Color to, bool horizontal);
    Bitmap* ThemeArt(const ThemeInfo& t, int maxSide);
    void BehaviorPage(const RectF& r);
    void DeskPage(const RectF& r);
    void SystemPage(const RectF& r);
    void StylePage(const RectF& r);
    void NetPage(const RectF& r);
    void HomePage(const RectF& r);
    // «Обзор / Разделы» - карта экрана (studio_home.cpp)
    void HomeEdge(int side, const RectF& scr, double t);
    void HomeCorners(const RectF& scr, float taskH, double t);
    void HomeWindow(const RectF& scr, float taskH, double t);
    void HomeTaskbar(const RectF& scr, float taskH, double t);
    void HomeShelf(const RectF& r, double t);
    void HomeBadge(float cx, float cy, int n, float scale);
    void HomeTipAt(const RectF& anchor, const std::wstring& title, const std::wstring& sub, bool warn, int place);
    void HomeTipDraw(const RectF& page);
    void StatePage(const RectF& r);    // «Состояние»: шкалы, что освободить, проекты, Загрузки, рост, система
    void AutoPage(const RectF& r);     // «Автоматика»: переключатели и журнал
    float SubTabs(const RectF& body);  // вкладки группы над страницей; сколько места заняли
    void ToolsPage(const RectF& r);
    void ToolsRun(const std::wstring& title, std::function<bool(const tools::Log&, ToolsJob&)> work);
    void ToolsScan(const std::wstring& dir, bool setup = false);  // setup - сперва настроить задачу с правами
    void NetCheck();
    void NetRun(const std::wstring& title, std::function<bool(const net::Log&, NetJob&)> work);
    void CornerMenu(int corner, int kind);  // kind: 0 задержка, 1 двойной толчок, 2 колесо
    void SetCorner(int corner, int kind, const std::wstring& val);
    bool CaptureKey(WPARAM vk);
    bool CaptureAppKey(WPARAM vk);  // сочетание для команды кольца программы
    void AppsPage(const RectF& r);
    void SetsCards(class CardColumns& cols, const RectF& view);  // рабочие наборы на вкладке «Программы»
    void AidsPage(const RectF& r);
    void KeysPage(const RectF& r);
    void Header(const RectF& r, const std::wstring& title, const std::wstring& sub);
    RectF Column(const RectF& in, float maxW) const;  // полоса шириной не больше maxW по центру in
    void Card(const RectF& r, bool hi = false);
    void Mascot(const RectF& r, const std::wstring& file, const std::wstring& title, const std::wstring& text);
    void TourOverlay(float w, float h);
    void JournalOverlay(float w, float h);
    void LibraryOverlay(float w, float h);
    void MoreOverlay();
    Bitmap* Img(const std::wstring& path, int maxSide, const std::wstring& tag = L"");  // tag - часть ключа кэша
    bool FieldOrText(int id, const RectF& r, const RectF& view, const std::wstring& value,
                     std::function<void(const std::wstring&)> onChange, const std::wstring& ph = L"");

public:
    // для проверки без окна: нарисовать раздел в PNG (скрытое окно, ms - сколько дать фоновым задачам)
    int Snapshot(int page, int tab, const std::wstring& out, int ms);
    void GoPage(Page p);
    void GoTools(int tab, const std::wstring& scan);
    void GoState(const std::wstring& focus);

private:
    Page page_ = PAGE_STATE;
    Page groupLast_[5] = {PAGE_STATE, PAGE_RINGS, PAGE_DESK, PAGE_NET, PAGE_BEHAVIOR};  // последняя вкладка группы
    ULONGLONG homeAt_ = 0;
    std::vector<tools::Volume> homeVols_;
    int homePathBad_ = 0;
    bool homeAuto_ = false;
    double homeBoot_ = -1;                        // когда экран включился (clock_); -1 - включится в следующем кадре
    float homeOpen_[2] = {};                      // ручки краёв: насколько выехали значки
    double homeHealAt_[2] = {-10, -10};           // ручка вспыхнула зелёным после починки
    double homePathAt_ = -10, homeAutoAt_ = -10;  // метка PATH сжалась, метка автозапуска выскочила
    bool homeAutoWas_ = true;
    Mover homeThumbs_[8];   // подъём миниатюр тем в окне (x: 0..1)
    int homePathGone_ = 0;  // сколько было лишних в PATH: метка сжимается с этим числом
    PicFade homeWall_;      // обои монитора
    struct HomeTip {
        std::wstring title, sub;
        bool warn = false;
        RectF at;
        int place = 0;  // 0 справа от рамки, 1 слева, 2 над ней
        float a = 0;
        bool want = false;
    } homeTip_;
    // «Состояние»: живые данные (не чаще раза в 3 с), выбор строк, к какой карточке прокрутить
    ULONGLONG stateAt_ = 0, statePhysAt_ = 0;
    std::vector<tools::Volume> stateVols_;
    std::vector<tools::Forecast> stateFc_;  // прогноз места (И10), раз в минуту
    ULONGLONG stateFcAt_ = 0;
    tools::MemoryState stateMem_;
    std::vector<tools::HeavyApp> stateApps_;
    toolsui::State stateSnap_;  // последний снимок подсчёта: вектора копируются, только когда он поменялся
    unsigned stateSnapGen_ = ~0u;
    std::vector<tools::DownloadJunk> stateDls_;  // разбор Загрузок: тоже копия по номеру изменения
    unsigned stateDlsGen_ = ~0u;
    std::vector<tools::FolderSize> toolsSizes_;  // «Что занимает место»: копия job->sizes по номеру изменения
    std::weak_ptr<ToolsJob> toolsSizesJob_;      // чья копия: новая задача - копировать заново
    unsigned toolsSizesGen_ = ~0u;
    std::vector<tools::Physical> statePhys_;
    struct PhysJob {
        SRWLOCK lock = SRWLOCK_INIT;
        std::atomic<bool> done{false};
        std::vector<tools::Physical> disks;
    };
    std::shared_ptr<PhysJob> physJob_;
    ULONGLONG busyDue_ = 0;                       // когда сработает таймер следующего кадра (RepaintIn)
    bool painting_ = false, paintAgain_ = false;  // кадр уже рисуется (вложенный WM_PAINT из COM)
    std::set<std::wstring> projSel_, dlSel_;
    std::wstring stateFocus_;
    float stateFocusY_ = -1;
    Overlay ov_ = OV_NONE;
    int ring_ = 0, sel_ = 0, tab_ = 0, tourStep_ = 0;
    int editingField_ = 0;
    int captureCorner_ = -1, captureKind_ = 0;  // ждём сочетание клавиш для угла
    int captureApp_ = -1, captureCmd_ = -1;     // ждём сочетание для команды кольца программы
    std::wstring appNewExe_;                    // «Программы»: exe своей программы
    std::shared_ptr<IconJob> iconJob_;
    std::wstring sysStatus_;
    std::shared_ptr<lockscreen::Job> lockJob_;
    std::wstring logonStatus_;
    std::wstring boostStatus_;
    std::shared_ptr<NetJob> netJob_;
    std::shared_ptr<net::Report> netReport_;
    std::vector<net::ProxyProc> netProcs_;
    std::vector<net::CutApp> netCut_;  // отрезанные программы (И16), из кэша netui
    ULONGLONG netProcsAt_ = 0, netHistAt_ = 0;
    std::vector<std::pair<std::wstring, std::wstring>> netHist_;
    int toolsTab_ = 0;
    ULONGLONG toolsAt_ = 0;
    std::shared_ptr<ToolsJob> toolsJob_;
    std::vector<tools::PathEntry> toolsPath_;
    std::vector<tools::Which> toolsWhich_;
    std::vector<tools::EnvVar> toolsEnv_;
    std::vector<tools::Volume> toolsVols_;
    std::vector<tools::Physical> toolsPhys_;
    std::vector<tools::Junk> toolsJunk_;
    std::vector<tools::StartItem> toolsStarts_;
    std::vector<tools::Tweak> toolsPriv_;  // вкладка «Приватность»
    tools::HealthOut stateHealth_;         // «Состояние»: оценка 0-100 и за что снято
    std::wstring ruleDraft_;               // «Помощники», «Если - то»: правило, что ещё набирают
    std::vector<tools::Listener> toolsPorts_;
    std::vector<tools::Locker> toolsLockers_;
    bool toolsLockChecked_ = false, toolsJunkAsked_ = false, toolsShowSys_ = false;
    std::wstring toolsAddPath_, toolsEnvName_, toolsEnvValue_, toolsScanPath_, toolsLockPath_;
    std::wstring toolsFind_;  // поиск папки по таблице MFT (И8)
    std::vector<tools::FoundDir> toolsFound_;
    ULONGLONG toolsFindAge_ = 0;
    bool toolsFindAny_ = true;              // таблицы MFT вообще есть
    std::vector<tools::BigFile> toolsBig_;  // большие файлы томов (И9)
    ULONGLONG toolsBigAt_ = 0, toolsBigAge_ = 0;
    std::shared_ptr<std::atomic<int>> boostTask_;  // задача очистки: 0 нет, 1 есть, 2 создаётся; общая с потоком
    std::wstring boostDir_;
    bool boostPurge_ = true;
    ULONGLONG boostReadAt_ = 0;
    bool savePending_ = false;
    std::vector<Config> undo_, redo_;
    std::vector<std::wstring> undoWhat_, redoWhat_;  // что сделала каждая правка: подписи ленты правок
    bool tapeQuiet_ = false;
    std::wstring toastBtn_ = L"Отменить";
    // раздел «Кольца» живой
    std::map<std::wstring, Mover> laneCards_, laneMinis_, laneHints_, flowChips_, dockTiles_;
    std::vector<Crumb> crumbs_;
    std::vector<Sink> sinks_;
    bool crumbsBack_ = false, overBin_ = false;
    double clock_ = 0, crumbsAt_ = -10, binPopAt_ = -10, morphAt_ = -10, arriveAt_ = -10, typingAt_ = -10;
    double chainAt_ = -10, chainCheck_ = -10, flowAt_ = -10, iconSwapAt_ = -10;
    ULONGLONG clockAt_ = 0;
    float ldt_ = 0.016f, laneContentH_ = 0, lanePitch_ = 0, laneCardW_ = 0, laneCardH_ = 0;
    std::wstring binKey_, pressKey_, openStack_, mergeKey_, lastShown_, chainKey_, flowKey_, iconOld_;
    RectF binRect_, lanesView_, laneRect_[2], morphFrom_, headIcon_, headTitle_;
    Gdiplus::PointF lanesOrg_, grab_;
    std::vector<float> laneSlotY_[2];
    int laneHot_ = -1, laneIns_ = -1, typingRing_ = -1, typingIdx_ = -1, dockDrag_ = -1;
    int lastDragL_ = -1, lastDragI_ = -1, lastDragJ_ = -1;  // кого тянули в прошлом кадре: в кадре отпускания
    Mover tapeKnob_;
    Arrival arr_;
    bool arrWas_ = true;
    std::vector<std::wstring> chainParts_, chainSugg_;
    int chainBad_ = -1;
    int renaming_ = -1;  // сектор, который переименовывают прямо в списке
    bool renameFocus_ = false, renameActive_ = false, focusPath_ = false, moreAtMouse_ = false;
    RectF sideRect_[2], listView_;
    float listOff_ = 0, itemH_ = 0;
    int childDrag_ = -1, childDragShown_ = -1;
    std::wstring toast_;
    ULONGLONG toastUntil_ = 0, pageAt_ = 0, lastBackup_ = 0;
    // появление страницы: кадр рисуется в свой холст, карточки выезжают из него по очереди
    HDC revealDC_ = nullptr;
    HBITMAP revealBmp_ = nullptr;
    HGDIOBJ revealOld_ = nullptr;
    int revealW_ = 0, revealH_ = 0;
    void Reveal(const RectF& r) { ui.MarkCard(r); }  // карточка выезжает отдельно от страницы
    void RevealComposite(const RectF& body, ULONGLONG since);
    int tabsGroup_ = -1;  // группа, чьи вкладки рисовались в прошлом кадре
    int SizeRing(const RectF& r, const std::wstring& dir, const std::vector<tools::FolderSize>& sizes, bool scanning,
                 long seen, int& hot);     // кольцо долей «Что занимает место»
    int pathDrag_ = -1, pathDragTo_ = -1;  // своя строка PATH, которую тянут, и куда её поставят
    float pathGrab_ = 0;
    std::map<std::wstring, float> ringShown_;
    std::wstring ringDir_;
    ULONGLONG ringAt_ = 0;
    // соты места: у каждой своя пружина появления, подъём под курсором и цвет, который перетекает к доле
    struct HiveCell {
        float s = 0, vs = 0, lift = 0, x = 0, y = 0;
        int part = -1;
        Gdiplus::Color col;
    };
    std::vector<HiveCell> hive_;
    struct HiveGhost {  // соты прошлой папки: разлетаются, а та доля, куда зашли, втягивается в середину
        float x, y, vx, vy;
        Gdiplus::Color col;
        bool in;
    };
    std::vector<HiveGhost> hiveGhosts_;
    ULONGLONG hiveBorn_ = 0, hiveGone_ = 0;
    int hiveZoom_ = -1;             // доля, в которую зашли
    scene::CleanScene cleanScene_;  // вихрь очистки: «Состояние» и «Кэши»
    void CleanSceneIn(const RectF& r);
    bool netWasBusy_ = false;
    ULONGLONG netDoneAt_ = 0;  // когда пришёл итог проверки: узлы схемы загораются по очереди
    // живые сцены части 2 (studio_live.cpp): сеть, порты, автозагрузка, память, батарея, версии
    scene::RouteScene routeScene_;
    scene::PortsScene portsScene_;
    scene::StartScene startScene_;
    scene::FlaskScene flaskScene_, flaskState_;  // во вкладке «Процессы» и на «Состоянии» - у каждой своя жидкость
    scene::BatteryScene batScene_;
    scene::VersScene versScene_;
    std::shared_ptr<NetSampler> sampler_;
    std::vector<tools::ToolVersion> versions_;
    bool versionsBusy_ = false;
    toolsui::SecurityView secView_;  // вкладка «Защита»
    int secKind_ = 0;                // фильтр: 0 - все, дальше - виды, что есть в находках
    ULONGLONG liveAt_ = 0;           // данные колбы и батареи - не чаще раза в 3 с
    tools::MemoryState liveMem_;
    std::vector<tools::HeavyApp> liveApps_;
    tools::BatteryInfo liveBat_;
    toolsui::Watched liveWatch_;
    scene::Palette ScenePalette();
    scene::Input SceneInput(int id, const RectF& r);
    void RouteLive(const RectF& r, bool busy);
    void PortsLive(const RectF& r);
    void StartLive(const RectF& r);
    void FlaskLive(scene::FlaskScene& s, int id, const RectF& r);
    void BatteryLive(const RectF& r);
    void VersLive(const RectF& r);
    void LiveData();
    static bool Cable(const tools::Listener& l);  // порт в кабелях: сервер проекта или прокси-клиент
    static float RouteH(float k) { return scene::RouteScene::Height(k); }
    int railPop_ = -1;  // значок группы, которую только что выбрали, подпрыгивает
    ULONGLONG railPopAt_ = 0;
    std::wstring hoverTheme_, hoverThemeNext_;  // примерка темы наведением
    // витрина тем: прежняя тема растворяется в новую
    std::map<std::wstring, std::unique_ptr<Bitmap>> halos_;  // готовые тени и свечения (Halo)
    struct BakedPart {
        std::wstring key;
        std::unique_ptr<Bitmap> bmp;
    };
    std::map<std::wstring, BakedPart> baked_;  // по месту на странице (Baked)
    ULONGLONG wallAt_ = 0;                     // путь и время записи обоев - не чаще раза в секунду (ThemeArt)
    std::wstring wallPath_, wallStamp_;
    bool homeHaloBaked_ = false;  // тень и свечение окна темы уже в готовой картинке монитора
    PicFade heroPic_;
    float filmDock_[16] = {};  // лента тем: увеличение карточек под курсором
    ThemeInfo heroCur_{}, heroPrev_{};
    std::wstring heroKey_;
    bool heroTrying_ = false, heroPrevTrying_ = false;
    ULONGLONG heroAt_ = 0;
    UiColors curUi_{};
    bool curUiReady_ = false;
    // смена темы волной: прежний кадр в своём холсте уходит под растущий круг
    HDC waveDC_ = nullptr;
    HBITMAP waveBmp_ = nullptr;
    HGDIOBJ waveOld_ = nullptr;
    int waveW_ = 0, waveH_ = 0;
    bool waveGrab_ = false;
    ULONGLONG waveStart_ = 0;
    Gdiplus::PointF waveFrom_;
    ULONGLONG lastPaint_ = 0;
    std::vector<std::pair<std::wstring, std::wstring>> log_;
    std::wstring status_ = L"Готово";
    bool statusFresh_ = false;
    std::vector<Problem> problems_;
    bool problemsHidden_ = false;
    std::map<std::wstring, FolderStats> stats_;
    std::map<std::wstring, std::vector<Item>> listings_;  // содержимое папок для вкладки «Второе кольцо»
    std::vector<std::wstring> programIcons_;              // icons\*.png, читаются один раз
    const std::vector<Item>& Listing(const Item& it);
    std::map<std::wstring, std::unique_ptr<Bitmap>> imgs_;
    std::vector<std::function<void()>> deferred_;
    RectF iconTile_, childArea_, moreAnchor_;
    float mainContentH_ = 0, pageContentH_ = 0;
    std::map<std::wstring, float> cardGrow_;  // насколько карточки выросли под длинный текст (CardColumns)
    // библиотека значков
    std::vector<std::wstring> libDirs_, libFiles_;
    int libCat_ = -1;
    std::set<std::wstring> thumbsDone_;
    // кадр
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int bw_ = 0, bh_ = 0;
};

}  // namespace studio
