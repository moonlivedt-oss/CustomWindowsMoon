#pragma once
// Утилиты в основной программе: кольцо «Утилиты» (действие угла tools), действия в фоне с ходом в карточке,
// предупреждение, когда на диске мало места.
#include <functional>
#include <string>

#include <windows.h>

struct Config;
class BoostCard;
class RadialMenu;

namespace toolsui {

inline constexpr UINT WM_TOOLS_JOB = WM_APP + 63;  // ход действия: lParam - строка журнала или итог

// openStudio(page): 0 - PATH, 1 - диски, 2 - автозагрузка, 3 - порты; scan - что сразу просканировать
void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below,
          std::function<void(int page, const std::wstring& scan)> openStudio);
void ShowRing(RadialMenu& menu);
void Run(const std::wstring& key);
void OnJob(LPARAM lp);
void Tick();  // карточка и раз в 10 минут - проверка места на дисках
void Shutdown();

}  // namespace toolsui
