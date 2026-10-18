// Тесты пунктов «топ-20» (NEXT.md, И1-И20): разбор и правила без окон - макросы, нечёткий поиск, росчерк,
// секреты в буфере, раскладки, прогноз места, пороги сторожей, резервная копия, безопасный запуск.
#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

#include "core/fuzzy.h"
#include "core/macro.h"
#include "core/secrets.h"
#include "core/stroke.h"
#include "desk/zones.h"

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

void TestSecrets() {
    CHECK(secrets::Luhn(L"4111111111111111") && !secrets::Luhn(L"4111111111111112"));
    CHECK(secrets::Kind(L"4111 1111 1111 1111") == L"номер карты");
    CHECK(secrets::Kind(L"4111-1111-1111-1112").empty());  // контрольная сумма не сошлась
    CHECK(secrets::Kind(L"+7 912 345 67 89").empty());     // телефон - не карта
    CHECK(secrets::Kind(L"ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zA9cE1gH3jK") == L"токен GitHub");
    CHECK(secrets::Kind(L"  sk-ant-api03-abcdefghijklmnopqrstuvwxyz0123  ") == L"ключ Anthropic");
    CHECK(secrets::Kind(L"AKIAIOSFODNN7EXAMPLE") == L"ключ AWS");
    CHECK(secrets::Kind(L"-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXk\n-----END OPENSSH PRIVATE KEY-----") ==
          L"закрытый ключ");
    CHECK(secrets::Kind(
              L"eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dozjgNryP4J3jVmNHl0w5N_XgL0n3I9PlFUP0THsR8U") ==
          L"токен (JWT)");
    CHECK(secrets::Kind(L"password: Hunter2!x") == L"пароль или ключ в строке");
    CHECK(secrets::Kind(L"пароль = qwerty123") == L"пароль или ключ в строке");
    CHECK(secrets::Kind(L"Xk9fQ2mZ7pLw4Rt8Vb6Nc3Jh5Gd1Sa0E") == L"похоже на ключ");
    // не секреты: код, пути, адреса, хэши, GUID, обычный текст
    CHECK(secrets::Kind(L"const token = getToken();").empty());
    CHECK(secrets::Kind(L"password = input").empty());
    CHECK(secrets::Kind(L"D:\\Desktop\\components\\RadialMenu\\src\\core\\secrets.cpp").empty());
    CHECK(secrets::Kind(L"https://github.com/moonlivedt-oss/TestMenuMoon/pull/12").empty());
    CHECK(secrets::Kind(L"2308fe0c4e1a9b7d3f5e6a8c0b2d4f6e8a0c2e4f").empty());  // sha коммита
    CHECK(secrets::Kind(L"3f2504e0-4f89-11d3-9a0c-0305e82c3301").empty());
    CHECK(secrets::Kind(L"Привет, это просто текст для проверки буфера").empty());
    CHECK(secrets::Kind(L"SomeVeryLongCamelCaseIdentifierName").empty());  // нет цифр
    CHECK(secrets::Entropy(L"aaaa") == 0 && secrets::Entropy(L"abcd") > 1.99);
}

void TestZones() {
    RECT work{0, 0, 1920, 1040}, r{};
    CHECK(zones::Rect(L"z:left", work, 0, r) && r.left == 0 && r.right == 960 && r.bottom == 1040);
    CHECK(zones::Rect(L"z:right", work, 8, r) && r.left == 964 && r.right == 1912 && r.top == 8 && r.bottom == 1032);
    CHECK(zones::Rect(L"z:mid3", work, 0, r) && r.left == 640 && r.right == 1280);
    CHECK(!zones::Rect(L"z:nope", work, 0, r));
    RECT off{-1920, 0, 0, 1080};  // монитор слева от основного
    CHECK(zones::Rect(L"z:br", off, 0, r) && r.left == -960 && r.top == 540 && r.right == 0);
    auto l = zones::Layout(L"l:main2", work, 0);
    CHECK(l.size() == 3 && l[0].right == 1152 && l[1].left == 1152 && l[1].bottom == 520 && l[2].top == 520);
    CHECK(zones::Layout(L"l:4", work, 8).size() == 4 && zones::Layout(L"z:left", work, 0).empty());
    for (const zones::Zone& z : zones::OneWindow()) CHECK(zones::Rect(z.key, work, 8, r) && r.right > r.left);
    // Alt + правая: какие края тянуть
    RECT w{100, 100, 400, 400};
    int ex = -1, ey = -1;
    zones::ResizeEdges(w, {110, 110}, ex, ey);
    CHECK(ex == 0 && ey == 0);
    zones::ResizeEdges(w, {250, 250}, ex, ey);  // середина - правый нижний угол
    CHECK(ex == 2 && ey == 2);
    zones::ResizeEdges(w, {390, 250}, ex, ey);  // правый край, по высоте середина - только ширина
    CHECK(ex == 2 && ey == 1);
    RECT n = zones::Resized(w, 0, 1, 250, 40, 100, 100);  // левый край не заходит за минимум
    CHECK(n.left == 300 && n.top == 100 && n.bottom == 400);
    n = zones::Resized(w, 2, 2, -20, 30, 100, 100);
    CHECK(n.right == 380 && n.bottom == 430 && n.left == 100);
}

}  // namespace

void TestTop20(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestMacro();
    TestStroke();
    TestFuzzy();
    TestSecrets();
    TestZones();
}
