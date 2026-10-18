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
// Раздел «Состояние»; focus - к какой карточке прокрутить («downloads», «projects» или пусто).
void EditorGoState(const std::wstring& focus);

// Для проверки без окна: раздел (page - номер из enum Page, tab - вкладка утилит) в PNG.
int EditorSnapshot(HINSTANCE inst, const std::wstring& cfgPath, int page, int tab, const std::wstring& out, int ms);

// Для цикла сообщений: горячие клавиши студии (Ctrl+Z, Enter в поле).
bool EditorPreTranslate(MSG* m);

// Показать вживую то, что живёт в основной программе (свечение заряда и т.п.): задаёт main.
enum EditorPreview { PREVIEW_BATTERY_LOW = 1, PREVIEW_BATTERY_CHARGE, PREVIEW_LOCK_REVEAL, PREVIEW_GAME_WAVE, PREVIEW_BOOST };
void SetEditorPreview(std::function<void(int what)> f);
// Состояние игрового режима для студии («выключен», «включён: cs2.exe»): задаёт main.
void SetEditorGameStatus(std::function<std::wstring()> f);
// Включить или выключить игровой режим вручную (щелчок в «Обзоре»): задаёт main.
void SetEditorGameToggle(std::function<void()> f);
// То же для режима разработки («включён (Unreal Editor)»).
void SetEditorDevStatus(std::function<std::wstring()> f);
// Строка состояния сети для «Обзора» («Интернет есть | Швеция | 190 мс»): задаёт main.
void SetEditorNetStatus(std::function<std::wstring()> f);
