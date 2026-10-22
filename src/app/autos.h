#pragma once
// Автоматика: то, что делается само, без похода в кольцо или студию.
//  - тихая чистка раз в неделю и сразу, когда на диске мало места (TEMP старше 6 часов и не занятое, быстрые кэши);
//  - разбор Загрузок раз в неделю: старые установщики, распакованные архивы, копии - карточка «в Корзину?»;
//  - режим питания: от батареи - экономия, на зарядке - сбалансированный, в игре и Unreal - производительность;
//  - самовосстановление: пропал автозапуск - вернуть, пропал radial.cfg - достать из копий, пропал RadialTools.exe -
//    сказать.
// Каждое действие пишется в журнал (%APPDATA%\MoonDesk\tools\journal.tsv), его показывает студия.
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "tools/journal.h"
#include "tools/tools.h"

struct Config;

namespace autos {

inline constexpr UINT WM_AUTO_JOB = WM_APP + 64;

void Init(HINSTANCE inst, HWND app, const Config* cfg, const std::wstring& cfgPath, std::function<void()> reload,
          std::function<void(int page, const std::wstring& scan)> openStudio);
void Tick();
void OnPower();                     // сменился источник питания
void OnModes(bool game, bool dev);  // включился или выключился игровой режим или режим разработки
void OnJob(WPARAM wp, LPARAM lp);
void Shutdown();
void OpenState();  // студия на «Состоянии» (из «Всё спокойно» в кольце утилит)

// для студии
using Entry = tools::JournalEntry;            // «16.10 14:05», что сделано
std::vector<Entry> Journal(size_t max = 60);  // новые сверху
void Note(const std::wstring& text);          // записать в журнал
// Несрочный итог (И30): при включённом дайджесте копится и приходит одной карточкой вечером - тогда true, и
// показывать самому не надо; выключен - false.
bool Digest(const std::wstring& title, const std::wstring& text);
int DigestCount();  // сколько ждёт вечера (для студии)
void CleanNow();                              // тихая чистка прямо сейчас (ход и итог - в карточке)
bool Cleaning();
int LastCleanDays();                           // сколько дней назад была тихая чистка, -1 - не было
ULONGLONG LastCleanFreed();                    // сколько она освободила
std::vector<tools::DownloadJunk> Downloads();  // последний разбор Загрузок
// Для кадра студии: копирует, только если разбор поменялся с прошлого раза (gen - номер у вызывающего, сначала ~0u)
bool DownloadsIfChanged(std::vector<tools::DownloadJunk>& out, unsigned& gen);
void ScanDownloads();  // разобрать заново (в фоне)
void RecycleDownloads(const std::vector<std::wstring>& paths);
std::wstring PowerStatus();  // «экономия, от батареи» - что сейчас и почему
std::wstring RepairStatus();

}  // namespace autos
