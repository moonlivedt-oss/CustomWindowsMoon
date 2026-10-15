#pragma once
#include <functional>
#include <string>

#include <windows.h>

// Radial Studio - окно настройки колец.
// onSaved вызывается после каждой записи radial.cfg (автосохранение),
// showMenu открывает настоящее меню у левого (true) или правого края, рядом с окном студии.
void OpenEditor(HINSTANCE inst, const std::wstring& cfgPath, std::function<void()> onSaved,
                std::function<void(bool left, HWND studio)> showMenu);

// Открытая студия переходит в раздел «Сеть» (из кольца сети: «Проверить сеть»).
void EditorGoNet();
// Раздел «Утилиты»: tab 0 PATH, 1 диски, 2 автозагрузка, 3 порты; scan - сразу посчитать, что занимает место.
void EditorGoTools(int tab, const std::wstring& scan);

// Для цикла сообщений: горячие клавиши студии (Ctrl+Z, Enter в поле).
bool EditorPreTranslate(MSG* m);

// Показать вживую то, что живёт в основной программе (свечение заряда и т.п.): задаёт main.
enum EditorPreview { PREVIEW_BATTERY_LOW = 1, PREVIEW_BATTERY_CHARGE, PREVIEW_LOCK_REVEAL, PREVIEW_GAME_WAVE, PREVIEW_BOOST };
void SetEditorPreview(std::function<void(int what)> f);
// Состояние игрового режима для студии («выключен», «включён: cs2.exe»): задаёт main.
void SetEditorGameStatus(std::function<std::wstring()> f);
