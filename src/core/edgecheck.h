#pragma once
// Что мешает кольцам у края экрана: PowerToys CursorWrap (с 0.97) переносит курсор через край на другую сторону -
// до края он не доходит, и кольцо не откроется. Проверка дешёвая (файл настроек + список процессов), с кэшем.
#include <string>

namespace edgecheck {

// Пусто - всё хорошо; иначе - что мешает и что сделать (одной фразой для студии и карточки).
std::wstring Conflict();
// Разбор settings.json PowerToys: включён ли модуль name в разделе "enabled" (для тестов - без файла).
bool PowerToysEnabled(const std::string& json, const char* name);

}  // namespace edgecheck
