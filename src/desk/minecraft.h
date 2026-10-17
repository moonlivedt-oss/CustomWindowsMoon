#pragma once
#include <string>
#include <vector>

#include <windows.h>

// Minecraft через PrismLauncher: сборки, какая сборка запущена, слежка за игрой.
// - Сборки читаются из %APPDATA%\PrismLauncher\instances\*\instance.cfg (имя, память, аргументы Java, значок).
// - Запущенную игру узнаём по командной строке javaw: Prism передаёт -Djava.library.path=...\instances\<id>\natives
//   и -Xmx. Официальный лаунчер - по net.minecraft / --gameDir.
// - Во время игры поток раз в 5 секунд дочитывает logs\latest.log («Can't keep up!», OutOfMemoryError),
//   раз в 2 минуты спрашивает у Java занятую кучу (jcmd GC.heap_info - лежит рядом с javaw, права не нужны).
// - После игры: новый отчёт о падении в crash-reports или hs_err_pid*.log - что случилось и какой мод виноват.
namespace mc {

struct Instance {
    std::wstring id;    // папка в instances (её же понимает prismlauncher -l)
    std::wstring name;  // как в Prism
    std::wstring game;  // папка игры: instances\<id>\minecraft (или .minecraft)
    std::wstring icon;  // значок сборки (png/gif) или пусто
    int maxMb = 0;      // -Xmx
    int mods = 0;
    bool preTouch = false;  // -XX:+AlwaysPreTouch в аргументах
    bool ownArgs = false;   // свои аргументы Java, а не общие из настроек Prism
    ULONGLONG lastLaunch = 0;
};

std::wstring PrismDir();
std::wstring PrismExe();
std::vector<Instance> Instances();  // последние запущенные первыми
bool FindInstance(const std::wstring& id, Instance& out);
bool PrismRunning();
std::vector<DWORD> PrismPids();  // сам лаунчер (не игра)
bool Launch(const std::wstring& id);

// Процесс - Minecraft? id - сборка Prism (пусто у официального лаунчера), xmxMb - из -Xmx.
bool IsMinecraft(DWORD pid, const std::wstring& path, std::wstring& id, int& xmxMb);
// Для игрового режима: имя игры для процесса или пусто (не Minecraft).
std::wstring GameName(DWORD pid, const std::wstring& path);

// Убрать -XX:+AlwaysPreTouch у сборки (Prism должен быть закрыт: он перезаписывает instance.cfg при выходе).
bool RemovePreTouch(const std::wstring& id, std::wstring& err);

// ---------------- слежка за игрой ----------------
struct Stats {
    std::wstring id, name;
    int xmxMb = 0;
    double peakHeapMb = 0;  // пик занятой кучи (по jcmd), 0 - не удалось спросить
    double heapMb = 0;      // сколько кучи выделено (committed)
    int heapSamples = 0;
    int lags = 0;             // «Can't keep up!»
    double lagMs = 0;         // сколько всего отставал
    bool oom = false;         // OutOfMemoryError в логе
    ULONGLONG startWall = 0;  // FILETIME начала (для поиска отчёта о падении)
};
void StartWatch(DWORD pid, const std::wstring& javaExe, const Instance& inst);
Stats StopWatch();
bool Watching();

struct Crash {
    std::wstring file;  // отчёт
    std::wstring what;  // Description или первая строка ошибки
    std::wstring mods;  // подозреваемые моды, если отчёт их называет
    bool jvm = false;   // упала сама Java (hs_err), чаще драйвер или нехватка памяти
};
bool NewCrash(const std::wstring& gameDir, ULONGLONG sinceWall, Crash& out);

// Подкачка размером «по выбору системы» (растёт прямо во время игры).
bool PagefileAuto();

}  // namespace mc
