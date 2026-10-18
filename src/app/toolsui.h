#pragma once
// Утилиты в основной программе: кольцо «Утилиты» (действие угла tools), действия в фоне с ходом в карточке,
// предупреждение, когда на диске мало места.
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

struct Config;
struct Item;
class BoostCard;
class RadialMenu;
#include "tools/tools.h"

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
    ULONGLONG at = 0;       // когда посчитано (GetTickCount64), 0 - ещё ни разу
    bool scanning = false;  // считается прямо сейчас
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
void TrimDdc();                            // старое из кэша Unreal
void CleanProjectList(const std::vector<tools::ProjectJunk>& list);
bool Busy();

// Вкладка «Защита» в студии: находки последней проверки без отмеченных «Это моё».
struct SecurityView {
    std::vector<tools::Suspect> finds;
    bool checked = false, running = false;
    int processes = 0, autoruns = 0;
    int ignored = 0;  // сколько отмечено «Это моё»
};
SecurityView SecurityFinds();                    // не проверялось - запускает проверку в фоне
void PrimeSecurityForSnapshot();                 // для --studio-snap 7 6: проверить сразу, в этом потоке
void SecurityRescan();                           // проверить заново (итог - в SecurityFinds)
void SecurityAct(const tools::Suspect& s);       // карточка «что с этим сделать», как из кольца
void IgnoreFind(const tools::Suspect& s);        // «Это моё»: больше не показывать (ignored.tsv)
void ClearIgnored();                             // показывать снова всё
const wchar_t* FindKindName(tools::FindKind k);  // «процесс», «сертификат»...
const wchar_t* FindKindIcon(tools::FindKind k);  // файл значка в icons: tools-certs.png...
// Для проверки (WM_APP+58): карточка со сценой на выдуманных данных, ничего не удаляет.
// 0 - очистка, 1 - защита с находками, 2 - защита, всё чисто
void Demo(int what);

}  // namespace toolsui
