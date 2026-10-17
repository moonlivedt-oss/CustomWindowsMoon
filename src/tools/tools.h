#pragma once
// Утилиты: PATH и переменные окружения, диски, автозагрузка, порты и кто держит файл.
// Всё блокирующее зовётся из рабочих потоков; правки без прав - сразу, с правами - через задачу сети.
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
};
// Что занимает место в папке: размеры вложенных папок (с файлами в корне - отдельной строкой).
// cancel - выставить в true, чтобы остановить; progress - сколько файлов уже просмотрено.
// partial - после каждой посчитанной папки: что уже известно (по убыванию), чтобы список рос на глазах.
std::vector<FolderSize> ScanSizes(const std::wstring& dir, volatile bool* cancel, volatile long* progress,
                                  const std::function<void(const std::vector<FolderSize>&)>& partial = nullptr);

struct Junk {
    std::wstring name, path;
    ULONGLONG bytes = 0;
    bool admin = false;
    bool slow = false;  // пересоздаётся дорого (шейдеры): в «Всё сразу» не входит
};
std::vector<Junk> FindJunk(bool sizes = true);  // sizes=false - только список, быстро (для кольца)
ULONGLONG DirBytes(const std::wstring& root, int* files = nullptr, volatile bool* cancel = nullptr,
                   volatile long* progress = nullptr);
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
std::vector<ProjectJunk> FindProjectJunk(const std::vector<std::wstring>& roots, volatile bool* cancel = nullptr);
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

// ---------------- действия через RadialTools.exe ----------------
// Удаление и завершение процессов живут в отдельном RadialTools.exe рядом с
// основным: постоянно работающий RadialMenu.exe сам ничего не удаляет и чужие
// процессы не трогает. Всё ниже - обёртки, зовущие его.

ULONGLONG CleanJunk(const Junk& j,
                    const Log& log);  // удалить содержимое; сколько освобождено
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
    bool net = false;  // есть соединения в сеть
    int start = -1;    // номер в SecurityScan::starts (для автозапуска)
    int score = 0;     // сумма признаков
    std::wstring why;  // признаки через «; »
};
struct SecurityScan {
    std::vector<Suspect> procs, autos, hijacks;  // по убыванию счёта
    std::vector<Suspect> tasks;                  // задачи планировщика (where - путь задачи)
    std::vector<Suspect> exts;                   // расширения Chrome/Edge не из магазина (path - папка)
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

// Песочница Windows (sandbox.cpp): одноразовая Windows для незнакомой программы.
bool SandboxAvailable();    // компонент «Песочница Windows» включён
void OpenSandboxFeature();  // окно «Компоненты Windows», где её включают
// файл - внутрь только для чтения (папка, если программе нужны DLL рядом, иначе копия), сеть по выбору
// checkTools - папка с Sysinternals (папка защиты): режим «проверить поведение» со снимками до и после и отчётом
bool RunInSandbox(const std::wstring& file, bool network, std::wstring& err, const std::wstring& checkTools = L"");
std::wstring SandboxConfig(const std::wstring& file, bool network, std::wstring& err,
                           const std::wstring& checkTools = L"");  // только собрать .wsb
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
                                     volatile bool* cancel = nullptr, volatile long* progress = nullptr);
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
