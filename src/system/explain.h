#pragma once
// «Подробнее» для карточек Boost, кольца процессов и утилит: что это за программа, что именно произойдёт при
// закрытии, усыплении или действии утилит, что можно потерять и как вернуть. Только тексты - без окон и системы,
// под тестами.
#include <string>

namespace explain {

// Что это за программа и чем грозит её закрытие; exe - имя файла в любом регистре («chrome.exe»). Пусто - не знаем.
std::wstring Process(const std::wstring& exe);

// Как закрывается программа (просьба, потом силой) - с описанием самой программы, если она знакома
std::wstring Close(const std::wstring& exe);
// Boost: «Усыпить» и «Оставить» - что станет с программой
std::wstring Sleep(const std::wstring& exe);
std::wstring Keep(const std::wstring& exe);
// Boost перед игрой или редактором: что сделает «Освободить», что вернётся после
std::wstring BoostOffer();

// Действие кольца утилит по его ключу (как в Item::path: «clean:3», «start-off:1», «rec-repair»...). exe - процесс,
// если действие над ним (app:, leak:, hog:, port:). Пусто - объяснять нечего (группа, открыть папку).
std::wstring Action(const std::wstring& key, const std::wstring& exe = L"");

}  // namespace explain
