#pragma once
// Скриншот в работу (Л29): понятное имя снимка по окну, из которого снимали, и куда его положить. Чистые функции
// (под тестами); окно и его папку находит карточка снимка (desk/snip) через подсказку программы (resume).
#include <windows.h>

#include <string>

namespace snipname {

// Что за окно - коротко, для имени: «Player.cpp - MyGame - Visual Studio Code» -> «MyGame Player.cpp»,
// «Лекция 5 - YouTube - Google Chrome» -> «Лекция 5 - YouTube», папка Проводника - её имя
std::wstring Context(const std::wstring& title, const std::wstring& exe);
// Имя файла: «MyGame Player.cpp 2026-10-23 10-41.png» - без запрещённых знаков, до 80 знаков; пустой контекст -
// «Снимок 2026-10-23 10-41-05.png»
std::wstring FileName(const std::wstring& context, const SYSTEMTIME& t);
// Куда: папка проекта (есть .git, package.json, CMakeLists.txt, .uproject...) - её «screenshots», иначе сама папка
std::wstring TargetDir(const std::wstring& folder, bool isProject);

}  // namespace snipname
