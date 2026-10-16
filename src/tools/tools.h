#pragma once
// Утилиты: PATH и переменные окружения, диски, автозагрузка, порты и кто держит файл.
// Всё блокирующее зовётся из рабочих потоков; правки без прав - сразу, с правами - через задачу сети.
#include <functional>
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
std::vector<FolderSize> ScanSizes(const std::wstring& dir, volatile bool* cancel, volatile long* progress);

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
