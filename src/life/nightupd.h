#pragma once
// Обновление программ ночью (Е1): winget сам, раз в сутки с 02:00 до 05:30, только от сети, когда компьютер 30 минут
// никто не трогает и нет игры. Каждый пакет - «winget upgrade --id ... -e --silent», по одному, до 15 минут на пакет.
// Соглашения пакетов не принимаются за вас (winget_night_accept = 0): такие пакеты остаются «обновить вручную» -
// окно Л12 в студии. Исключения - winget_skip в radial.cfg (ИД или имя, маски *, через |). Утром, когда вы
// вернулись, - карточка: что обновилось, что нет. Итог - в журнале и %APPDATA%\MoonDesk\winget-night.tsv.
// Включается в студии (Состояние - Автоматика, «Обновлять программы ночью»), по умолчанию выключено.
#include <windows.h>

#include <string>
#include <vector>

struct Config;

namespace nightupd {

void Init(HINSTANCE inst, const Config* cfg, bool (*busy)());
void Tick();
void Shutdown();
std::wstring Status();  // «последний раз: 25.10, обновлено 4, не вышло 1» / «ещё не было»
void RunNow();          // для проверки: не ждать ночи (условия питания и простоя всё равно проверяются)

// --- чистое ---
bool InWindow(int hour, int minute);  // 02:00-05:30
bool Skipped(const std::wstring& id, const std::wstring& name, const std::wstring& masks);
struct Result {
    std::wstring name, id;
    int code = 0;      // код winget: 0 - обновлено, -1 - не уложился во время
    std::wstring why;  // почему не вышло (Why)
};
// «нужно согласие», «программа запущена», «нужна перезагрузка»...; out - вывод winget (там тоже видно согласие)
std::wstring Why(int code, const std::wstring& out);
std::wstring Summary(const std::vector<Result>& r);  // «обновлено 4: A, B, C...; не вышло 1: D (нужно согласие)»

}  // namespace nightupd
