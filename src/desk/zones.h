#pragma once
// Раскладки окон (как FancyZones): куда поставить активное окно (половина, треть, четверть, середина) и как
// разложить несколько последних окон монитора (2 рядом, 3 колонки, главное + 2, 4 четверти). Здесь только
// геометрия - без окон, под тестами; ставит окна desk/layout.
#include <string>
#include <vector>

#include <windows.h>

namespace zones {

struct Zone {
    const wchar_t* key;   // z:left ...
    const wchar_t* name;  // для кольца
    const wchar_t* note;
};
const std::vector<Zone>& OneWindow();  // зоны для одного (активного) окна
const std::vector<Zone>& Layouts();    // раскладки нескольких окон

// Рамка зоны key в рабочей области (без панели задач) с зазором gap между окнами и от краёв; пусто - не знаю.
bool Rect(const std::wstring& key, const RECT& work, int gap, RECT& out);
// Раскладка key: рамки по порядку (первое - самое недавнее окно); сколько окон ей нужно - размер ответа.
std::vector<RECT> Layout(const std::wstring& key, const RECT& work, int gap);

// Alt + правая: какие края окна тянуть - по тому, в какой трети окна нажали (0 - левый/верхний край,
// 2 - правый/нижний, 1 - середина: этот край не тянется). Для середины по обеим осям - правый нижний угол.
void ResizeEdges(const RECT& win, POINT at, int& ex, int& ey);
// Новая рамка при тяге краёв на (dx, dy) с наименьшим размером minW x minH.
RECT Resized(const RECT& start, int ex, int ey, int dx, int dy, int minW, int minH);

}  // namespace zones
