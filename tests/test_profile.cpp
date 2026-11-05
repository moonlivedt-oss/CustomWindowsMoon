// Профили без окон: core/profile_logic.
#include <string>

#include "core/profile.h"

#define CHECK(c) check((c), #c, __LINE__)

void TestProfile(void (*check)(bool, const char*, int)) {
    using namespace profile;
    CHECK(Parse(L"simple") == Simple);
    CHECK(Parse(L"dev") == Dev);
    CHECK(Parse(L"expert") == Expert);
    CHECK(Parse(L"") == Expert);        // ещё не выбирали - ничего не прячем
    CHECK(Parse(L"что-то") == Expert);  // испорченный файл настроек не должен прятать настройки
    for (int i = 0; i < kCount; ++i) CHECK(Parse(Key((Level)i)) == (Level)i);

    CHECK(CardMin(L"Где открывать") == Simple);
    CHECK(CardMin(L"Новая карточка из будущего") == Simple);  // незнакомое видно всем
    CHECK(CardMin(L"Режим разработки") == Dev);
    CHECK(CardMin(L"Ожидаемый VPN") == Expert);

    CHECK(ToolsTabMin(0) == Dev);
    CHECK(ToolsTabMin(5) == Dev);
    CHECK(ToolsTabMin(6) == Expert);
    CHECK(ToolsTabMin(7) == Expert);
    CHECK(ToolsRingMin(0) == Simple);
    CHECK(ToolsRingMin(2) == Dev);
    CHECK(ToolsRingMin(3) == Expert);

    Load(L"");
    CHECK(!Chosen());
    CHECK(Current() == Expert);
    CHECK(std::wstring(CurrentKey()).empty());  // не выбирали - в файл ничего не пишется
    Choose(Simple);
    CHECK(Chosen());
    CHECK(Effective() == Simple);
    SetPeek(true);
    CHECK(Effective() == Expert);  // «Показать всё» открывает разделы
    CHECK(Current() == Simple);    // а профиль остаётся
    SetPeek(false);
    Load(L"dev");
    CHECK(Current() == Dev);
    Load(L"");
}
