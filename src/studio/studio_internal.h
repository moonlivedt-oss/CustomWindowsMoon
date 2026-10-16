#pragma once
// Внутреннее устройство Radial Studio: идентификаторы элементов, класс окна.
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
#include "tools/tools.h"
#include "core/config.h"
#include "core/themes.h"
#include "ui/ui.h"

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
    PAGE_AUTO
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
    ID_BLUR,
    ID_SCALE,
    ID_MAXCH_MINUS,
    ID_MAXCH_PLUS,
    ID_SOUNDS,
    ID_VOLUME,
    ID_TERMINAL,
    ID_AUTOSTART,
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
    ID_TOOLS_JUNK_SCAN,
    ID_TOOLS_LOCK_PATH,
    ID_TOOLS_LOCK,
    ID_TOOLS_LOCK_CLIP,
    ID_TOOLS_SYS_TOGGLE,
    ID_STATE_REFRESH,
    ID_STATE_CLEAN,
    ID_STATE_DDC,
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
    ID_AUTO_REPAIR,
    ID_BACKUP = 7000,        // +i
    ID_NET_KILL = 8000,      // +i: «Завершить» у прокси-процесса
    ID_TOOLS_ROW = 8100,     // +запись*4: кнопки у записи PATH
    ID_TOOLS_SIZE = 8400,    // +i*2: «Внутрь» и «Открыть» в «Что занимает место»
    ID_TOOLS_JUNK = 8450,    // +i: «Очистить» у кэша
    ID_TOOLS_START = 8500,   // +i: переключатель автозагрузки
    ID_TOOLS_PORT = 8600,    // +i: «Завершить» у порта
    ID_TOOLS_LOCKER = 8700,  // +i: «Завершить» у того, кто держит файл
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
    ID_HOME_TILE = 9800,     // +i: плитка «Обзора» (+50 - её анимация)
    ID_STATE_PROJ = 10000,   // +i: строка мусора проектов (выбор)
    ID_STATE_DL = 10100,     // +i: строка Загрузок (выбор)
    ID_STATE_APP = 10200,    // +i: «Закрыть» у тяжёлой программы
    ID_STATE_ANIM = 10300,   // +i: появление шкал и полос
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
    volatile int progress = 0;
    volatile bool done = false;
    bool ok = false;
    std::wstring out, message, previous;
    COLORREF body = 0, accent = 0;
    std::wstring name;
    HWND wnd = nullptr;
};

// Работа раздела «Сеть» в своём потоке: журнал и отчёт под замком, страница только читает.
struct NetJob {
    SRWLOCK lock = SRWLOCK_INIT;
    volatile bool done = false;
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
    volatile bool done = false, cancel = false;
    volatile long progress = 0;
    bool ok = false, hasSizes = false, hasJunk = false, hasPhys = false;
    std::vector<tools::FolderSize> sizes;
    std::vector<tools::Junk> junk;
    std::vector<tools::Physical> phys;
    std::vector<std::wstring> log;
    std::wstring title;
    std::function<bool(const tools::Log&, ToolsJob&)> work;
    HWND wnd = nullptr;
};

void Preview(int what);     // studio.h: EditorPreview
std::wstring GameStatus();  // studio.h: SetEditorGameStatus
std::wstring DevStatus();   // studio.h: SetEditorDevStatus

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
    void TabBasic(const RectF& view, float y, float& bottom);
    void TabRing(const RectF& view, float y, float& bottom);
    void TabIcon(const RectF& view, float y, float& bottom);
    void ThemesPage(const RectF& r);
    void ThemeHero(const RectF& hero, const ThemeInfo& t, bool trying);
    void ThemeCard(int index, const ThemeInfo& t, const RectF& cr, const RectF& view);
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
    void StatePage(const RectF& r);    // «Состояние»: шкалы, что освободить, проекты, Загрузки, рост, система
    void AutoPage(const RectF& r);     // «Автоматика»: переключатели и журнал
    float SubTabs(const RectF& body);  // вкладки группы над страницей; сколько места заняли
    void ToolsPage(const RectF& r);
    void ToolsRun(const std::wstring& title, std::function<bool(const tools::Log&, ToolsJob&)> work);
    void ToolsScan(const std::wstring& dir);
    void NetCheck();
    void NetRun(const std::wstring& title, std::function<bool(const net::Log&, NetJob&)> work);
    void CornerMenu(int corner, int kind);  // kind: 0 задержка, 1 двойной толчок, 2 колесо
    void SetCorner(int corner, int kind, const std::wstring& val);
    bool CaptureKey(WPARAM vk);
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
    // «Состояние»: живые данные (не чаще раза в 3 с), выбор строк, к какой карточке прокрутить
    ULONGLONG stateAt_ = 0, statePhysAt_ = 0;
    std::vector<tools::Volume> stateVols_;
    tools::MemoryState stateMem_;
    std::vector<tools::HeavyApp> stateApps_;
    std::vector<tools::Physical> statePhys_;
    struct PhysJob {
        SRWLOCK lock = SRWLOCK_INIT;
        volatile bool done = false;
        std::vector<tools::Physical> disks;
    };
    std::shared_ptr<PhysJob> physJob_;
    bool painting_ = false, paintAgain_ = false;  // кадр уже рисуется (вложенный WM_PAINT из COM)
    std::set<std::wstring> projSel_, dlSel_;
    std::wstring stateFocus_;
    float stateFocusY_ = -1;
    Overlay ov_ = OV_NONE;
    int ring_ = 0, sel_ = 0, tab_ = 0, tourStep_ = 0;
    int editingField_ = 0;
    int captureCorner_ = -1, captureKind_ = 0;  // ждём сочетание клавиш для угла
    std::shared_ptr<IconJob> iconJob_;
    std::wstring sysStatus_;
    std::shared_ptr<lockscreen::Job> lockJob_;
    std::wstring logonStatus_;
    std::wstring boostStatus_;
    std::shared_ptr<NetJob> netJob_;
    std::shared_ptr<net::Report> netReport_;
    std::vector<net::ProxyProc> netProcs_;
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
    std::vector<tools::Listener> toolsPorts_;
    std::vector<tools::Locker> toolsLockers_;
    bool toolsLockChecked_ = false, toolsJunkAsked_ = false, toolsShowSys_ = false;
    std::wstring toolsAddPath_, toolsEnvName_, toolsEnvValue_, toolsScanPath_, toolsLockPath_;
    std::shared_ptr<volatile int> boostTask_;  // задача очистки: 0 нет, 1 есть, 2 создаётся; общая с потоком
    std::wstring boostDir_;
    bool boostPurge_ = true;
    ULONGLONG boostReadAt_ = 0;
    bool savePending_ = false;
    std::vector<Config> undo_, redo_;
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
    void NetRoute(const RectF& r, bool busy);  // схема ПК - выход - DNS - интернет, по ней бежит проверка
    bool netWasBusy_ = false;
    ULONGLONG netDoneAt_ = 0;  // когда пришёл итог проверки: узлы схемы загораются по очереди
    int railPop_ = -1;    // значок группы, которую только что выбрали, подпрыгивает
    ULONGLONG railPopAt_ = 0;
    std::wstring hoverTheme_, hoverThemeNext_;  // примерка темы наведением
    // витрина тем: прежняя тема растворяется в новую
    ThemeInfo heroCur_{}, heroPrev_{};
    std::wstring heroKey_;
    bool heroTrying_ = false, heroPrevTrying_ = false;
    ULONGLONG heroAt_ = 0;
    UiColors curUi_{};
    bool curUiReady_ = false;
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
