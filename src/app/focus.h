#pragma once
// Режим фокуса (Л8) и шахматы без отвлечений (Л34). Фокус на N минут (трей, палитра, студия - Помощники):
// уведомления Windows молчат (та же тишина, что в игре), отвлекающие программы (focus_apps: Telegram, Discord)
// свёрнуты и усыплены, тяжёлые фоновые проходы (подсчёт места, «Защита», недельные) ждут, в кольце внизу - сколько
// осталось; в конце - карточка «перерыв» с «Ещё 25 минут». Само: пока открыто окно, в заголовке которого есть
// focus_auto (lichess.org, chess.com), - тишина и ожидание тяжёлого без отсчёта и без усыпления программ; окна нет
// 30 с - выключается. focus_logic.cpp - совпадение заголовков и строка отсчёта (под тестами).
#include <windows.h>

#include <string>
#include <vector>

struct Config;

namespace focus {

// --- без окон (под тестами) ---
// Какая маска из «a|b|c» есть в одном из заголовков (без учёта регистра; ответ строчными); пусто - ни одной
std::wstring AutoMatch(const std::vector<std::wstring>& titles, const std::wstring& masks);
std::wstring LeftText(ULONGLONG msLeft);  // «Фокус: ещё 23 мин», последняя минута - «Фокус: меньше минуты»

// --- в программе ---
void Init(HINSTANCE inst, const Config* cfg);
void Start(int minutes);  // вручную; 0 - из настроек
void Stop();
bool On();
bool Auto();            // включился сам (шахматы)
std::wstring Status();  // для кольца, трея и студии: «Фокус: ещё 23 мин», «Шахматы: тишина»; пусто - выключен
void Tick();
void Shutdown();

}  // namespace focus
