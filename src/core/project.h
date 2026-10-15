#pragma once
#include <string>

// Что за папка или файл под курсором: свой значок проекта, его тип и строки для инфокарточки.
// Всё кэшируется по пути, диск читается один раз.
namespace project {

enum class Kind { None, Cpp, Python, Web, VsCodeExt, Game, Java, Scripts, Data, Docs };

struct Look {
    std::wstring ownPng;  // собственная картинка проекта (icon.png, logo.png, значок из package.json)
    std::wstring ownIco;  // или .ico - его отдаёт оболочка в нужном размере
    Kind kind = Kind::None;
    wchar_t letter = 0;  // для монограммы, если ни значка, ни типа
};

const Look& LookOf(const std::wstring& dir);
const wchar_t* KindIcon(Kind k);  // значок типа из icons/, без пути
const wchar_t* KindName(Kind k);  // «Проект C++»; для None - nullptr

struct Info {
    std::wstring kind;    // «Папка», «Проект C++», «Программа», «Ярлык», тип файла
    std::wstring detail;  // «23 объекта | изменена 14 окт 2026», «45,2 МБ | ...»
    std::wstring path;    // где лежит (для ярлыка - куда ведёт)
};
// guess - угадывать тип проекта по файлам (для папок без своего значка в настройках)
const Info& InfoOf(const std::wstring& full, bool dir, bool guess = true);
void Forget();      // сбросить всё (перечитали настройки)
void ForgetInfo();  // сбросить карточки (число файлов, размеры) - при каждом открытии меню

}  // namespace project
