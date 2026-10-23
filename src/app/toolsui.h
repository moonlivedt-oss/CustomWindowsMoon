#pragma once
// Утилиты в основной программе: кольцо «Утилиты» (действие угла tools), действия в фоне с ходом в карточке,
// предупреждение, когда на диске мало места.
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

struct Config;
struct Item;
class BoostCard;
class RadialMenu;
#include "tools/devserv.h"
#include "tools/tools.h"
#include "tools/weekdiff.h"

namespace toolsui {

inline constexpr UINT WM_TOOLS_JOB = WM_APP + 63;  // ход действия: lParam - строка журнала или итог

// openStudio(page): 0 - PATH, 1 - диски, 2 - автозагрузка, 3 - порты, 4 - процессы (колба, батарея), 5 - версии,
// 6 - защита; scan -
// что сразу просканировать
void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below,
          std::function<void(int page, const std::wstring& scan)> openStudio);
void ShowRing(RadialMenu& menu);
std::vector<Item> RingItems(std::wstring& status);  // пункты кольца без показа (для командной палитры)
void Run(const std::wstring& key);
// «Важное» (О4): дела из первого сектора кольца - имя, причина (note), ключ действия (path) для Run; пусто - всё
// спокойно. demo - выдуманные, для снимков
std::vector<Item> ImportantItems(bool demo = false);
void OnJob(WPARAM wp, LPARAM lp);
void Tick();  // карточка и раз в 10 минут - проверка места на дисках
void Shutdown();
void OnModes(bool game, bool dev);  // игровой режим и Unreal: в игре карточки молчат, после Unreal - хвосты

// Где искать мусор проектов: папки из колец (radial.cfg) и обычные места проектов (source/repos, Unreal Projects).
std::vector<std::wstring> ProjectRoots(const Config& cfg);

// Мало памяти перед игрой или редактором (когда Boost не настроен или ему нечего закрыть): карточка с самыми
// тяжёлыми программами и «Закрыть» для первой. keep - что не предлагать (сама игра, редактор). force - показать,
// даже если памяти хватает (вызов вручную). true - карточка показана.
bool MemoryOffer(const std::wstring& why, const std::vector<std::wstring>& keep, bool force);
void CloseHeavy(const tools::HeavyApp& app);  // мягко закрыть программу в фоне, ход - в карточке

// Для страницы «Состояние» в студии: последний фоновый подсчёт и действия над ним.
struct State {
    std::vector<tools::Junk> junk;
    std::vector<tools::ProjectJunk> projects;
    std::vector<tools::Growth> growth;
    ULONGLONG ddcOld = 0;
    tools::Driver driver;
    tools::WeekReport week;  // что изменилось за неделю (И36), для «Состояния»
    ULONGLONG at = 0;        // когда посчитано (GetTickCount64), 0 - ещё ни разу
    bool scanning = false;   // считается прямо сейчас
};
State Snapshot();
// То же для того, кто держит снимок у себя и спрашивает часто (страница «Состояние» - каждые 250 мс): вектора
// копируются, только если подсчёт или чистка их поменяли с прошлого раза (gen - номер изменения, начать с ~0u);
// когда посчитано и идёт ли подсчёт - всегда. true - снимок обновился целиком.
bool SnapshotIfChanged(State& s, unsigned& gen);
// Для живых сцен студии (колба, батарея, табло версий): что знает слежка и последняя проверка версий.
struct Watched {
    std::vector<tools::ProcHistory::Leak> leaks;
    std::vector<tools::ProcHistory::Hog> hogs;  // фон без окна за 10 минут, самые прожорливые первыми
    bool hogsReady = false;                     // история уже покрывает 10 минут
    tools::CommitInfo commit;
};
Watched Watch();
// Сколько слежка молчит, мс (0 - ещё не прошла ни разу); periodMs - как часто она должна проходить сейчас
ULONGLONG WatchAge(ULONGLONG& periodMs);
double NetDownBps();  // приём по физическим адаптерам за последнюю минуту, байт/с (слежка)
// Забытые серверы разработки (Л19): снимок портов, проектов и заголовков окон, решение - tools/devserv
std::vector<tools::Forgotten> FindForgotten(const std::vector<tools::ProcInfo>& ps,
                                            std::map<std::wstring, ULONGLONG>& lastSeen, ULONGLONG idleMs,
                                            ULONGLONG minBytes);
std::vector<tools::ToolVersion> Versions();                  // пусто - ещё не проверялось
void SetVersions(const std::vector<tools::ToolVersion>& v);  // проверили из студии - кольцу тоже
void Refresh();                                              // пересчитать сейчас
// Чистка кэшей - одна на всю программу: кольцо, студия и тихая чистка делят одну ленту scene::Clean.
void SystemClean();  // остатки обновлений Windows (Delivery Optimization, DISM) - спросит и пойдёт с правами
bool CleanClaim();   // занять; false - уже идёт другая
void CleanRelease();
bool CleanBusy();
// В рабочем потоке после CleanClaim и scene::CleanBegin(list): один запуск RadialTools на весь список, ход - в сцену,
// размеры в подсчёте правятся по факту, в конце CleanRelease. Сколько освобождено.
// lockers - кто не дал удалить занятое: «clangd, msedgewebview2» (пусто - всё удалилось или было свежим).
ULONGLONG CleanList(const std::vector<tools::Junk>& list, const tools::Log& log, std::wstring* lockers = nullptr);
void PrimeForSnapshot(const Config* cfg);  // для --studio-snap: посчитать сразу и дождаться
// для --tools-ring-snap: посчитать, собрать кольцо утилит и снять его с курсором на секторе root
// demo - выдуманные пункты (эталон build.cmd snaps), иначе - настоящие данные этого компьютера
void SnapRing(RadialMenu& menu, const Config* cfg, UINT dpi, int root, const std::wstring& path, bool demo = false);
void TrimDdc();  // старое из кэша Unreal
void CleanProjectList(const std::vector<tools::ProjectJunk>& list);
bool Busy();

// Вкладка «Защита» в студии: находки последней проверки без отмеченных «Это моё».
struct SecurityView {
    std::vector<tools::Suspect> finds;
    bool checked = false, running = false;
    int processes = 0, autoruns = 0;
    int ignored = 0;                      // сколько отмечено «Это моё»
    std::vector<tools::BrowserExt> exts;  // все расширения браузеров с правами, опасные сначала (Н4)
};
SecurityView SecurityFinds();                    // не проверялось - запускает проверку в фоне
void PrimeSecurityForSnapshot();                 // для --studio-snap 7 6: проверить сразу, в этом потоке
void SecurityRescan();                           // проверить заново (итог - в SecurityFinds)
void SecurityAct(const tools::Suspect& s);       // карточка «что с этим сделать», как из кольца
void OpenExtension(const tools::BrowserExt& e);  // страница расширения в его браузере (там - удалить)
void IgnoreFind(const tools::Suspect& s);        // «Это моё»: больше не показывать (ignored.tsv)
void ClearIgnored();                             // показывать снова всё
const wchar_t* FindKindName(tools::FindKind k);  // «процесс», «сертификат»...
const wchar_t* FindKindIcon(tools::FindKind k);  // файл значка в icons: tools-certs.png...
// Системные гиганты (И11, toolsui_giants.cpp): hiberfil, подкачка, Windows.old, точки восстановления... Размеры
// считаются в фоне (пусто, пока не посчитано); GiantsMeasure - точные размеры задачей с правами; GiantAct - действие
// с вопросом (undo - обратное: вернуть гибернацию).
// Сторожа процессов (И12-И14, toolsui_guard.cpp): что держит компьютер без сна (пусто - ничего), кому тихий
// баланс понизил приоритет, вернуть всем обычный.
std::wstring AwakeReason();
std::vector<std::wstring> BalanceLowered();
void BalanceRestoreAll();
std::vector<tools::Giant> Giants();
bool GiantsCounting();
void GiantsMeasure();
void GiantAct(const std::wstring& key, bool undo = false);
// Для проверки (WM_APP+58): карточка со сценой на выдуманных данных, ничего не удаляет.
// 0 - очистка, 1 - защита с находками, 2 - защита, всё чисто
void Demo(int what);

}  // namespace toolsui
