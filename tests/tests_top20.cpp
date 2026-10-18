// Тесты пунктов «топ-20» (NEXT.md, И1-И20): разбор и правила без окон - макросы, нечёткий поиск, росчерк,
// секреты в буфере, раскладки, прогноз места, пороги сторожей, резервная копия, безопасный запуск.
#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

#include "core/fuzzy.h"
#include "core/macro.h"
#include "core/stroke.h"

namespace {

void (*g_check)(bool, const char*, int) = nullptr;
#define CHECK(x) g_check((x), #x, __LINE__)

void TestMacro() {
    using macro::Step;
    auto s = macro::Parse(L"keys:ctrl+shift+p");
    CHECK(s.size() == 1 && s[0].kind == Step::KEYS);
    CHECK(s.size() == 1 && s[0].vk == std::vector<WORD>({VK_CONTROL, VK_SHIFT, 'P'}));
    // модификаторы встают первыми, даже если написаны после
    s = macro::Parse(L"keys:S+win+Shift");
    CHECK(s.size() == 1 && s[0].vk == std::vector<WORD>({VK_LWIN, VK_SHIFT, 'S'}));
    s = macro::Parse(L"keys:ctrl+`");
    CHECK(s.size() == 1 && s[0].vk == std::vector<WORD>({VK_CONTROL, VK_OEM_3}));
    s = macro::Parse(L"keys:ctrl++");  // плюс как клавиша
    CHECK(s.size() == 1 && s[0].vk.size() == 2);
    CHECK(macro::Parse(L"keys:ctrl+щ").empty());  // незнакомая клавиша - ничего, а не половина
    CHECK(macro::Parse(L"keys:").empty());
    CHECK(macro::Parse(L"C:\\Windows").empty());

    s = macro::Parse(L"macro:alt+d; wait 150; text wt -d .; enter");
    CHECK(s.size() == 4);
    CHECK(s.size() == 4 && s[1].kind == Step::WAIT && s[1].ms == 150);
    CHECK(s.size() == 4 && s[2].kind == Step::TEXT && s[2].text == L"wt -d .");
    CHECK(s.size() == 4 && s[3].vk == std::vector<WORD>({VK_RETURN}));
    s = macro::Parse(L"macro:text:Привет, мир;wait:99999");
    CHECK(s.size() == 2 && s[0].text == L"Привет, мир" && s[1].ms == 10000);  // пауза не больше 10 с
    CHECK(macro::Parse(L"macro:ctrl+l; непонятно; enter").empty());
    CHECK(macro::Parse(L"macro: ; ;").empty());

    CHECK(macro::Describe(L"keys:ctrl+shift+p") == L"Ctrl + Shift + P");
    CHECK(macro::Describe(L"macro:alt+d; wait 150; enter") == L"макрос: 3 шага, сначала Alt + D");
    CHECK(macro::TokenVk(L"f12") == VK_F12 && macro::TokenVk(L"f25") == 0);
    CHECK(macro::KeyName(VK_OEM_3) == L"`" && macro::KeyName(VK_RCONTROL) == L"ctrl");
}

void TestStroke() {
    stroke::Tracker t;
    t.Start(1000, 0, 500);
    for (int i = 1; i <= 10; ++i) t.Feed(1000 + i * 15, i * 12.0, 500);  // 120 px за 150 мс вправо
    CHECK(t.Flick(70));
    CHECK(t.Turns() == 0);
    CHECK(std::abs(t.Angle()) < 1);
    // медленный подход к кольцу - не росчерк
    stroke::Tracker slow;
    slow.Start(0, 0, 0);
    for (int i = 1; i <= 30; ++i) slow.Feed(i * 40, i * 4.0, 0);
    CHECK(!slow.Flick(70));
    // далеко, но позже окна - тоже нет
    stroke::Tracker late;
    late.Start(0, 0, 0);
    late.Feed(500, 200, 0);
    CHECK(!late.Flick(70));
    // ломаная: вправо, потом вниз - один поворот; направление первого отрезка - вправо
    stroke::Tracker poly;
    poly.Start(0, 0, 0);
    for (int i = 1; i <= 5; ++i) poly.Feed(i * 10, i * 25.0, 0);
    for (int i = 1; i <= 5; ++i) poly.Feed(50 + i * 10, 125, i * 25.0);
    CHECK(poly.Turns() == 1);
    CHECK(std::abs(poly.Angle()) < 1);
    // дрожь руки поворотами не считается
    stroke::Tracker shake;
    shake.Start(0, 0, 0);
    for (int i = 1; i <= 40; ++i) shake.Feed(i * 5, i * 6.0, (i % 2) * 3.0);
    CHECK(shake.Turns() == 0);
}

void TestFuzzy() {
    CHECK(fuzzy::Score(L"проекты", L"про") == 0);
    CHECK(fuzzy::Score(L"проекты", L"ghj") == 0);  // не та раскладка
    CHECK(fuzzy::Score(L"vs code", L"code") == 1);
    CHECK(fuzzy::Score(L"vs code", L"ода") == -1);
    CHECK(fuzzy::Score(L"проверить сеть", L"пс") == 3);  // по началам слов
    CHECK(fuzzy::Score(L"проверить сеть", L"gc") == 3);  // и в другой раскладке
    CHECK(fuzzy::Score(L"починить умно", L"пчн") == 4);  // буквы по порядку с пропусками
    CHECK(fuzzy::Score(L"починить умно", L"нп") == -1);  // порядок важен
    CHECK(fuzzy::Score(L"загрузки", L"з") == 0);
    CHECK(fuzzy::Score(L"загрузки", L"и") == 2);
    CHECK(fuzzy::Score(L"кольцо", L"") == -1);
    CHECK(fuzzy::OtherLayout(L"ghjtrns") == L"проекты" && fuzzy::OtherLayout(L"сеть") == L"ctnm");
}

}  // namespace

void TestTop20(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestMacro();
    TestStroke();
    TestFuzzy();
}
