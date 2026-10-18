#pragma once
// Командная палитра: по сочетанию (palette_key, по умолчанию Alt+Space) целое кольцо у курсора сразу ждёт
// ввода и ищет по всем пунктам колец, действиям утилит и сети, разделам студии, открытым окнам и командам
// программы, что впереди; Enter - выполнить. Поиск нечёткий, раскладка RU/EN не важна (core/fuzzy).
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

struct Config;
struct Item;
class RadialMenu;

namespace palette {

inline constexpr int kHotId = 2;  // WM_HOTKEY

void Init(HWND app, const Config* cfg, RadialMenu* menu, std::function<void(int page)> openPage);
// (пере)регистрировать сочетание по настройкам; false - занято другой программой или не понято
bool Apply();
void Show();
// дополнительные пункты (история буфера, папки из таблицы MFT): их даёт тот, кто их знает
void AddSource(std::function<void(std::vector<Item>&)> source);
// кто выполняет пункты с ключом prefix... (пункты special 3 из AddSource)
void AddRunner(const std::wstring& prefix, std::function<void(const std::wstring& rest)> run);
std::wstring Status();  // для студии: «Alt + Space» или почему не работает

}  // namespace palette
