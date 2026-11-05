#pragma once
// Профили: кто сейчас работает с программой - что видно в студии, трее и кольце утилит.
// Профиль только прячет: скрытое продолжает работать как настроено и находится через палитру команд.
#include <string>

namespace profile {

enum Level { Simple = 0, Dev = 1, Expert = 2 };
constexpr int kCount = 3;

Level Parse(const std::wstring& key);  // неизвестное и пустое - Expert: ничего не прячем
const wchar_t* Key(Level l);           // как пишется в radial.cfg
const wchar_t* Name(Level l);          // «Упрощённый»
const wchar_t* Blurb(Level l);         // одна строка на экране входа
const wchar_t* Holds(Level l);         // подробности: что внутри

// Минимальный профиль для карточки по её заголовку. Заголовка нет в таблице - карточка видна всем:
// новая настройка не пропадёт молча.
Level CardMin(const std::wstring& title);
Level ToolsTabMin(int tab);     // вкладки «Утилит» по порядку: PATH, Диски, ... Приватность
Level ToolsRingMin(int group);  // группы кольца утилит: Место, Память, Разработка, Защита, Починка, Система

// Текущий выбор. Один на процесс: его читают трей, кольцо утилит и студия.
void Load(const std::wstring& key);  // из radial.cfg; пусто - ещё не выбирали
void Choose(Level l);
bool Chosen();
Level Current();
const wchar_t* CurrentKey();  // пусто, если ещё не выбирали

// «Показать всё» в студии: на время открытого окна, профиль не меняется.
void SetPeek(bool on);
bool Peek();
Level Effective();  // что показывать в студии с учётом «Показать всё»

}  // namespace profile
