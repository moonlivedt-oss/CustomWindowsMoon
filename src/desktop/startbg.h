#pragma once
#include <string>

#include <windows.h>

#include "core/themes.h"

// Фон меню Пуск (Stardock Start11) из текущих обоев: кусок обоев размывается, притемняется
// цветом темы и снизу получает отсвет акцента. Картинка - в %APPDATA%\MoonDesk\start-bg.jpg,
// путь пишется в настройки Start11, Start11 перезапускается (иначе не перечитает).
namespace startbg {

bool Installed();  // Start11 есть на компьютере
// Всё оформление Пуска в цветах темы: фон из обоев, подложка меню (графит с каплей акцента)
// и кнопка Пуск (серебро и сталь, открытое меню - акцент). message - что получилось, для студии
bool Apply(const ThemeInfo& t, std::wstring& message);

}  // namespace startbg
