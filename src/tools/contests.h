#pragma once
// Сроки из ContestReminder (Л7): ближайшие дедлайны конкурсов - в «Важном» кольца утилит и на «Состоянии», когда до
// срока меньше трёх дней. Данные - из самой программы-напоминалки (%APPDATA%\ContestReminder\contests.json), без
// своей копии: что отметили там выполненным или закрыли - здесь пропадает само. Разбор и выбор - под тестами.
#include <windows.h>

#include <string>
#include <vector>

namespace tools {

struct Contest {
    std::wstring title, url;
    LONGLONG deadline = 0;  // секунды unix (в файле - местное время)
    bool done = false;
};

std::vector<Contest> ParseContests(const std::wstring& json);
// Не выполненные, ещё не прошедшие и ближе days дней - самые срочные первыми
std::vector<Contest> SoonContests(const std::vector<Contest>& all, LONGLONG now, int days = 3);
std::wstring DeadlineLeft(LONGLONG deadline, LONGLONG now);  // «через 2 дн. 13 ч», «через 5 ч», «через 40 мин»
std::vector<Contest> ContestsSoon();                         // с диска, на сейчас

}  // namespace tools
