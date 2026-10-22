#pragma once
// Журнал автоматики: строки «когда<TAB>что» в journal.tsv, больше 64 КБ - файл уходит в journal.old.tsv (одна
// старая копия). Без окон, чтобы ротацию проверяли тесты; папку передаёт вызывающий (автоматика - tools::DataDir()).
#include <windows.h>

#include <string>
#include <vector>

namespace tools {

struct JournalEntry {
    std::wstring when, text;  // «16.10 14:05», что сделано
    std::wstring undo;        // как отменить (Л3): «вид|аргумент|...», пусто - никак; третий столбец строки
};

void JournalAdd(const std::wstring& dir, const std::wstring& when, const std::wstring& text,
                ULONGLONG limit = 64 * 1024, const std::wstring& undo = L"");

// Отмена записи журнала (Л3): recycle|путь|путь... - вернуть из Корзины, cfg|копия - прежний radial.cfg,
// power|режим - прежний режим питания. Пути не содержат «|», табуляций и переводов строк.
struct Undo {
    std::wstring kind;
    std::vector<std::wstring> args;
};
std::wstring UndoPack(const std::wstring& kind, const std::vector<std::wstring>& args);
bool UndoParse(const std::wstring& s, Undo& u);                              // false - пусто или не разобралось
std::vector<JournalEntry> JournalRead(const std::wstring& dir, size_t max);  // новые первыми

}  // namespace tools
