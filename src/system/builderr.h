#pragma once
// Сломанная сборка понятным языком (Л31): первая настоящая ошибка из вывода сборки - Unreal (лог UBT), MSVC, GCC и
// Clang, линкер, CMake - с файлом и строкой, короткое объяснение и что обычно помогает. Без окон и файлов, под
// тестами; где взять вывод и куда показать - gameboost (UBT) и toolsui_guard (cmake --build).
#include <string>

namespace builderr {

struct Error {
    std::wstring file;  // как в выводе (может быть относительным - к папке сборки)
    int line = 0, col = 0;
    std::wstring code;  // C2065, LNK2019; пусто - у GCC и Clang
    std::wstring text;  // сообщение компилятора
};

// Первая настоящая ошибка: сперва - с файлом и строкой, если таких нет - линкер и прочие «error»; итоговые строки
// («ld returned 1», «build stopped», «FAILED:», «Result: Failed») не в счёт. false - ошибок не нашлось.
bool First(const std::wstring& log, Error& e);
// «Player.cpp, строка 42: 'Foo': undeclared identifier»
std::wstring Plain(const Error& e);
// Что обычно помогает, по коду и словам; пусто - сказать нечего
std::wstring Hint(const Error& e);
// Для «code -g»: «полный\путь:строка:столбец»; base - папка, от которой считать относительный путь
std::wstring GotoArg(const Error& e, const std::wstring& base);

}  // namespace builderr
