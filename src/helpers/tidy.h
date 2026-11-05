#pragma once
// Порядок на Рабочем столе и в снимках (Ф3, вместе с Л40): файлы на Рабочем столе и в «Изображения\Снимки экрана»
// старше 2 недель - в «Архив\2026-10» той же папки (по месяцу файла). Ничего не удаляется. Раз в неделю, если таких
// набралось от 5, - карточка с вопросом; действие tidy - спросить сейчас. Ярлыки, desktop.ini, скрытые и папки не
// трогаются. Опись перенесённого - %APPDATA%\MoonDesk\tidy\<время>.tsv; запись в журнале автоматики можно отменить
// («Отменить» - всё на место, если там свободно). tidy_logic.cpp - что брать и куда (под тестами).
#include <windows.h>

#include <string>
#include <vector>

struct Config;

namespace tidy {

void Init(HINSTANCE inst, const Config* cfg);
void Ask();   // действие tidy: посчитать и спросить
void Tick();  // раз в неделю - сам
void Shutdown();
std::wstring Undo(const std::wstring& manifest);  // вернуть по описи; итог строкой (для журнала)
std::wstring Diag();                              // сколько старых файлов в каждой папке (--tidy-diag)

// --- чистое ---
// брать ли файл: не папка, не скрытый и не системный, не ярлык и не desktop.ini, старше days дней
bool Candidate(const std::wstring& name, DWORD attrs, ULONGLONG ageDays, ULONGLONG days = 14);
std::wstring MonthDir(const std::wstring& folder, const FILETIME& written);  // folder\Архив\2026-10

}  // namespace tidy
