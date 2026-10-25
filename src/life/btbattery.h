#pragma once
// Заряд Bluetooth-наушников (Ж4): раз в минуту - уровень заряда подключённых Bluetooth-устройств из свойства, которое
// Windows показывает в Параметрах (DEVPKEY_Bluetooth_Battery, без прав). Есть - строкой в подсказке значка
// RadialMenu («realme Buds Air7 Pro: 64%»); упал до 15% - карточка, один раз (снова - когда зарядили выше 25%).
// btbattery_logic.cpp - имена и порог (под тестами).
#include <windows.h>

#include <map>
#include <string>
#include <vector>

struct Config;

namespace btbattery {

void Init(HINSTANCE inst, const Config* cfg, void (*changed)());  // changed - обновить подсказку значка
void Tick();
void Shutdown();
std::wstring Status();  // «realme Buds Air7 Pro: 64%» / пусто
std::wstring Diag();    // прочитать сейчас (--bt-diag)

// --- чистое ---
// «realme Buds Air7 Pro Hands-Free», «Транспорт AVRCP realme Buds Air7 Pro» -> «realme Buds Air7 Pro»
std::wstring BaseName(const std::wstring& deviceName);
class LowWatch {  // true - пора сказать «мало заряда»
public:
    bool Feed(const std::wstring& name, int percent);

private:
    std::map<std::wstring, bool> told_;
};
std::wstring Line(const std::vector<std::pair<std::wstring, int>>& levels);  // «A: 64%, B: 20%»

}  // namespace btbattery
