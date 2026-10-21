#pragma once
// Недавние файлы и папки (И24) - из списка самой Windows (%APPDATA%\Microsoft\Windows\Recent, ярлыки по времени).
// Кольцо «Недавнее» (действие угла recent, пункт action:recent): свежие файлы, сектор «Папки», «Ещё»; строки
// «Недавнее: ...» в палитре. Пропавшие файлы не показываются.
#include <string>
#include <vector>

struct Config;
struct Item;
class RadialMenu;

namespace recent {

struct Entry {
    std::wstring name, path;
    bool dir = false;
    unsigned long long when = 0;  // FILETIME ярлыка: когда открывали
};

std::vector<Entry> List(size_t max = 40);          // свежие первыми, без повторов и пропавших; кэш 10 с
std::vector<Item> Items(const std::wstring& tag);  // для палитры: special 3, ключ tag + путь
void Open(const std::wstring& path);
void ShowRing(RadialMenu& menu);
void Init(const Config* cfg);

}  // namespace recent
