#pragma once
#include <vector>

#include <windows.h>

#include "core/config.h"

// Открытые окна для кольца окон: то же, что показывает Alt+Tab на текущем рабочем столе.
namespace winlist {

// Секторы по программам в порядке последнего использования. У программы с одним окном сектор и есть окно,
// у программы с несколькими окнами - второе кольцо с заголовками. maxRoots - сколько программ влезает в кольцо.
std::vector<Item> Build(int maxRoots);
void Activate(HWND w);  // развернуть, если свёрнуто, и вывести вперёд
void Close(HWND w);     // вежливо попросить закрыться (как крестик)
std::wstring Title(HWND w);

}  // namespace winlist
