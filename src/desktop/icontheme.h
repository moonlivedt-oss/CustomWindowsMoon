#pragma once
#include <functional>
#include <string>

#include <windows.h>

// Тема значков для мода Windhawk «Resource Redirect» под цвета темы RadialMenu.
// Берётся текущая тема мода (папка themeFolder из его настроек), копируется,
// и во всех её DLL сиреневые значки перекрашиваются: корпус папок - в графит,
// более тёмные детали (рисунки на папках) - в цвет акцента. Остальные значки не трогаются.
// Работа долгая (десятки секунд): запускать в своём потоке.
namespace icontheme {

std::wstring SourceFolder();  // папка текущей темы мода, пусто - мод не настроен
// body - цвет корпуса, accent - цвет деталей; out - куда легла новая тема
bool Build(COLORREF body, COLORREF accent, const std::wstring& name, std::wstring& out, std::wstring& message,
           const std::function<void(int percent)>& progress);

}  // namespace icontheme
