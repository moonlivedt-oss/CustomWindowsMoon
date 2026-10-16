#pragma once
// Утилиты в основной программе: кольцо «Утилиты» (действие угла tools), действия в фоне с ходом в карточке,
// предупреждение, когда на диске мало места.
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

struct Config;
class BoostCard;
class RadialMenu;
#include "tools/tools.h"

namespace toolsui {

inline constexpr UINT WM_TOOLS_JOB = WM_APP + 63;  // ход действия: lParam - строка журнала или итог

// openStudio(page): 0 - PATH, 1 - диски, 2 - автозагрузка, 3 - порты; scan - что сразу просканировать
void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below,
          std::function<void(int page, const std::wstring& scan)> openStudio);
void ShowRing(RadialMenu& menu);
void Run(const std::wstring& key);
void OnJob(WPARAM wp, LPARAM lp);
void Tick();  // карточка и раз в 10 минут - проверка места на дисках
void Shutdown();
void OnModes(bool game, bool dev);  // игровой режим и Unreal: в игре карточки молчат, после Unreal - хвосты

// Где искать мусор проектов: папки из колец (radial.cfg) и обычные места проектов (source/repos, Unreal Projects).
std::vector<std::wstring> ProjectRoots(const Config& cfg);

// Мало памяти перед игрой или редактором (когда Boost не настроен или ему нечего закрыть): карточка с самыми
// тяжёлыми программами и «Закрыть» для первой. keep - что не предлагать (сама игра, редактор). force - показать,
// даже если памяти хватает (вызов вручную). true - карточка показана.
bool MemoryOffer(const std::wstring& why, const std::vector<std::wstring>& keep, bool force);
void CloseHeavy(const tools::HeavyApp& app);  // мягко закрыть программу в фоне, ход - в карточке

// Для страницы «Состояние» в студии: последний фоновый подсчёт и действия над ним.
struct State {
    std::vector<tools::Junk> junk;
    std::vector<tools::ProjectJunk> projects;
    std::vector<tools::Growth> growth;
    ULONGLONG ddcOld = 0;
    tools::Driver driver;
    ULONGLONG at = 0;       // когда посчитано (GetTickCount64), 0 - ещё ни разу
    bool scanning = false;  // считается прямо сейчас
};
State Snapshot();
void Refresh();  // пересчитать сейчас
void PrimeForSnapshot(const Config* cfg);  // для --studio-snap: посчитать сразу и дождаться
void CleanQuick();  // «Всё сразу»: быстрые кэши и временные файлы
void TrimDdc();     // старое из кэша Unreal
void CleanProjectList(const std::vector<tools::ProjectJunk>& list);
bool Busy();

}  // namespace toolsui
