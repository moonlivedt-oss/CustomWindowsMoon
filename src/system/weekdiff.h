#pragma once
// «Что изменилось с прошлой недели» (И36): раз в сутки снимок того, что стоит и запускается (программы из «Программ и
// компонентов», автозапуск, службы, драйверы, задачи планировщика - не Microsoft), в %APPDATA%\MoonDesk\tools\week\
// <ГГГГММДД>.tsv (15 дней). Сравнение - с самым свежим снимком не моложе недели (если его нет - с самым старым).
#include <windows.h>

#include <string>
#include <vector>

namespace tools {

struct WeekEntry {
    std::wstring kind;   // app / start / svc / drv / task
    std::wstring key;    // по нему сравниваем
    std::wstring label;  // как показать; у программы - с версией (поменялась - «обновлено»)
};
struct WeekChange {
    enum What { ADDED, REMOVED, UPDATED } what = ADDED;
    WeekEntry entry;
    std::wstring was;  // для UPDATED - прежняя подпись
};

std::vector<WeekEntry> WeekSnapshot();  // сейчас (секунды: службы и задачи с подписями из кэша)
std::wstring WeekRows(const std::vector<WeekEntry>& s);
std::vector<WeekEntry> ParseWeekRows(const std::wstring& text);
std::vector<WeekChange> WeekDiff(const std::vector<WeekEntry>& old, const std::vector<WeekEntry>& now);
const wchar_t* WeekKindName(const std::wstring& kind);  // «программа», «служба»...

struct WeekReport {
    int days = 0;  // с какого дня сравниваем (0 - сравнивать ещё не с чем)
    std::vector<WeekChange> changes;
};
// Записать сегодняшний снимок (если сегодня ещё не писали) и сравнить с неделей назад. Из фонового потока.
WeekReport WeekRecordAndCompare();

}  // namespace tools
