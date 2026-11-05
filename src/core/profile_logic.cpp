#include "core/profile.h"

#include <atomic>

namespace profile {

namespace {
std::atomic<int> g_level{Expert};
std::atomic<bool> g_chosen{false}, g_peek{false};

struct CardRule {
    const wchar_t* title;
    Level min;
};
// Только то, что прячется. Заголовки - как в карточках студии.
const CardRule kCards[] = {
    // Поведение
    {L"Действия", Dev},
    {L"Перенос на другой ПК", Dev},
    // Красота
    {L"Режим разработки", Dev},
    {L"Показ экрана", Dev},
    {L"Во время игры", Dev},
    {L"Экран блокировки", Expert},
    {L"Ввод пароля", Expert},
    {L"Сотовое стекло", Expert},
    // Система
    {L"Значки папок", Expert},
    {L"Центр уведомлений", Expert},
    {L"Проводник", Expert},
    {L"Меню Пуск", Expert},
    {L"Курсор", Expert},
    // Сеть
    {L"Скорость", Dev},
    {L"Wi-Fi", Dev},
    {L"Сторож", Dev},
    {L"Кто качает", Dev},
    {L"История", Dev},
    {L"Права администратора", Expert},
    {L"Защита и фоновые передачи", Expert},
    {L"Ожидаемый VPN", Expert},
    {L"Программы защиты VPN", Expert},
    {L"Состояние защиты", Expert},
    {L"Прокси-процессы", Expert},
    // Диски
    {L"Найти папку", Expert},
    {L"Системные гиганты", Expert},
    {L"Большие файлы", Expert},
    {L"Физические диски", Expert},
    // Помощники
    {L"Росчерк", Expert},
    {L"Правила процессов", Expert},
    {L"Если - то", Expert},
    {L"Геймпад", Expert},
    {L"Тихий баланс процессора", Expert},
    {L"Громкость по программам", Expert},
    // Автоматика
    {L"Обновление программ ночью", Expert},
    {L"Режим питания", Expert},
    {L"Дайджест", Expert},
    {L"Самовосстановление", Expert},
    {L"Отчёт за неделю", Expert},
};
}  // namespace

Level Parse(const std::wstring& key) {
    if (key == L"simple") return Simple;
    if (key == L"dev") return Dev;
    return Expert;
}

const wchar_t* Key(Level l) { return l == Simple ? L"simple" : l == Dev ? L"dev" : L"expert"; }

const wchar_t* Name(Level l) { return l == Simple ? L"Упрощённый" : l == Dev ? L"Разработчик" : L"Специалист"; }

const wchar_t* Blurb(Level l) {
    return l == Simple ? L"Кольца, темы и самое нужное"
           : l == Dev  ? L"Плюс проекты, порты, PATH и сборки"
                       : L"Всё, что умеет программа";
}

const wchar_t* Holds(Level l) {
    return l == Simple ? L"Состояние, кольца, темы, углы, красота и проверка сети. Меню в трее короче, в кольце утилит "
                         L"только место и память."
           : l == Dev ? L"К упрощённому добавляются Программы, Автоматика, Утилиты, Система и Помощники, режим "
                        L"разработки, скорость сети и группа «Разработка» в кольце утилит."
                      : L"Без скрытых разделов: защита, приватность, VPN, экран блокировки, правила процессов, "
                        L"оформление Windows и все группы кольца утилит.";
}

Level CardMin(const std::wstring& title) {
    for (const CardRule& r : kCards)
        if (title == r.title) return r.min;
    return Simple;
}

Level ToolsTabMin(int tab) {
    return tab >= 6 ? Expert : Dev;  // «Защита» и «Приватность»
}

Level ToolsRingMin(int group) {
    if (group <= 1) return Simple;  // Место, Память
    return group == 2 ? Dev : Expert;
}

void Load(const std::wstring& key) {
    g_chosen = !key.empty();
    g_level = Parse(key);
}

void Choose(Level l) {
    g_level = l;
    g_chosen = true;
}

bool Chosen() { return g_chosen; }

Level Current() { return (Level)g_level.load(); }

const wchar_t* CurrentKey() { return g_chosen ? Key(Current()) : L""; }

void SetPeek(bool on) { g_peek = on; }

bool Peek() { return g_peek; }

Level Effective() { return g_peek ? Expert : Current(); }

}  // namespace profile
