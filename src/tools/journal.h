#pragma once
// Журнал автоматики: строки «когда<TAB>что» в journal.tsv, больше 64 КБ - файл уходит в journal.old.tsv (одна
// старая копия). Без окон, чтобы ротацию проверяли тесты; папку передаёт вызывающий (автоматика - tools::DataDir()).
#include <windows.h>

#include <string>
#include <vector>

namespace tools {

struct JournalEntry {
    std::wstring when, text;  // «16.10 14:05», что сделано
};

void JournalAdd(const std::wstring& dir, const std::wstring& when, const std::wstring& text,
                ULONGLONG limit = 64 * 1024);
std::vector<JournalEntry> JournalRead(const std::wstring& dir, size_t max);  // новые первыми

}  // namespace tools
