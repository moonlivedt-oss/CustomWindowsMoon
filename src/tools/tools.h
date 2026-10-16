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
std::vector<Junk> FindJunk(bool sizes = true);  // sizes=false - только список, быстро (для кольца) // временные файлы и
                                                // кэши, с размерами
ULONGLONG CleanJunk(const Junk& j, const Log& log);  // удалить содержимое; сколько освобождено

// ---------------- автозагрузка ----------------

struct StartItem {
    std::wstring name, command, where;  // where: «Run (пользователь)», «Папка Автозагрузка», «Задача при входе»
    bool enabled = true;
    bool canToggle = true;  // без прав переключаем только своё (HKCU, своя папка, свои задачи)
    int kind = 0;           // внутреннее: откуда
    std::wstring key;       // внутреннее: имя значения, файл или путь задачи
};
std::vector<StartItem> StartupItems();
bool SetStartup(const StartItem& it, bool on, const Log& log);

// ---------------- порты и блокировки ----------------

struct Listener {
    int port = 0;
    bool udp = false;
    DWORD pid = 0;
    std::wstring process;
    bool dev = false;  // типичный порт разработки (3000, 5173, 8080...)
};
std::vector<Listener> Listeners();  // кто слушает порты
struct Locker {
    DWORD pid = 0;
    std::wstring name;
};
std::vector<Locker> WhoLocks(const std::wstring& path);  // кто держит файл или файлы папки (Restart Manager)
// завершить процесс; expect - каким exe он должен быть (номер мог достаться другому)
bool KillPid(DWORD pid, const std::wstring& expect = L"");
std::wstring ClipboardPath();  // путь из буфера: скопированный файл или текст-путь

// ---------------- процессы и корзина ----------------

struct HeavyApp {
    std::wstring name;        // chrome.exe
    std::vector<DWORD> pids;  // все процессы этой программы
    ULONGLONG bytes = 0;      // сумма рабочих наборов
};
std::vector<HeavyApp> HeavyApps(int count);  // самые тяжёлые программы по памяти; системные не показываются
struct AppClose {
    int closed = 0, killed = 0, failed = 0;  // закрылись сами, завершены силой, не далось
};
// закрыть программу: главным окнам WM_CLOSE, через waitMs оставшиеся процессы - силой
AppClose CloseApp(const HeavyApp& app, int waitMs, const Log& log);
ULONGLONG RecycleBinBytes(ULONGLONG* items = nullptr);
bool EmptyRecycleBin();

std::wstring DataDir();  // %APPDATA%\MoonDesk\tools
std::wstring Bytes(ULONGLONG b);

}  // namespace tools
