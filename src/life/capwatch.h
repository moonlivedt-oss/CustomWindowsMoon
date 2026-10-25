#pragma once
// Кто слушает микрофон или смотрит в камеру (Ж5, вместе с Б2): Windows сама отмечает в реестре (ConsentStore), какая
// программа и когда брала камеру и микрофон. Программа ждёт перемен в этом ключе (без опроса) и, когда программа
// включает микрофон или камеру впервые, показывает карточку: кто, путь. Приложение Магазина можно сразу «Запретить»
// (его отдельный переключатель в HKCU, без прав; вернуть - в Параметрах); обычной программе Windows отдельного
// запрета не даёт - «Параметры» открывают нужную страницу. Знакомые пары программа+устройство -
// %APPDATA%\MoonDesk\capseen.tsv; при первом запуске всё, что уже было, считается знакомым. Журнал - «Защита»,
// пункт «Кто брал камеру и микрофон» (кольцо утилит). capwatch_logic.cpp - чистое (под тестами).
#include <windows.h>

#include <set>
#include <string>
#include <vector>

struct Config;

namespace capwatch {

void Init(HINSTANCE inst, const Config* cfg);
void Tick();
void Shutdown();

// --- чистое ---
struct Use {
    std::wstring cap, app, key, path;  // «микрофон», имя, ключ ConsentStore, путь (у обычных программ)
    bool inUse = false, packaged = false;
};
std::wstring SeenKey(const Use& u);  // «микрофон|discord.exe»
// впервые включённые сейчас: в работе и не в seen
std::vector<Use> Fresh(const std::vector<Use>& now, const std::set<std::wstring>& seen);

}  // namespace capwatch
