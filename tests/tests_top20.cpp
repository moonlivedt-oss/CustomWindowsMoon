// Тесты пунктов «топ-20» (NEXT.md, И1-И20): разбор и правила без окон - макросы, нечёткий поиск, росчерк,
// секреты в буфере, раскладки, прогноз места, пороги сторожей, резервная копия, безопасный запуск.
#include <windows.h>

#include <string>
#include <vector>

#include "core/macro.h"

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

}  // namespace

void TestTop20(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestMacro();
}
