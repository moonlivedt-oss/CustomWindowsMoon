#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

struct Config;

// Игровой режим: пока запущена игра (по маскам путей из radial.cfg) или что-то идёт в эксклюзивном
// полноэкранном режиме, вся красота выключается: анимации, стекло, свечения, рамки окон, перехват мыши.
// По желанию заодно выключаются анимации и прозрачность самой Windows и ставится на паузу Wallpaper Engine.
// Что было изменено в системе, записывается в %APPDATA%\MoonDesk\gamemode.ini: если программа упадёт,
// при следующем запуске всё вернётся.
class GameMode {
public:
    std::function<void(bool on)> onChange;

    void Recover();                // при запуске: вернуть то, что осталось от прошлого раза
    void Tick(const Config& cfg);  // из общего опроса; сама проверка раз в 2 секунды
    void SetManual(bool on, const Config& cfg);
    bool Manual() const { return manual_; }
    bool On() const { return on_; }
    const std::wstring& Reason() const { return reason_; }  // что включило режим: имя игры или «вручную»
    std::vector<DWORD> GamePids() const;  // процессы игр; для полноэкранной без маски - процесс активного окна
    void Shutdown();                                        // при выходе: вернуть систему как была

private:
    bool Detect(const Config& cfg, std::wstring& name);
    void Update(const Config& cfg, bool want);
    void Enter(const Config& cfg);
    void Leave();

    bool on_ = false, manual_ = false, detected_ = false, suppress_ = false;
    std::wstring reason_;
    ULONGLONG lastCheck_ = 0;
    int misses_ = 0;
    std::map<DWORD, std::wstring> games_;  // pid -> имя файла игры или пусто (уже проверен, не игра)
    // что поменяли в системе
    bool changedAnim_ = false, changedTransp_ = false;
    std::wstring pausedWallpaper_;  // путь к wallpaper64.exe, если ставили на паузу
};
