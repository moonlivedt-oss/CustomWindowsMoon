#pragma once
// Сеть в основной программе: сторож связи, карточка «Пропал интернет - починить?», кольцо «Сеть»
// (действие угла), действия в фоне с ходом в карточке, проверка маршрута при запуске игры.
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

#include "net/net.h"

struct Config;
struct Item;
class BoostCard;
class RadialMenu;

namespace netui {

inline constexpr UINT WM_NET_EVENT = WM_APP + 61;  // событие сторожа или проверки игры: lParam - net::Event*
inline constexpr UINT WM_NET_JOB = WM_APP + 62;    // ход действия: lParam - строка журнала или итог

void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below, std::function<void()> openStudio);
void Apply(const Config& cfg);      // настройки сторожа после перечитывания radial.cfg
void ShowRing(RadialMenu& menu);    // кольцо «Сеть» у курсора (обычно - в углу)
std::vector<Item> RingItems();      // пункты кольца «Сеть» без показа (для командной палитры)
void Run(const std::wstring& key);  // действие по ключу: smart, proxy, vpn, ip, dns-cf, kill:123, ping:1.1.1.1...
void OnEvent(LPARAM lp);
void OnJob(LPARAM lp);
void Game(bool on, const std::vector<DWORD>& pids, const std::wstring& name);
void Dev(bool on);  // режим разработки (Unreal): тихий замер скорости не мешает сборке
void Wake();  // после сна и разблокировки: проверить сразу, но обрыв какое-то время не считать
void Tick();
void Shutdown();
std::wstring Status();  // для кольца и подсказки трея
// Кто качает (И15): последний замер трафика по программам (задача с правами, 3 с TCP EStats); Top() - замерить
// заново (ход и итог в карточке). Отрезать (И16): исходящее правило брандмауэра на exe, с вопросом; Uncut - вернуть.
struct TopRow {
    std::wstring name, exe;
    ULONGLONG down = 0, up = 0;  // байт/с
};
std::vector<TopRow> TopRows(ULONGLONG* at = nullptr);  // at - когда замерено (GetTickCount64), 0 - не замеряли
void Top();
void Cut(const std::wstring& exe);
void Uncut(const std::wstring& exe);
std::vector<net::CutApp> CutList();  // отрезанные - из кэша (освежается в фоне раз в 5 с), без задержки кадра

}  // namespace netui
