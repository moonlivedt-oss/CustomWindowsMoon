#pragma once
// Кольцо «Раскладка»: активное окно - в половину, треть, четверть или середину монитора; несколько последних окон
// монитора - рядом, колонками, «главное и два», четвертями (геометрия - desk/zones). Действие угла layout, пункт
// кольца action:layout, строки «Окно: ...» и «Раскладка: ...» в командной палитре.
#include <string>
#include <vector>

#include <windows.h>

struct Config;
struct Item;
class RadialMenu;

namespace layout {

void Init(const Config* cfg);
void ShowRing(RadialMenu& menu);
void Run(const std::wstring& key);                 // z:left, l:3 ...; окно - то, что было впереди
std::vector<Item> Items(const std::wstring& tag);  // для палитры
// Поставить окно в рамку (по видимой рамке DWM, без невидимых полей); развёрнутое сначала восстанавливается.
bool Place(HWND w, const RECT& r);
HWND Target();  // окно, с которым работает кольцо: что было впереди, когда его открыли

}  // namespace layout
