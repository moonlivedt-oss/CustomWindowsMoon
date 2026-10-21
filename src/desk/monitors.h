#pragma once
// Кольцо «Мониторы» (И34, как Power Display в PowerToys): у каждого экрана - яркость ступенями, вход (HDMI, DP, USB-C)
// и «выключить» по DDC/CI; экран ноутбука - яркость через WMI. DDC медленный (десятки мс на команду), поэтому всё -
// в фоне: список с уровнями берётся из кэша (освежается при каждом открытии), команды уходят потоком.
// Действие угла monitors, пункт action:monitors.
#include <string>

class RadialMenu;
struct Config;

namespace monitors {

void Init(const Config* cfg);
void ShowRing(RadialMenu& menu);
std::wstring Diag();  // --monitors-diag: что видно сейчас (синхронно, медленно)
void Run(const wchar_t* key);  // b:<номер>:<%>, in:<номер>:<код VCP 0x60>, off:<номер>

}  // namespace monitors
