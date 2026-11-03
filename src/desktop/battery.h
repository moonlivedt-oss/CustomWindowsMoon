#pragma once
#include <windows.h>

#include "core/themes.h"

struct Config;

// Заряд ноутбука прямо на краю экрана, без окон и уведомлений:
// мало заряда - у нижнего края тихо дышит свечение цвета темы,
// подключили зарядку - по краю слева направо пробегает светлая волна.
class BatteryGlow {
public:
    void Create(HINSTANCE inst);
    void Tick(const Config& cfg);  // из общего опроса раз в 20 мс
    void Test(bool charging);      // показать вживую из студии

private:
    void Paint(float pulse, float wave);
    void Hide();

    HWND wnd_ = nullptr;
    ThemeInfo theme_{};
    bool hex_ = true;  // волна зарядки сотами
    ULONGLONG lastCheck_ = 0, waveAt_ = 0, lastFrame_ = 0, testUntil_ = 0;
    bool hasBattery_ = false, ac_ = true, low_ = false, testCharge_ = false;
    int percent_ = 100;
    float show_ = 0;  // проявление низкого заряда
};
