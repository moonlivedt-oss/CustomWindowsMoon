#pragma once
#include <string>
#include <vector>

struct Item;

// Пути и файлы: папка программы, подстановки {desktop} и %VAR%, проверки, чтение папок.
std::wstring ExeDir();
std::wstring ExpandPath(const std::wstring& p);   // %VAR% и {desktop} {downloads} {documents} {music} ...
std::wstring ResolveIcon(const std::wstring& p);  // относительный путь значка - от папки программы
bool IsFolder(const std::wstring& full);
bool Exists(const std::wstring& p);
bool IsImageFile(const std::wstring& name);
std::wstring ParentDir(const std::wstring& p);
std::wstring ShortPath(const std::wstring& full);  // внутри папки программы - относительный

// Содержимое папки: сначала папки, потом ярлыки, потом (при files) обычные файлы.
std::vector<Item> ListFolder(const std::wstring& full, bool files);
