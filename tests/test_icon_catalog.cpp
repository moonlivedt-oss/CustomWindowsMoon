// Каталог значков студии (studio/icon_catalog.h): темы, поиск на двух языках, рекомендации
#include "studio/icon_catalog.h"

namespace {

void (*g_check)(bool, const char*, int) = nullptr;
#define CHECK(x) g_check((x), #x, __LINE__)

}  // namespace

void TestIconCatalog(void (*check)(bool, const char*, int)) {
    g_check = check;
    using namespace studio::icon_catalog;
    CHECK(Topic(L"УЧЁБА D:/notes") == 3);
    CHECK(Topic(L"МУЗЫКА") == 5);
    CHECK(Topic(L"Игры Steam") == 4);
    CHECK(Topic(L"Сеть WiFi") == 7);
    CHECK(Topic(L"Проекты D:/work") == 2);
    CHECK(Topic(L"Разное") == 0);
    auto book = Describe(L"illustrated/moon/study.png");
    CHECK(book.topic == 3 && Matches(book, L"study.png", L"КНИГА"));
    CHECK(Matches(book, L"study.png", L"study book"));
    CHECK(!Matches(book, L"study.png", L"study музыка"));
    CHECK(Score(book, L"study.png", L"Учёба") > Score(Describe(L"game.png"), L"game.png", L"Учёба"));
    CHECK(Score(Describe(L"proj-moon-core.png"), L"proj-moon-core.png", L"D:/moon-core") >= 250);
    // Пустое имя проекта не «совпадает» с любым контекстом
    CHECK(Score(Describe(L"proj-.png"), L"proj-.png", L"D:/anything") < 250);
    CHECK(Normalize(L"ICONS\\ЁЛКА.PNG") == L"icons/елка.png");
    CHECK(Score(Describe(L"folder.png"), L"folder.png", L"Разное") > 0);
    CHECK(Score(Describe(L"projects.png"), L"projects.png", L"Проекты") >
          Score(Describe(L"code.png"), L"code.png", L"Проекты"));
}
