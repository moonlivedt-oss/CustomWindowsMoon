#pragma once
// Подробности при наведении: что делает настройка и с чем связана. Страницы показывают только короткие подписи.
#include <string>

namespace studio {

struct Help {
    const wchar_t* what;   // что делает
    const wchar_t* links;  // с чем связано: другие части программы или Windows; может быть пустым
};

// По подписи строки или заголовку карточки; nullptr - подробностей нет.
const Help* FindHelp(const std::wstring& label);
// Группа в левой панели и вкладка над страницей: по имени.
const wchar_t* AreaBlurb(const std::wstring& name);

}  // namespace studio
