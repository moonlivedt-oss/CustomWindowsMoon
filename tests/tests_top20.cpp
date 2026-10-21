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
#include "tools/guard.h"
#include "tools/mft_detail.h"
#include "tools/tools.h"

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
    CHECK(secrets::Kind(L"Bearer eyJhbGciOiJIUzI1NiJ9.abcdefgh") == L"токен");
    CHECK(secrets::Kind(L"Authorization: Bearer abcDEF123456ghiJKL789") == L"токен");
    CHECK(secrets::Kind(L"the bearer of bad news").empty());
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

void TestBigFiles() {
    namespace md = tools::mft_detail;
    std::vector<md::Node> nodes(40);
    auto file = [&](size_t r, ULONGLONG size, bool dir = false, bool reparse = false) {
        nodes[r].used = nodes[r].named = true;
        nodes[r].size = size, nodes[r].dir = dir, nodes[r].reparse = reparse;
    };
    file(3, 1ull << 40);  // служебная запись NTFS ($LogFile) - не в счёт
    file(24, 500);
    file(25, 9000);
    file(26, 7000, true);  // папка
    file(27, 8000);
    file(28, 99999, false, true);  // ссылка
    file(30, 100);
    auto top = md::TopFiles(nodes, 2);
    CHECK(top == std::vector<ULONGLONG>({25, 27}));
    CHECK(md::TopFiles(nodes, 100).size() == 4);
    std::vector<md::BigRow> rows = {{9000, L"C:\\Users\\Moon\\Видео\\фильм.mkv"}, {8000, L"D:\\iso\\win.iso"}};
    ULONGLONG t = 0;
    std::vector<md::BigRow> back;
    CHECK(md::ParseBig(md::FormatBig(1760000000, rows), t, back));
    CHECK(t == 1760000000 && back.size() == 2 && back[0].path == rows[0].path && back[1].bytes == 8000);
    CHECK(!md::ParseBig("#big\t1\n100\tC:\\x\n", t, back));  // недописан - без #end
    // что можно отдать в Корзину
    CHECK(tools::BigFileKeep(L"C:\\Users\\Moon\\Downloads\\game.iso").empty());
    CHECK(tools::BigFileKeep(L"D:\\Видео\\запись.mp4").empty());
    CHECK(!tools::BigFileKeep(L"C:\\pagefile.sys").empty());
    CHECK(!tools::BigFileKeep(L"C:\\Windows\\Installer\\big.msi").empty());
    CHECK(!tools::BigFileKeep(L"C:\\Program Files\\Epic\\data.pak").empty());
    CHECK(!tools::BigFileKeep(L"D:\\SteamLibrary\\steamapps\\common\\Game\\data.pak").empty());
    CHECK(!tools::BigFileKeep(L"C:\\Users\\Moon\\AppData\\Local\\Docker\\wsl\\disk\\docker_data.vhdx").empty());
    CHECK(!tools::BigFileKeep(L"C:\\Users\\Moon\\proj\\.git\\objects\\pack\\pack-1.pack").empty());
    CHECK(!tools::BigFileKeep(L"relative\\path.bin").empty());
}

void TestForecast() {
    constexpr ULONGLONG GB = 1ull << 30;
    std::vector<tools::FreePoint> pts;
    for (int d = 0; d < 10; ++d) pts.push_back({1000 + d, 100 * GB - d * 4 * GB});  // -4 ГБ в сутки, сейчас 64 ГБ
    double per = 0;
    CHECK(tools::DaysLeft(pts, &per) == 16);
    CHECK(per > 3.9 * GB && per < 4.1 * GB);
    // место прибавилось (чистка) - не убывает
    std::vector<tools::FreePoint> up = {{1, 50 * GB}, {2, 49 * GB}, {3, 80 * GB}, {4, 80 * GB}};
    CHECK(tools::DaysLeft(up) == -1);
    // мало замеров или все за два дня
    CHECK(tools::DaysLeft({{1, 10 * GB}, {2, 5 * GB}}) == -1);
    CHECK(tools::DaysLeft({{1, 10 * GB}, {1, 9 * GB}, {2, 5 * GB}}) == -1);
    // убывает медленно (100 МБ в сутки) - не тревожим
    std::vector<tools::FreePoint> slow;
    for (int d = 0; d < 7; ++d) slow.push_back({d, 50 * GB - d * (100ull << 20)});
    CHECK(tools::DaysLeft(slow) == -1);
    // старые замеры (старше 21 дня) не в счёт: тогда убывало, теперь стоит
    std::vector<tools::FreePoint> old;
    for (int d = 0; d < 10; ++d) old.push_back({d, 200 * GB - d * 10 * GB});
    for (int d = 40; d < 50; ++d) old.push_back({d, 60 * GB});
    CHECK(tools::DaysLeft(old) == -1);
}

void TestGiants() {
    std::wstring en =
        L"Shadow Copy Storage association\r\n   For volume: (C:)\\\\?\\Volume{1}\\\r\n"
        L"   Used Shadow Copy Storage space: 2.53 GB (0%)\r\n   Allocated Shadow Copy Storage space: 3 GB (0%)\r\n"
        L"   Maximum Shadow Copy Storage space: 23.7 GB (5%)\r\n";
    ULONGLONG v = tools::ParseShadowUsed(en);
    CHECK(v > 2716000000ull && v < 2717000000ull);  // 2,53 ГиБ
    std::wstring ru =
        L"   Используемое пространство для хранения теневой копии: 512 MB (0%)\r\n"
        L"   Выделенное пространство: 1,5 GB (0%)\r\n";
    CHECK(tools::ParseShadowUsed(ru) == 512ull << 20);
    CHECK(tools::ParseShadowUsed(L"Нет элементов, удовлетворяющих запросу.") == 0);
}

void TestGuard() {
    CHECK(guard::Match(L"CMake.exe", L"ninja.exe|cmake.exe") && guard::Match(L"UE4Editor.exe", L"*Editor.exe"));
    CHECK(!guard::Match(L"cmd.exe", L"cmake.exe|"));
    // баланс: 8 ядер, фоновый процесс ест 4 ядра (50%) - через 90 с понижается, не раньше
    guard::Balancer b;
    std::vector<guard::Balancer::Act> lo, re;
    auto at = [](ULONGLONG ms, ULONGLONG cpu, DWORD pid = 10) {
        guard::Proc p;
        p.pid = pid, p.name = L"hog.exe", p.created = 5, p.cpu = cpu;
        return std::vector<guard::Proc>{p};
    };
    ULONGLONG cpu = 0;
    std::set<DWORD> none;
    for (ULONGLONG t = 0; t <= 80000; t += 10000, cpu += 4 * 10000 * 10000ull)
        b.Feed(t, at(t, cpu), 8, true, none, lo, re);
    CHECK(lo.empty());
    for (ULONGLONG t = 90000; t <= 100000; t += 10000, cpu += 4 * 10000 * 10000ull)
        b.Feed(t, at(t, cpu), 8, true, none, lo, re);
    CHECK(lo.size() == 1 && lo[0].pid == 10 && lo[0].share > 0.45 && b.Lowered().size() == 1);
    // пользователь ушёл - сразу вернуть
    lo.clear();
    b.Feed(110000, at(110000, cpu + 4 * 10000 * 10000ull), 8, false, none, lo, re);
    CHECK(re.size() == 1 && b.Lowered().empty());
    // защищённого (впереди, игра) не трогаем
    guard::Balancer b2;
    std::vector<guard::Balancer::Act> lo2, re2;
    cpu = 0;
    for (ULONGLONG t = 0; t <= 200000; t += 10000, cpu += 8 * 10000 * 10000ull)
        b2.Feed(t, at(t, cpu), 8, true, {10}, lo2, re2);
    CHECK(lo2.empty());
    // «не спать»
    guard::Proc a, c;
    a.name = L"chrome.exe", c.name = L"ninja.exe";
    CHECK(guard::KeepAwake({a, c}, L"cmake.exe|ninja.exe") == L"ninja.exe" &&
          guard::KeepAwake({a}, L"ninja.exe").empty());
    // долгие команды: cmake 5 минут, под ним ninja - сообщается только cmake; короткий git - нет
    constexpr ULONGLONG kMin = 60ull * 10000000;  // минута в 100 нс
    guard::LongWatch w;
    guard::Proc term, cm, nj, git;
    term.pid = 1, term.name = L"WindowsTerminal.exe", term.created = 1;
    cm.pid = 2, cm.parent = 1, cm.name = L"cmake.exe", cm.created = 10 * kMin;
    nj.pid = 3, nj.parent = 2, nj.name = L"ninja.exe", nj.created = 10 * kMin + 5;
    git.pid = 4, git.parent = 1, git.name = L"git.exe", git.created = 11 * kMin;
    const std::wstring masks = L"cmake.exe|ninja.exe|git.exe";
    CHECK(w.Feed(11 * kMin, {term, cm, nj, git}, masks, 3 * 60000).empty() && w.Tracking() == 3);
    auto done = w.Feed(12 * kMin, {term, cm, nj}, masks, 3 * 60000);  // git закончился через минуту - не долгий
    CHECK(done.empty() && w.Tracking() == 2);
    CHECK(w.Feed(14 * kMin, {term, cm, nj}, masks, 3 * 60000).empty());
    done = w.Feed(15 * kMin, {term}, masks, 3 * 60000);  // cmake и ninja закончились; последний раз видели в 14 мин
    CHECK(done.size() == 1 && done[0].name == L"cmake.exe" && done[0].ms == 4 * 60000);
    CHECK(done.size() == 1 && done[0].ancestors == std::vector<DWORD>({1}));
}

void TestCut() {
    CHECK(tools::CutRefuse(L"C:\\Users\\Moon\\AppData\\Local\\Discord\\app-1.0\\Discord.exe").empty());
    CHECK(tools::CutRefuse(L"D:\\Games\\Some Game\\game.exe").empty());
    CHECK(!tools::CutRefuse(L"C:\\Windows\\System32\\svchost.exe").empty());  // часть Windows
    CHECK(!tools::CutRefuse(L"c:\\windows\\explorer.exe").empty());
    CHECK(!tools::CutRefuse(L"C:\\Program Files\\MoonRadial\\RadialMenu-1a2b.exe").empty());
    CHECK(!tools::CutRefuse(L"D:\\Desktop\\components\\RadialMenu\\RadialMenu.exe").empty());
    CHECK(!tools::CutRefuse(L"notepad.exe").empty());                         // не полный путь
    CHECK(!tools::CutRefuse(L"C:\\x\\run.bat").empty());                      // не exe
    CHECK(!tools::CutRefuse(L"C:\\x\\a\" dir=in action=allow.exe").empty());  // кавычка - подстановка в netsh
    // защиту не отрезаем, обходы проверки папки Windows не проходят
    CHECK(!tools::CutRefuse(L"C:\\ProgramData\\Microsoft\\Windows Defender\\Platform\\4.18\\MsMpEng.exe").empty());
    CHECK(!tools::CutRefuse(L"C:\\Program Files (x86)\\Kaspersky Lab\\Kaspersky 21\\avp.exe").empty());
    CHECK(!tools::CutRefuse(L"D:\\x\\avp.exe").empty());
    CHECK(!tools::CutRefuse(L"C:\\Program Files\\..\\Windows\\System32\\svchost.exe").empty());
    CHECK(!tools::CutRefuse(L"C:\\WINDOW~1\\System32\\svchost.exe").empty());
    CHECK(!tools::CutRefuse(L"C:\\x\\a.exe:stream.exe").empty());
    CHECK(tools::CutRuleName(L"C:\\a\\b.exe") == std::wstring(tools::kCutPrefix) + L"C:\\a\\b.exe");
}

}  // namespace

void TestTop20(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestMacro();
    TestStroke();
    TestFuzzy();
    TestSecrets();
    TestZones();
    TestBigFiles();
    TestForecast();
    TestGiants();
    TestGuard();
    TestCut();
}
