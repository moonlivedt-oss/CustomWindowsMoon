#pragma once
// Утилиты: PATH и переменные окружения, диски, автозагрузка, порты и кто держит файл.
// Всё блокирующее зовётся из рабочих потоков; правки без прав - сразу, с правами - через задачу сети.
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

namespace tools {

using Log = std::function<void(const std::wstring&)>;

// ---------------- PATH и переменные ----------------

struct PathEntry {
    std::wstring raw;       // как записано (может быть с %VAR%)
    std::wstring full;      // после подстановки
    bool system = false;    // из системного PATH (HKLM), иначе - пользователя
    bool dead = false;      // папки нет
    bool dup = false;       // такая же папка уже была выше (в системном или раньше в своём)
    std::wstring provides;  // какие известные команды находятся здесь первыми: «python, pip»
};
std::vector<PathEntry> ReadPath();  // сначала системный (так ищет Windows), потом пользователя

struct Which {
    std::wstring cmd, path;           // python -> C:\...\python.exe
    std::vector<std::wstring> other;  // где ещё есть (затенены первым)
};
std::vector<Which> WhichAll();  // python, node, git, g++, cmake... - что найдётся первым

// PATH пользователя: записать (с бэкапом и рассылкой WM_SETTINGCHANGE)
bool WriteUserPath(const std::vector<std::wstring>& entries, const Log& log);
int CleanUserPath(const Log& log);  // убрать мёртвые и дубли; сколько убрано
bool AddUserPath(const std::wstring& dir, const Log& log);
std::vector<std::wstring> PathBackups();  // новые первыми
bool RestoreUserPath(const std::wstring& backupFile, const Log& log);
int CleanSystemPath(const Log& log);  // то же для системного - только из процесса с правами

struct EnvVar {
    std::wstring name, value;
    bool system = false;
    bool deadPath = false;  // значение похоже на путь, а его нет
};
std::vector<EnvVar> ReadEnv();
bool SetUserVar(const std::wstring& name, const std::wstring& value, const Log& log);  // пустое значение - удалить

// ---------------- диски ----------------

struct Volume {
    std::wstring root, label, fs;  // C:\, «Система», NTFS
    ULONGLONG total = 0, free = 0;
    UINT type = 0;  // DRIVE_FIXED, DRIVE_REMOVABLE...
};
std::vector<Volume> Volumes();
std::vector<Volume> VolumesCached(DWORD maxAgeMs = 30000);  // для основного потока: снимок, свежий - в фоне

struct Physical {
    std::wstring name;
    bool ssd = false;
    int health = 0;  // 0 исправен, 1 предупреждение, 2 плохо
    ULONGLONG size = 0;
    int temp = -1, wear = -1, hours = -1;  // SMART: только с правами (DiskHealth)
};
bool DiskWarn(const Volume& v);                                 // места мало: меньше 10% и меньше 20 ГБ
std::vector<Physical> PhysicalDisks();                          // без прав: имя, тип, здоровье
bool DiskHealth(std::vector<Physical>& disks, const Log& log);  // износ, температура, часы - через задачу с правами
std::wstring DiskHealthAdminMain();                             // в процессе с правами: строки для журнала

struct FolderSize {
    std::wstring path, name;
    ULONGLONG bytes = 0;
    int files = 0;
    bool closed = false;  // посчитано по MFT, а обходу внутрь нет входа (System Volume Information)
};
// Что занимает место в папке: размеры вложенных папок (с файлами в корне - отдельной строкой).
// cancel - выставить в true, чтобы остановить; progress - сколько файлов уже просмотрено.
// partial - после каждой посчитанной папки: что уже известно (по убыванию), чтобы список рос на глазах.
std::vector<FolderSize> ScanSizes(const std::wstring& dir, std::atomic<bool>* cancel, std::atomic<long>* progress,
                                  const std::function<void(const std::vector<FolderSize>&)>& partial = nullptr);

// Те же размеры прямым чтением MFT - секунды на весь диск. Только NTFS и только в процессе с правами (том
// открывает администратор); err - почему не вышло или сводка (записей, мс). Ссылки не раскрываются.
bool MftSizes(const std::wstring& dir, std::vector<FolderSize>& out, std::wstring& err);
// Из обычного процесса. Задача с правами за один проход считает все папки тома и кладёт таблицу в папку помощника;
// пока таблица свежая (5 минут), любая папка этого тома берётся из неё мгновенно. Таблицы нет - задача зовётся (без
// UAC, только если уже настроена) для диска и папок первого уровня, глубже быстрее обычный обход. Итог сверяется
// со списком папок (не совпал - false, тогда обычный ScanSizes); note - сводка или почему нет.
enum MftState { MFT_READY, MFT_NONE, MFT_OLD, MFT_BLOCKED };  // есть, не настроена, устарела, антивирус не пускает
// reading - true, пока задача читает таблицу (для подписи «Читаю таблицу MFT...»); state - что с задачей с правами,
// если её пришлось спросить (иначе не трогается).
bool ScanSizesMft(const std::wstring& dir, std::vector<FolderSize>& out, std::wstring& note,
                  std::atomic<bool>* reading = nullptr, MftState* state = nullptr,
                  const std::atomic<bool>* cancel = nullptr);  // cancel - «Стоп»: не ждать задачу
// Только из свежей таблицы в кэше, без задачи (для карточек): false - таблицы нет или устарела.
bool MftCached(const std::wstring& dir, std::vector<FolderSize>& out,
               bool verify = true);  // verify=false - для разбора
MftState MftReady();                 // спрашивает планировщик - не из потока окна
void MftForget();                    // выбросить разобранную таблицу из памяти (студия закрыта)
// Поиск папок по имени во всех таблицах MFT, что лежат в папке помощника (И8): таблица любой давности - имена папок
// меняются редко, размер рядом - на момент таблицы. age - сколько секунд самой старой из просмотренных таблиц.
// false - ни одной таблицы нет (подсчёт по MFT ни разу не запускали). Индекс держится в памяти до MftSearchForget.
struct FoundDir {
    std::wstring path;
    ULONGLONG bytes = 0;
    int files = 0;
};
bool MftFindDirs(const std::wstring& query, size_t max, std::vector<FoundDir>& out, ULONGLONG* age = nullptr);
void MftSearchForget(ULONGLONG idleMs);
// Большие файлы томов (И9): задача с правами при чтении MFT заодно кладёт 200 самых больших файлов в big-C.tsv.
// Здесь - все тома, только существующие файлы, размер - нынешний, по убыванию. why - почему в Корзину нельзя
// (BigFileKeep), пусто - можно. age - сколько секунд самому старому списку.
struct BigFile {
    std::wstring path, why;
    ULONGLONG bytes = 0;
};
std::vector<BigFile> BigFiles(ULONGLONG* age = nullptr);
// Можно ли большой файл отдать в Корзину: пусто - можно, иначе почему нет (системный, программа, файл подкачки...).
// Только по пути - одно правило для студии и RadialTools (тот ещё смотрит атрибуты).
std::wstring BigFileKeep(const std::wstring& path);
// Отрезать программу от интернета (И16): исходящее правило брандмауэра «MoonRadial: отрезано - <путь>». Одно правило
// для обычного процесса и задачи с правами: пусто - можно, иначе почему нет (системная программа, не exe...).
std::wstring CutRefuse(const std::wstring& exe);
std::wstring CutRuleName(const std::wstring& exe);
inline constexpr wchar_t kCutPrefix[] = L"MoonRadial: отрезано - ";

// Системные гиганты (И11, giants.cpp): hiberfil, подкачка, Windows.old, точки восстановления, загрузки обновлений,
// дампы. action - что предложить: hiber-off, old-windows, shadow-shrink (задача с правами), sys-clean («Остатки
// обновлений»), page-open, cleanmgr (открыть окно Windows); undo - обратное действие (hiber-on).
struct Giant {
    std::wstring key, name, path, why, action, undo;
    ULONGLONG bytes = 0;
    bool exact = false;  // размер из замера с правами (иначе - что видно без прав, папки могут быть больше)
};
// exact - размеры из последнего замера с правами (ключ -> байты)
std::vector<Giant> SystemGiants(const std::map<std::wstring, ULONGLONG>& exact = {});
ULONGLONG ParseShadowUsed(const std::wstring& vssadminOutput);  // «Used Shadow Copy Storage space: 2.53 GB (0%)»
// В процессе с правами: job - read (строки «@giant<TAB>ключ<TAB>байт»), hiber-off, hiber-on, shadow-shrink,
// old-windows; другое не выполняется. Пусто - не получилось.
std::wstring GiantsAdminMain(const std::wstring& job, const Log& log);
int RecycleBig(const std::wstring& path, const Log& log);  // через RadialTools.exe recycle-big  // выбросить индекс
                                                           // поиска, если им не пользовались дольше idleMs
// В процессе с правами: таблица тома, где лежит dir, - в файл кэша; ответ - строка «@mftok» или «@mftfail»
std::wstring MftAdminMain(const std::wstring& dir);

// Живой рост папок (livesize.cpp): ReadDirectoryChangesW будит поток, размер пересчитывается не чаще раза в минуту и
// только после изменений. today - с первого замера за сегодня, hour - за последний час (или с запуска, если меньше).
struct LiveSize {
    std::wstring name, path;
    ULONGLONG now = 0;
    LONGLONG today = 0, hour = 0;
    bool ready = false;  // первый замер ещё не сделан
};
void LiveStart(const std::vector<std::pair<std::wstring, std::wstring>>& folders);  // имя и путь
void LiveStop();
std::vector<LiveSize> LiveSizes();  // в процессе с правами: строки «@mft...» для журнала

struct Junk {
    std::wstring name, path;
    ULONGLONG bytes = 0;  // сколько чистка правда удалит (в TEMP - по правилу TempRemovable)
    ULONGLONG fresh = 0;  // в TEMP: свежее или занятое - чистка оставит, уйдёт в следующие разы
    bool admin = false;
    bool slow = false;  // пересоздаётся дорого (шейдеры): в «Всё сразу» не входит
};
// sizes=false - только список, быстро (для кольца); ddcOld - заодно старое в кэше Unreal, как OldBytes(DdcDir(), ddcDays)
std::vector<Junk> FindJunk(bool sizes = true, ULONGLONG* ddcOld = nullptr, int ddcDays = 30);
ULONGLONG DirBytes(const std::wstring& root, int* files = nullptr, std::atomic<bool>* cancel = nullptr,
                   std::atomic<long>* progress = nullptr);
// Сколько освободит удаление папки: как DirBytes, но без файлов «только в облаке» (для «можно освободить»).
ULONGLONG FreeableBytes(const std::wstring& root, std::atomic<bool>* cancel = nullptr);
// Обход дерева без рекурсии и без захода в папки-ссылки: NtQueryDirectoryFile с большим буфером (запасной путь -
// FindFirstFileEx). fn получает каждый файл, и файлы-ссылки тоже (их EndOfFile, как в MFT); папки обходятся сами.
inline bool LinkDir(DWORD attrs) {  // папка-ссылка: не раскрываем ни обходом, ни MFT
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}
// Файл «только в облаке» (OneDrive и др.): размер виден, а места на диске нет - удаление ничего не освободит.
inline bool CloudOnly(DWORD attrs) {
    return attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
}
struct WalkFile {
    DWORD attrs = 0;
    ULONGLONG size = 0, write = 0, create = 0, access = 0;  // время - FILETIME одним числом
    const std::wstring* dir = nullptr;                      // папка и имя - живут только на время вызова fn
    const wchar_t* name = L"";
    size_t nameLen = 0;
    std::wstring Path() const { return *dir + L"\\" + std::wstring(name, nameLen); }
};
// Можно ли удалить файл из TEMP сейчас (одно правило для подсчёта и для RadialTools):
// старше 6 часов и никто не держит (занятый не удалится, и обещать его нечестно); кэш разбора clangd
// (preamble-*.pch, по 30 МБ, остаются после закрытия редактора) - через час, если не держит. Остальное - свежее.
bool TempRemovable(const WalkFile& f, ULONGLONG now);
// занят кем-то или удалить его нам нельзя (пробное открытие на удаление без общего доступа)
bool FileInUse(const std::wstring& path);
void WalkFiles(const std::wstring& root, const std::function<void(const WalkFile&)>& fn,
               std::atomic<bool>* cancel = nullptr);
std::wstring TempDir();

// Кэш Unreal (DDC): общий для всех проектов; файлы, к которым давно не
// обращались, можно убрать без пересборки шейдеров текущих проектов.
std::wstring DdcDir();
ULONGLONG
OldBytes(const std::wstring& root, int days,
         int* files = nullptr);  // не открывались и не менялись days дней

// Пересобираемый мусор проектов: node_modules, Intermediate, target, .venv,
// build с CMakeCache.txt...
struct ProjectJunk {
    std::wstring project, name;  // папка проекта и её имя
    std::wstring path, kind;     // сама папка мусора и что это («node_modules»,
                                 // «Unreal: промежуточные»)
    ULONGLONG bytes = 0;
    int idleDays = 0;  // сколько дней в проекте ничего не менялось
};
std::vector<ProjectJunk> FindProjectJunk(const std::vector<std::wstring>& roots, std::atomic<bool>* cancel = nullptr);
// Итог обхода на диске (О5, scancache.cpp): %APPDATA%\MoonDesk\tools\scan-cache.tsv. Перезапуск в пределах срока
// берёт его, а не обходит кэши и проекты заново (главная цена старта - этот обход, 4-6 с процессора).
struct ScanCache {
    LONGLONG when = 0;  // секунды unix
    std::vector<Junk> junk;
    std::vector<ProjectJunk> projects;
    ULONGLONG ddcOld = 0;
};
std::wstring ScanCacheRows(const ScanCache& c);
bool ParseScanCache(const std::wstring& text, ScanCache& c);  // false - пусто или без времени
void SaveScanCache(const ScanCache& c);
bool LoadScanCache(ScanCache& c);
void DropScanCache();  // после чистки размеры на диске уже неверны - следующий старт обойдёт заново
bool IsProjectJunk(const std::wstring& path,
                   std::wstring* kind = nullptr);  // проверка признаков перед удалением

// Разбор Загрузок: старые установщики, распакованные архивы, копии «(1)», недокачанное.
struct DownloadJunk {
    std::wstring path, name, why;  // why: «установщик», «уже распакован», «копия», «недокачанный», «торрент-файл»
    ULONGLONG bytes = 0;
    int days = 0;  // сколько дней лежит
};
std::wstring DownloadsDir();
std::vector<DownloadJunk> FindDownloadJunk(int days);  // установщики и архивы - старше days дней
// в Корзину (можно вернуть) через RadialTools.exe; только файлы из корня Загрузок
int RecycleDownloads(const std::vector<std::wstring>& paths, const Log& log);

// Режим питания Windows 11 (ползунок «Режим питания»): экономия, сбалансированный, производительность.
enum PowerMode { POWER_SAVER, POWER_BALANCED, POWER_BEST, POWER_UNKNOWN };
PowerMode GetPowerMode();
bool SetPowerMode(PowerMode m);
bool OnBattery();
const wchar_t* PowerModeName(PowerMode m);

// Рост папок: снимок размеров раз в сутки (sizes.tsv), сравнение с неделей
// назад.
struct Growth {
    std::wstring name, path;
    LONGLONG delta = 0;  // на сколько выросло (может быть меньше нуля)
    ULONGLONG now = 0;
};
void RecordSizes(const std::vector<std::pair<std::wstring, std::wstring>>& namePath,
                 const std::vector<ULONGLONG>& bytes);  // имя и путь -> размер, на сегодня
std::vector<Growth> WeekGrowth(int days = 7);           // что выросло сильнее всего (по убыванию)

// Прогноз места (И10): свободное место томов раз в день (free.tsv, 60 дней) и через сколько дней оно кончится при
// нынешнем темпе - прямая по замерам последних 21 дня (метод наименьших квадратов).
struct FreePoint {
    int day = 0;
    ULONGLONG free = 0;
};
// Дней до нуля; -1 - место не убывает (медленнее 200 МБ в сутки) или замеров мало (нужно 3 за 3+ дня). perDay - на
// сколько байт в сутки убывает.
int DaysLeft(const std::vector<FreePoint>& pts, double* perDay = nullptr);
struct Forecast {
    std::wstring root;  // «C:\»
    ULONGLONG free = 0;
    int days = -1;
    double perDay = 0;
};
void RecordFree();                             // сегодняшний замер всех несъёмных томов (заменяет прежний за сегодня)
std::vector<Forecast> SpaceForecast();         // по каждому тому из free.tsv
std::wstring ForecastText(const Forecast& f);  // «C: закончится примерно через 23 дня (-4,1 ГБ в сутки)»

// ---------------- действия через RadialTools.exe ----------------
// Удаление и завершение процессов живут в отдельном RadialTools.exe рядом с
// основным: постоянно работающий RadialMenu.exe сам ничего не удаляет и чужие
// процессы не трогает. Всё ниже - обёртки, зовущие его.

ULONGLONG CleanJunk(const Junk& j,
                    const Log& log);  // удалить содержимое; сколько освобождено
// Несколько кэшей одним запуском RadialTools: started(i) - взялся за i-й, done(i, байт) - закончил (зовётся для
// каждого, даже если RadialTools не запустился). Сколько освобождено всего.
// busy - пути файлов, которые удалить не дали (до 300 на кэш): кто держит - WhoLocksFiles.
ULONGLONG CleanJunks(const std::vector<Junk>& list, const std::function<void(size_t)>& started,
                     const std::function<void(size_t, ULONGLONG)>& done, const Log& log,
                     std::vector<std::wstring>* busy = nullptr);
// Строка хода «clean» от RadialTools: «#start\t<i>», «#freed\t<i>\t<байт>», «#busy\t<путь>»; иначе - журнал.
struct CleanLine {
    enum Kind { LOG, START, FREED, BUSY } kind = LOG;
    size_t index = (size_t)-1;
    ULONGLONG bytes = 0;
    std::wstring text;  // для BUSY - путь
};
CleanLine ParseCleanLine(const std::wstring& s);
// Вывод «DISM /Online /Cleanup-Image /AnalyzeComponentStore»: фактический размер хранилища компонентов (DISM
// по-английски или по-русски, запятая или точка в числе), 0 - такой строки нет.
ULONGLONG ParseStoreSize(const std::wstring& dismOut);
ULONGLONG TrimDdc(int days,
                  const Log& log);  // удалить из кэша Unreal то, что не открывалось days дней
ULONGLONG CleanProjects(const std::vector<std::wstring>& paths,
                        const Log& log);  // каждую проверит IsProjectJunk

// ---------------- автозагрузка ----------------

struct StartItem {
    std::wstring name, command, where;  // where: «Run (пользователь)», «Папка
                                        // Автозагрузка», «Задача при входе»
    bool enabled = true;
    bool canToggle = true;  // без прав переключаем только своё (HKCU, своя папка, свои задачи)
    int kind = 0;           // внутреннее: откуда
    std::wstring key;       // внутреннее: имя значения, файл или путь задачи
    // вес: запущена ли программа сейчас и сколько личной памяти держит
    bool running = false;
    ULONGLONG memory = 0;
};
std::vector<StartItem> StartupItems();

bool SetStartup(const StartItem& it, bool on, const Log& log);
// Та же запись: откуда, ключ и команда. Перед выключением показанной записи сверяем её со свежим списком -
// пока карточка висела, запись могли поменять или убрать.
inline bool SameStart(const StartItem& a, const StartItem& b) {
    return a.kind == b.kind && a.key == b.key && a.command == b.command;
}
inline const StartItem* FindStart(const std::vector<StartItem>& items, const StartItem& it) {
    for (const StartItem& x : items)
        if (SameStart(x, it)) return &x;
    return nullptr;
}
void StartupWeight(std::vector<StartItem>& items);  // заполнить running и memory
                                                    // по запущенным процессам

// ---------------- порты и блокировки ----------------

struct Listener {
    int port = 0;
    bool udp = false;
    DWORD pid = 0;
    std::wstring process;
    bool dev = false;      // типичный порт разработки (3000, 5173, 8080...)
    std::wstring project;  // папка проекта по командной строке процесса (где
                           // package.json, Cargo.toml...)
    std::wstring command;  // командная строка, коротко
    int conns = 0;         // TCP-соединений на этом порту сейчас (для живой сцены: ток по кабелю)
};
std::vector<Listener> Listeners();                 // кто слушает порты
void ListenerProjects(std::vector<Listener>& ls);  // заполнить project и command
                                                   // (только для dev-портов)
struct Locker {
    DWORD pid = 0;
    std::wstring name;
};
std::vector<Locker> WhoLocks(const std::wstring& path);  // кто держит файл или файлы папки (Restart Manager)
std::vector<Locker> WhoLocksFiles(const std::vector<std::wstring>& files);  // то же для списка файлов, одним запросом
// завершить процесс; expect - каким exe он должен быть (номер мог достаться
// другому)
bool KillPid(DWORD pid, const std::wstring& expect = L"");
std::wstring ClipboardPath();   // путь из буфера: скопированный файл или текст-путь
std::wstring ExplorerFolder();  // папка верхнего окна Проводника, пусто - нет такого

// ---------------- процессы и корзина ----------------

struct HeavyApp {
    std::wstring name;        // chrome.exe
    std::vector<DWORD> pids;  // все процессы этой программы
    ULONGLONG bytes = 0;      // личная память всех процессов
};
std::vector<HeavyApp> HeavyApps(int count);  // самые тяжёлые программы по памяти; системные не показываются
bool ProtectedExe(const std::wstring& exe);  // системное, антивирус, сам RadialMenu: не
                                             // показывать и не завершать
struct AppClose {
    int closed = 0, killed = 0,
        failed = 0;  // закрылись сами, завершены силой, не далось
};
// закрыть программу: главным окнам WM_CLOSE, через waitMs оставшиеся процессы -
// силой
AppClose CloseApp(const HeavyApp& app, int waitMs, const Log& log);
ULONGLONG RecycleBinBytes(ULONGLONG* items = nullptr);
bool EmptyRecycleBin();
struct MemoryState {
    ULONGLONG total = 0, avail = 0;
    int loadPercent = 0;
};
MemoryState Memory();

// ---------------- защита ----------------
// Без своего движка: подпись файла (встроенная или каталожная, как у системных), откуда запущено, подмена
// системных имён, скрипты в автозапуске, перехваты в реестре. Только смотрит, ничего не меняет.

enum SignState { SIGN_NOFILE, SIGN_OK, SIGN_NONE, SIGN_UNTRUSTED, SIGN_BAD };
SignState CheckSign(const std::wstring& path, std::wstring* signer = nullptr);  // кэш по пути, размеру и времени

// Службы, драйверы и задачи планировщика (И17, sysitems.cpp). kind: svc / drv / task; name - имя службы или путь
// задачи, display - для людей, command - строка запуска, path - сам exe/sys, how - когда запускается.
struct SysItem {
    std::wstring kind, name, display, command, path, how, signer;
    int start = 0;  // тип запуска службы (SERVICE_AUTO_START...)
    SignState sign = SIGN_NOFILE;
};
std::vector<SysItem> SysItems();
std::vector<SysItem> NewSysItems();  // новое против sysitems.tsv (первый раз - только снимок); подписанное Microsoft - нет
std::wstring ImageOf(const std::wstring& command);  // exe или sys из строки запуска (\SystemRoot\, \??\, кавычки)
std::wstring SysItemKey(const SysItem& it);  // ключ снимка: без версии и GUID в хвосте имени
std::wstring SysItemRefuse(const SysItem& it);      // пусто - службу можно выключить задачей с правами
bool SetTaskEnabled(const std::wstring& path, bool on);
bool SysItemAdminMain(const std::wstring& req, const Log& log);

// Приватность Windows с откатом (И18, tweaks.cpp). on - всё «приватно» уже стоит; ours - менял RadialMenu (есть что
// вернуть); admin - для всего компьютера (HKLM), меняется задачей с правами JOB_PRIVACY («on|id», «off|id», «off|*»).
struct Tweak {
    std::wstring id, title, why;
    bool admin = false, on = false, ours = false;
};
std::vector<Tweak> PrivacyTweaks();
bool SetPrivacyTweak(const std::wstring& id, bool priv, const Log& log);  // только свои (HKCU)
int RevertPrivacy(const Log& log);                                        // свои - как были до нас
bool PrivacyAdminMain(const std::wstring& req, const Log& log);  // JOB_SYS_ITEM: «svc|имя», «on|svc|имя|тип»

constexpr int kSuspectScore = 3;  // с какого счёта показывать; 6 и выше - «опасно»
enum FindKind {
    FIND_PROC,
    FIND_AUTO,
    FIND_REG,
    FIND_TASK,
    FIND_EXT,
    FIND_NET,
    FIND_PATH,
    FIND_SVC,
    FIND_WMI,
    FIND_SELF,
    FIND_CERT,
    FIND_FW,
    kFindKinds
};
struct Suspect {
    FindKind kind = FIND_PROC;
    DWORD pid = 0;        // процесс; 0 - запись автозапуска или реестра
    std::wstring name;    // svchost.exe, имя записи автозапуска, «Shell»
    std::wstring path;    // файл (для реестра - значение)
    std::wstring where;   // откуда запись: «Run (пользователь)», ключ реестра
    std::wstring parent;  // кто запустил процесс
    std::wstring signer;  // издатель из подписи
    SignState sign = SIGN_NOFILE;
    bool net = false;      // есть соединения в сеть
    bool isStart = false;  // запись автозапуска: сама запись в startItem
    StartItem startItem;   // копия, а не номер: список автозагрузки мог обновиться, пока находка показана
    int score = 0;         // сумма признаков
    std::wstring why;      // признаки через «; »
};
// Права расширений браузеров (И35, extrights.cpp - разбор manifest.json под тестами).
enum ExtRight {
    EXT_ALL_SITES = 1,
    EXT_CLIPBOARD = 2,
    EXT_NATIVE = 4,
    EXT_PROXY = 8,
    EXT_DEBUGGER = 16,
    EXT_COOKIES = 32,
    EXT_HISTORY = 64,
    EXT_MANAGEMENT = 128,
    EXT_REQUESTS = 256
};
int ExtRightsOf(const std::string& manifest);  // биты ExtRight
std::wstring ExtRightsText(int rights);       // «читает и меняет все сайты, читает буфер обмена»
int ExtRisk(int rights);                      // 0 - ничего особого; от 4 - доступ к входам и паролям
struct BrowserExt {
    std::wstring browser, profile, id, name, dir;
    int rights = 0;
    bool store = true;  // обновляется из магазина браузера
};

struct SecurityScan {
    std::vector<Suspect> procs, autos, hijacks;  // по убыванию счёта
    std::vector<Suspect> tasks;                  // задачи планировщика (where - путь задачи)
    std::vector<Suspect> exts;                   // расширения Chrome/Edge не из магазина (path - папка)
    std::vector<BrowserExt> extList;             // все расширения с правами, опасные сначала
    std::vector<Suspect> nets;                   // hosts и DNS
    int tasksChecked = 0, extsChecked = 0;
    std::vector<StartItem> starts;    // вся автозагрузка на момент проверки
    int processes = 0, autoruns = 0;  // сколько проверено
    int unsignedCount = 0;            // запущенных программ без подписи (разных файлов)
};
SecurityScan ScanSecurity();  // секунды при первом разе (подписи), потом из кэша; из рабочего потока
// Только сеть (hosts, DNS, прокси) - для мгновенной проверки, когда эти настройки поменялись.
std::vector<Suspect> ScanNetwork();

// Системная часть «Защиты» (syscheck.cpp): находки - в SecurityScan::hijacks.
struct PathAcl {
    std::wstring raw, dir;  // как записано в PATH и после подстановки
    bool exists = false, windows = false, writable = false;
};
std::vector<PathAcl> SystemPathAcl();  // папки системного PATH, куда (или где создать которые) может писать кто угодно
void CheckPathAcl(std::vector<Suspect>& out);
void CheckServices(std::vector<Suspect>& out);  // службы: путь без кавычек, файл или папка открыты на запись
void CheckWmi(std::vector<Suspect>& out);       // подписки WMI, запускающие команды и скрипты
void CheckSelf(std::vector<Suspect>& out);      // свои exe изменились после сборки
std::wstring IntegrityRecord(const std::wstring& exePath);  // файл-отметка сборки для exe
int FixPathAclAdminMain(const Log& log);  // в процессе с правами: закрыть запись в папки системного PATH
// ---------------- восстановление (recovery.cpp) ----------------
enum BrokenKind {
    BROKEN_TASKMGR,
    BROKEN_REGEDIT,
    BROKEN_CMD,
    BROKEN_ASSOC,
    BROKEN_SHELL,
    BROKEN_SHELL_USER,
    BROKEN_USERINIT,
    BROKEN_IFEO
};
struct Broken {
    int id = 0;
    std::wstring what, where, value;  // что сломано, ключ реестра, нынешнее значение
    bool admin = false;               // чинится только с правами (HKLM)
};
std::vector<Broken> FindBroken();  // то, что ломают вирусы: политики, запуск .exe, Shell, Userinit, IFEO
int FixBrokenUser(const std::vector<Broken>& list, const Log& log);  // HKCU - без прав
int FixBrokenAdminMain(const Log& log);                              // HKLM - в процессе с правами
std::vector<std::wstring> PendingReboot();         // почему Windows ждёт перезагрузки (пусто - не ждёт)
bool RestartToRecovery();                          // перезагрузка в меню особой загрузки (спросит UAC)
bool RepairWindowsAdminMain(const Log& log);       // DISM RestoreHealth, потом SFC
bool ResetWindowsUpdateAdminMain(const Log& log);  // службы, SoftwareDistribution, catroot2
bool DefenderOfflineAdminMain(const Log& log);     // автономная проверка Defender (перезагрузит)
// Кэш Delivery Optimization (раздача обновлений) и вытесненные версии компонентов Windows (DISM
// StartComponentCleanup без ResetBase - последние обновления по-прежнему можно удалить). Строка «@freed=<байт>» -
// насколько выросло свободное место на системном диске.
bool SystemCleanAdminMain(const Log& log);

// ---------------- приватность и доверие (privacy.cpp) ----------------
struct PrivacyUse {
    std::wstring capability, app, path, signer;  // «камера», имя программы, путь (у обычных программ)
    ULONGLONG start = 0, stop = 0;               // FILETIME последнего начала и конца
    bool inUse = false, unsignedApp = false;
};
std::vector<PrivacyUse> PrivacyUses();              // камера, микрофон, геопозиция: кто и когда (сейчас - первыми)
void CheckCerts(std::vector<Suspect>& out);         // незнакомые корневые сертификаты (FIND_CERT)
void CheckFirewall(std::vector<Suspect>& out);      // входящие разрешения для программ из папок пользователя
std::wstring FileOrigin(const std::wstring& path);  // откуда скачан файл (метка Zone.Identifier), пусто - неизвестно
// Подпись своих exe: сертификат CN=MoonRadial Local в CurrentUser\My (подписывает сборка, cmake/sign.cmake)
bool CodeSigningReady();
bool SetupCodeSigning(const Log& log);  // создать и доверить у этого пользователя (Windows спросит), секунды

// ---------------- система (sysinfo.cpp) ----------------
struct DeviceProblem {
    std::wstring name, text;
    unsigned long code = 0;
    bool disabled = false;  // выключено вручную - не ошибка
};
std::vector<DeviceProblem> DeviceProblems();
struct GpuSample {
    bool ok = false;
    int temp = 0, load = 0;
    double watts = 0;
    bool thermal = false, powerCap = false;  // упирается в температуру / в питание
};
GpuSample GpuNow();  // nvidia-smi: секунда-две, из рабочего потока
struct BatteryTrend {
    ULONGLONG designed = 0, now = 0, monthAgo = 0;  // мВт*ч
    int monthDays = 0;
    bool mostlyAc = false;                          // последние две недели почти всё время на зарядке
    std::vector<std::pair<int, ULONGLONG>> points;  // день -> ёмкость
};
void RecordBattery();  // раз в минуту из слежки
BatteryTrend BatteryHistory();
struct UpdateInfo {
    bool ok = false;
    ULONGLONG lastOk = 0;  // FILETIME последнего успешного обновления
    std::wstring lastTitle;
    std::vector<std::wstring> failed;  // не установились за 30 дней
};
UpdateInfo WindowsUpdates();  // история Центра обновления, из рабочего потока
std::wstring SystemReport();  // отчёт о системе текстом, секунды

// ---------------- разработка (devtools2.cpp) ----------------
struct GitRepo {
    std::wstring path, name, branch;
    int changed = 0, ahead = 0, behind = 0;
    bool noUpstream = false;
};
std::vector<GitRepo> GitRepos(const std::vector<std::wstring>& roots);  // секунды: git status по каждому
struct VirtualDisk {
    std::wstring path, name;
    ULONGLONG bytes = 0;
    bool docker = false;
};
std::vector<VirtualDisk> VirtualDisks();  // диски WSL и Docker Desktop
int CompactVhdAdminMain(const Log& log);  // в процессе с правами: diskpart compact
struct Leftover2 {
    std::wstring path, name;
    ULONGLONG bytes = 0;
    int idleDays = 0;
};
// папки в AppData от программ, которых больше нет: не трогались idleDays дней и больше minBytes
std::vector<Leftover2> AppLeftovers(int idleDays = 90, ULONGLONG minBytes = 50ull * 1024 * 1024);
int RecycleLeftovers(const std::vector<std::wstring>& paths, const Log& log);  // через RadialTools.exe
bool ResetExplorer(const Log& log);                                            // через RadialTools.exe
// отмена броска на папку кольца: копии в Корзину; pairs - оригинал, копия, оригинал, копия... (RadialTools recycle-copy)
int RecycleCopies(const std::vector<std::wstring>& pairs, const Log& log);

// Песочница Windows (sandbox.cpp): одноразовая Windows для незнакомой программы.
bool SandboxAvailable();    // компонент «Песочница Windows» включён
void OpenSandboxFeature();  // окно «Компоненты Windows», где её включают
// файл - внутрь только для чтения (папка, если программе нужны DLL рядом, иначе копия), сеть по выбору
// checkTools - папка с Sysinternals (папка защиты): режим «проверить поведение» со снимками до и после и отчётом
bool RunInSandbox(const std::wstring& file, bool network, std::wstring& err, const std::wstring& checkTools = L"");
std::wstring SandboxConfig(const std::wstring& file, bool network, std::wstring& err,
                           const std::wstring& checkTools = L"");  // только собрать .wsb
// Ссылка в песочнице (И38): адрес открывается браузером внутри одноразовой Windows, сеть - есть, папок с этого
// компьютера - нет. SafeUrl - только http(s) без пробелов, кавычек и управляющих символов (пусто - не годится).
std::wstring SafeUrl(const std::wstring& raw);
std::wstring SandboxUrlConfig(const std::wstring& url, std::wstring& err);  // только собрать .wsb
bool OpenUrlInSandbox(const std::wstring& url, std::wstring& err);
std::wstring ClipboardUrl();  // адрес из буфера (текст, одна строка), прошедший SafeUrl
// системный PATH: как записано и после подстановки системных переменных
std::vector<std::pair<std::wstring, std::wstring>> SystemPathEntries();
std::wstring FileSha256(const std::wstring& path);  // в common.cpp: VirusTotal по хэшу, сверка дубликатов
bool SetTaskEnabled(const std::wstring& taskPath, bool on, const Log& log);  // своя задача; чужая - нужны права
std::wstring AntivirusUi();                                                  // avpui.exe Касперского, пусто - нет
struct SecurityApp {
    std::wstring name, exe;
};
std::vector<SecurityApp> SecurityApps(const std::wstring& dir);  // exe из папки и подпапок (кроме установщиков)
std::wstring StartExe(const StartItem& it);                      // какой exe запускает запись автозагрузки
const std::vector<std::wstring>& SystemExeNames();  // svchost.exe, lsass.exe...: под них маскируются чаще всего
bool InWindowsDir(const std::wstring& path);        // файл прямо в Windows, System32, SysWOW64 (или их wbem)
// Имя системного процесса, но файл не в папке Windows: такой не защищён от завершения.
bool SystemImposter(const std::wstring& fullPath);

// ---------------- слежка: процессы, память, батарея, сеть ----------------
// Раз в минуту из фонового потока: снимок процессов копится в ProcHistory, по нему - утечки и фоновый процессор.

struct ProcInfo {
    DWORD pid = 0, parent = 0;
    std::wstring name, path;
    ULONGLONG created = 0;  // время создания (FILETIME): номер процесса мог достаться новому
    ULONGLONG mem = 0;      // личная память
    ULONGLONG cpu = 0;      // процессорное время, 100 нс
    bool window = false;    // есть видимое окно верхнего уровня
};
std::vector<ProcInfo> Processes();  // этого входа (службы и чужие сеансы без прав не видны)
int ProcessMinutes(DWORD pid);      // сколько минут работает процесс, -1 - не узнать
ULONGLONG PrivateBytes(HANDLE h);   // личная память процесса, как столбец «Память» в Диспетчере задач
// запустить консольную программу без окна и забрать вывод (UTF-8 или кодировка консоли); код выхода, -1 - не
// запустилась
int RunCapture(const std::wstring& cmdline, std::wstring& out, DWORD timeoutMs);
// То же, но пока программа работает, не чаще раза в 2 с зовёт percent(N) - последний «NN.N%» в выводе (полоска DISM,
// «Проверка 45% завершена» у SFC), только когда число поменялось.
int RunCapture(const std::wstring& cmdline, std::wstring& out, DWORD timeoutMs,
               const std::function<void(int)>& percent);
// Последний процент в куске вывода (байты; нули UTF-16 пропускаются): «[=====  25.3%  ]» -> 25; нет - -1.
int LastPercent(const std::string& raw);

class ProcHistory {
public:
    void Feed(const std::vector<ProcInfo>& ps, ULONGLONG tick);  // tick - GetTickCount64
    struct Leak {
        DWORD pid = 0;
        std::wstring name, path;
        ULONGLONG was = 0, now = 0;  // личная память час назад и сейчас
        int minutes = 0;             // за сколько выросло
    };
    std::vector<Leak> Leaks() const;  // выросли вдвое за час (и больше чем на 700 МБ), рост ровный, не скачок
    struct Hog {
        std::wstring name, path;
        std::vector<DWORD> pids;
        double percent = 0;  // средняя доля всего процессора
        bool window = false;
        ULONGLONG mem = 0;
    };
    std::vector<Hog> CpuHogs(int minutes, bool backgroundOnly) const;  // по программам, самые прожорливые первыми
    bool Ready(int minutes) const;                                     // история уже покрывает столько минут

private:
    struct Sample {
        ULONGLONG tick, mem, cpu;
    };
    struct Track {
        ProcInfo last;
        std::vector<Sample> samples;  // раз в минуту, не дольше трёх часов
    };
    std::map<std::pair<DWORD, ULONGLONG>, Track> tracks_;  // по номеру и времени создания
    ULONGLONG first_ = 0, lastTick_ = 0;
};

// Хвосты сборки: воркеры Unreal без открытого редактора; компиляторы, node и clangd, чей родитель уже закрыт.
struct Leftover {
    DWORD pid = 0;
    std::wstring name, why;
    ULONGLONG mem = 0;
    int minutes = 0;
};
std::vector<Leftover> BuildLeftovers(const std::vector<ProcInfo>& ps);

struct CommitInfo {
    ULONGLONG total = 0, limit = 0, peak = 0, phys = 0;  // выделено, предел (память + подкачка), пик с загрузки
};
CommitInfo Commit();
ULONGLONG PagefileBytes();  // размер файлов подкачки сейчас
ULONGLONG PagefileUsed();   // сколько в них занято на деле (счётчик PDH)

struct BatteryInfo {
    bool present = false, ac = true, charging = false;
    int percent = -1, minutesLeft = -1, cycles = -1;
    ULONGLONG designed = 0, full = 0;  // мВт·ч: по паспорту и полный заряд сейчас
};
BatteryInfo Battery();                    // через драйвер батареи, без прав
std::vector<std::wstring> WakeDevices();  // кто может разбудить: powercfg /devicequery wake_armed

void NetCounters(ULONGLONG& in, ULONGLONG& out);  // байты через физические адаптеры (без туннелей и Hyper-V)
struct NetUse {
    DWORD pid = 0;
    std::wstring name;
    ULONGLONG down = 0, up = 0;  // байт/с
};
std::vector<NetUse> NetTopAdminMain(int ms);  // в процессе с правами: TCP по процессам за ms (EStats)
std::wstring PowerRequestsAdminMain();        // в процессе с правами: powercfg /requests и /waketimers

// Новый автозапуск с прошлой проверки (autostart.tsv): первый раз только запоминает, о каждой записи - один раз.
std::vector<StartItem> NewStartupItems();

// ---------------- версии инструментов ----------------
struct ToolVersion {
    std::wstring name, path, installed, latest;
    std::wstring note;  // «вышла 3.15», «12 пакетов MSYS2»
    bool newer = false;
    std::vector<std::wstring> train;  // ветки от своей до последней («3.10», «3.11»...), пусто - не знаем
    std::wstring eol;                 // своя ветка без поддержки скоро или уже: «поддержка до 10.2026»
};
std::vector<ToolVersion> ToolVersions();  // по сети, секунды: python, node, cmake, git, пакеты MSYS2

// ---------------- дубликаты ----------------
struct DupGroup {
    ULONGLONG size = 0;
    std::wstring hash;
    std::vector<std::wstring> paths;  // paths[0] - самый старый, он остаётся
};
std::vector<std::wstring> DupRoots();  // Загрузки, Рабочий стол, Документы, Видео, Изображения, Музыка
std::vector<DupGroup> FindDuplicates(const std::vector<std::wstring>& roots, ULONGLONG minBytes,
                                     std::atomic<bool>* cancel = nullptr, std::atomic<long>* progress = nullptr);
// копии - в Корзину через RadialTools.exe: он сам сверит размер и SHA-256 с оставляемым файлом
int RecycleDuplicates(const std::wstring& keep, const std::vector<std::wstring>& copies, const Log& log);

// ---------------- PATH: конфликты и терминал ----------------

// Затенённые версии: python 3.10 находится раньше 3.14 и т.п. Пустой список -
// конфликтов нет.
struct Conflict {
    std::wstring cmd, winner,
        loser;  // команда, кто найдётся первым, кто затенён (полные пути к exe)
    std::wstring winnerVer,
        loserVer;          // версии из пути («3.10», «14.2»), если видны
    bool fixable = false;  // обе папки в своём PATH - можно поднять затенённую выше
};
std::vector<Conflict> PathConflicts();
bool MoveUserPathFirst(const std::wstring& dir,
                       const Log& log);  // поднять папку в начало своего PATH (с бэкапом)

// Терминал с окружением проекта: venv, MSYS2 для C++, node_modules\.bin, Unreal
// (RunUAT).
struct TermEnv {
    std::wstring dir;
    std::vector<std::wstring> what;  // что подключено: «venv», «MSYS2 ucrt64»...
    std::wstring setup;              // команды cmd перед приглашением
};
TermEnv DetectEnv(const std::wstring& dir);
bool OpenEnvTerminal(const TermEnv& env);

// ---------------- здоровье и драйверы ----------------

// Здоровье SSD раз в неделю: сравнить с прошлым разом; вернуть, что стоит
// сказать (пусто - всё хорошо).
std::wstring HealthWeekly(std::vector<Physical>& disks);
struct Driver {
    std::wstring gpu, installed, latest,
        date;            // «NVIDIA GeForce RTX 5050 Laptop GPU», «610.74», «617.42»
    bool ok = false;     // удалось узнать обе версии
    bool newer = false;  // вышел новее установленного
};
Driver NvidiaDriver();  // по сети; зовётся из рабочего потока

std::wstring DataDir();  // %APPDATA%\MoonDesk\tools
std::wstring Bytes(ULONGLONG b);

}  // namespace tools
