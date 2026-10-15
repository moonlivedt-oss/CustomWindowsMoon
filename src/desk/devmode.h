#pragma once
#include <functional>
#include <map>
#include <string>

#include <windows.h>

struct Config;

// Режим разработки: пока открыт тяжёлый редактор (Unreal Editor - по маскам dev_apps из radial.cfg),
// ему отдаётся память и видеокарта: Wallpaper Engine на паузе, при запуске - карточка «освободить память».
// В отличие от игрового режима красота и перехват мыши остаются: углы и кольца нужны и во время работы.
// Поставленные на паузу обои записываются в %APPDATA%\MoonDesk\devmode.ini: упадёт программа - при запуске
// обои вернутся.
class DevMode {
public:
    std::function<void(bool on)> onChange;
    std::function<bool()> gameOn;  // игровой режим держит обои сам: снимать с паузы, только когда он выключен

    void Recover();
    void Tick(const Config& cfg);  // из общего опроса; сама проверка раз в 3 секунды
    void SetManual(bool on, const Config& cfg);
    void Reapply(const Config& cfg);  // игровой режим кончился и вернул обои - снова пауза, если режим идёт
    bool Manual() const { return manual_; }
    bool On() const { return on_; }
    const std::wstring& Reason() const { return reason_; }  // имя редактора или «вручную»
    void Shutdown();

private:
    bool Detect(const Config& cfg, std::wstring& name);
    void Update(const Config& cfg, bool want);
    void Enter(const Config& cfg);
    void Leave();

    bool on_ = false, manual_ = false, detected_ = false, suppress_ = false;
    std::wstring reason_;
    ULONGLONG lastCheck_ = 0;
    int misses_ = 0;
    struct Seen {
        std::wstring exe, app;  // имя файла процесса и, если подошёл по маске, имя редактора
    };
    std::map<DWORD, Seen> seen_;
    std::wstring masks_;
    std::wstring pausedWallpaper_;
};
