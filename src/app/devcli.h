#pragma once
#include <string>
#include <vector>

// Служебные режимы запуска RadialMenu.exe: проверки для разработки и очистка памяти для задачи планировщика.
// Каждый делает своё и сразу завершает программу, окно и трей не создаются.
namespace devcli {

bool HasFlag(const wchar_t* flag);  // есть ли аргумент ровно с таким именем (путь к exe не в счёт)
std::vector<std::wstring> ArgsAfter(const wchar_t* flag);  // всё после флага (--to-phone <файлы>); нет флага - пусто

// Выполнить служебный режим, если он есть в командной строке. true - режим отработал, вернуть exitCode.
bool Run(int& exitCode);

}  // namespace devcli
