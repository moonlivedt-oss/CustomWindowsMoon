#pragma once
#include <string>

#include <windows.h>

// Настройки модов Windhawk лежат в HKLM, а программа работает без прав администратора.
// Поэтому здесь готовятся .reg-файлы: двойной клик по ним вносит настройки (Windows спросит
// разрешение), после чего мод в Windhawk нужно выключить и включить, чтобы он их перечитал.
namespace whreg {

// Тема значков: путь к папке темы для мода «Resource Redirect».
std::wstring IconThemeReg(const std::wstring& themeFolder);
// Центр уведомлений и быстрые настройки: тема TintedGlass с цветами темы RadialMenu.
std::wstring NotificationReg(COLORREF bg, COLORREF accent);
// Проводник (мод Windows 11 File Explorer Styler): стекло с каплей акцента и акцентные кромки.
std::wstring ExplorerReg(COLORREF bg, COLORREF accent);
// «Параметры» (мод Windows 11 Settings Styler): графитовое стекло, карточки с каплей акцента.
std::wstring SettingsReg(COLORREF bg, COLORREF panel, COLORREF accent);
// Поиск Windows (мод Windows 11 Start Menu Styler): TintedGlass с графитовым стеклом.
std::wstring SearchReg(COLORREF bg, COLORREF accent);
// Всё сразу для тех модов, что установлены; mods - через запятую, что вошло. Пусто - ни одного мода нет.
std::wstring AllReg(COLORREF bg, COLORREF panel, COLORREF accent, std::wstring* mods);
bool ModInstalled(const wchar_t* mod);  // мод есть в Windhawk
// Вернуть прежнюю тему значков (папка, которая стояла до нас).
std::wstring IconThemeRestoreReg(const std::wstring& oldFolder);

// Экран ввода пароля: чёткая картинка блокировки вместо размытой (политика DisableAcrylicBackgroundOnLogon).
std::wstring LogonReg(bool crisp);
bool LogonCrisp();  // сейчас фон экрана входа чёткий

// Открыть .reg (Windows покажет запрос на изменение реестра).
void Run(const std::wstring& regFile);

}  // namespace whreg
