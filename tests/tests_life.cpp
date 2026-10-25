// Тесты серии «упростить жизнь» (NEXT.md, список 2026-10-25: Ф - файлы и телефон, Ж - ноутбук, Е - система)
#include <windows.h>

#include <string>
#include <vector>

#include "life/awake.h"
#include "life/btbattery.h"
#include "life/nightupd.h"
#include "life/picker.h"
#include "life/remind.h"
#include "phone/phone.h"

namespace {

void (*g_check)(bool, const char*, int) = nullptr;
#define CHECK(x) g_check((x), #x, __LINE__)

void TestPhoneHttp() {
    phone::Request r;
    CHECK(
        phone::ParseHead("GET /abc/f/2?x=1&name=%D1%84%D0%BE%D1%82%D0%BE+1.jpg HTTP/1.1\r\nHost: a\r\n"
                         "Content-Length: 42\r\nRange: bytes=10-\r\n",
                         r));
    CHECK(r.method == "GET" && r.path == "/abc/f/2");
    CHECK(r.query["x"] == "1" && r.query["name"] == "\xD1\x84\xD0\xBE\xD1\x82\xD0\xBE 1.jpg");
    CHECK(r.headers["host"] == "a" && r.headers["range"] == "bytes=10-" && r.length == 42);
    CHECK(!phone::ParseHead("hello\r\n", r));
    CHECK(!phone::ParseHead("GET abc HTTP/1.1\r\n", r));                            // путь не с /
    CHECK(!phone::ParseHead("PUT /a HTTP/1.1\r\nContent-Length: 12x\r\n", r));      // мусор в длине
    CHECK(phone::ParseHead("GET /a%2Fb+c HTTP/1.1\r\n", r) && r.path == "/a/b+c");  // + в пути - не пробел
    CHECK(phone::UrlEncode("a b/\xD1\x84") == "a%20b%2F%D1%84");
    CHECK(phone::UrlDecode("%zz%4") == "%zz%4");  // битые %XX - как есть

    long long a = 0, b = 0;
    CHECK(phone::ParseRange("bytes=10-", 100, a, b) && a == 10 && b == 99);
    CHECK(phone::ParseRange("bytes=10-19", 100, a, b) && a == 10 && b == 19);
    CHECK(phone::ParseRange("bytes=90-500", 100, a, b) && a == 90 && b == 99);
    CHECK(phone::ParseRange("bytes=-30", 100, a, b) && a == 70 && b == 99);
    CHECK(phone::ParseRange("bytes=-300", 100, a, b) && a == 0 && b == 99);
    CHECK(!phone::ParseRange("bytes=100-", 100, a, b));  // за концом
    CHECK(!phone::ParseRange("bytes=5-1", 100, a, b));
    CHECK(!phone::ParseRange("bytes=0-1,5-6", 100, a, b));  // несколько кусков - отдаём целиком
    CHECK(!phone::ParseRange("items=0-1", 100, a, b));

    CHECK(phone::Json(L"a\"b\\c\nd") == "\"a\\\"b\\\\c\\nd\"");
    CHECK(phone::Json(std::wstring(1, L'\x01')) == "\"\\u0001\"");
    unsigned char t[3] = {0xFB, 0xFF, 0x00};
    CHECK(phone::Token(t, 3) == "-_8A");
    unsigned char t12[12] = {};
    CHECK(phone::Token(t12, 12).size() == 16);
}

void TestPhoneNames() {
    CHECK(phone::SafeName(L"../../evil.exe") == L"evil.exe");
    CHECK(phone::SafeName(L"C:\\Windows\\a.txt") == L"a.txt");
    CHECK(phone::SafeName(L"a<b>:c\"d|e?f*.jpg") == L"a_b__c_d_e_f_.jpg");
    CHECK(phone::SafeName(L"CON.txt") == L"_CON.txt");
    CHECK(phone::SafeName(L"nul") == L"_nul");
    CHECK(phone::SafeName(L"console.txt") == L"console.txt");
    CHECK(phone::SafeName(L"file. . ") == L"file");
    CHECK(phone::SafeName(L"") == L"файл" && phone::SafeName(L"...") == L"файл");
    std::wstring longName(200, L'x');
    std::wstring s = phone::SafeName(longName + L".jpeg");
    CHECK(s.size() == 120 && s.substr(115) == L".jpeg");
    CHECK(phone::IsMedia(L"IMG_1.HEIC") && phone::IsMedia(L"v.mp4") && !phone::IsMedia(L"a.apk"));
    CHECK(phone::ContentType(L"a.JPG") == "image/jpeg" && phone::ContentType(L"a.bin") == "application/octet-stream");
    // дата снимка из lastModified: 2026-10-25 12:00 UTC - в любом поясе от -11 до +11 это 25-е
    std::wstring d = phone::DatedDir(L"P", 1792929600000ll);
    CHECK(d == L"P\\2026-10-25");
}

void TestPhoneRoute() {
    using phone::Adapter;
    CHECK(phone::PrivateIp(0xC0A80105) && phone::PrivateIp(0x0A000001) && phone::PrivateIp(0xAC100001));
    CHECK(phone::PrivateIp(0xAC1F0001) && !phone::PrivateIp(0xAC200001) && !phone::PrivateIp(0x08080808));
    CHECK(phone::HotspotIp(0xC0A88901) && !phone::HotspotIp(0xC0A80101));
    CHECK(phone::IpText(0xC0A80105) == L"192.168.1.5");
    std::vector<Adapter> list = {
        {L"happ", 0xAC130001, 6, true},       // TUN VPN
        {L"Ethernet", 0xC0A80064, 6, false},  // провод
        {L"Wi-Fi", 0xC0A8010A, 71, false},
        {L"WAN", 0x5DB8D822, 71, false},  // белый адрес - телефон сюда не придёт
    };
    CHECK(phone::PickAddress(list) == 0xC0A8010A);  // Wi-Fi лучше провода
    list.push_back({L"Хот-спот", 0xC0A88901, 71, false});
    CHECK(phone::PickAddress(list) == 0xC0A88901);  // раздача с ноутбука включена - телефон на ней
    CHECK(phone::PickAddress({{L"vpn", 0x0A000002, 53, true}}) == 0);

    using phone::Way;
    std::vector<Way> w = phone::Ways(false, true, 5 << 20);
    CHECK(w.size() == 3 && w[0] == phone::WAY_BLUETOOTH && w[1] == phone::WAY_PHONE_HOTSPOT);
    w = phone::Ways(true, true, 5 << 20);
    CHECK(w.size() == 3 && w[0] == phone::WAY_PHONE_HOTSPOT && w[2] == phone::WAY_BLUETOOTH);
    w = phone::Ways(false, true, 500ll << 20);  // крупное - не по Bluetooth
    CHECK(w.size() == 2 && w[0] == phone::WAY_PHONE_HOTSPOT && w[1] == phone::WAY_PC_HOTSPOT);
    w = phone::Ways(false, false, 1 << 20);
    CHECK(w.size() == 2);
    w = phone::Ways(true, true, 0);  // страница без файлов - Bluetooth не нужен
    CHECK(w.size() == 2);
}

void TestAwake() {
    CHECK(awake::PartialDownload(L"setup.exe.crdownload") && awake::PartialDownload(L"film.mkv.PART"));
    CHECK(awake::PartialDownload(L"a.!ut") && !awake::PartialDownload(L"party.png") &&
          !awake::PartialDownload(L"a.exe"));
    awake::DoneWatch w;
    const ULONGLONG m = 60000;
    w.Start(1000);
    CHECK(!w.Feed(1000 + 9 * m, false));  // ещё ждём, вдруг начнётся
    CHECK(w.Feed(1000 + 10 * m, false));  // 10 минут ничего - разрешить сон
    w.Start(0);
    CHECK(!w.Feed(m, true) && !w.Feed(3 * m, false) && !w.Feed(5 * m, true));
    CHECK(!w.Feed(9 * m, false));  // тихо 4 минуты
    CHECK(w.Feed(10 * m, false) && w.SeenBusy());
    CHECK(awake::Left(30000) == L"ещё меньше минуты");
    CHECK(awake::Left(47 * m) == L"ещё 47 мин");
    CHECK(awake::Left(65 * m) == L"ещё 1 ч 05 мин");
    CHECK(awake::Left(59 * m + 1) == L"ещё 1 ч 00 мин");
}

void TestPicker() {
    CHECK(picker::Hex(200, 50, 63) == L"#C8323F" && picker::Hex(0, 0, 0) == L"#000000");
    CHECK(picker::RgbText(200, 50, 63) == L"rgb(200, 50, 63)");
    CHECK(picker::Measure(312, -48) == L"312 x 48, 315,7 px");
    CHECK(picker::Measure(0, -40) == L"40 px" && picker::Measure(7, 0) == L"7 px");
    POINT a{10, 10};
    POINT h = picker::Constrain(a, {100, 30}), v = picker::Constrain(a, {15, -90});
    CHECK(h.x == 100 && h.y == 10 && v.x == 10 && v.y == -90);
}

void TestRemind() {
    std::vector<remind::Note> l = {{1792929600, L"позвонить\tмаме"}, {1792933200, L"чай"}};
    std::vector<remind::Note> back = remind::Parse(remind::Save(l));
    CHECK(back.size() == 2 && back[0].text == L"позвонить маме" && back[1].when == 1792933200);
    CHECK(remind::Parse(L"мусор\n\tбез времени\n0\tноль\n").empty());
    CHECK(remind::Caption(L"  купить хлеб \r\nи молоко") == L"купить хлеб");
    CHECK(remind::Caption(std::wstring(100, L'a')).size() == 80);
    std::vector<int> h = remind::Hours(14, 10);
    CHECK(h.size() == 2 && h[0] == 15 && h[1] == 18);
    h = remind::Hours(14, 40);  // до 15:00 двадцать минут - сразу 16:00
    CHECK(h[0] == 16 && h[1] == 18);
    h = remind::Hours(17, 40);
    CHECK(h[0] == 19 && h[1] == 21);
    h = remind::Hours(22, 5);
    CHECK(h[0] == 23 && h[1] == 9);
    h = remind::Hours(19, 0);
    CHECK(h[0] == 20 && h[1] == 21);
    h = remind::Hours(23, 50);
    CHECK(h[0] == 1 && h[1] == 9);
}

void TestNightUpdate() {
    CHECK(!nightupd::InWindow(1, 59) && nightupd::InWindow(2, 0) && nightupd::InWindow(5, 29));
    CHECK(!nightupd::InWindow(5, 30) && !nightupd::InWindow(23, 0));
    std::wstring skip = L"Discord.Discord | *Steam* |Мой редактор";
    CHECK(nightupd::Skipped(L"discord.discord", L"Discord", skip));
    CHECK(nightupd::Skipped(L"Valve.Steam", L"Steam", skip));
    CHECK(nightupd::Skipped(L"X.Y", L"мой редактор", skip));
    CHECK(!nightupd::Skipped(L"Mozilla.Firefox", L"Firefox", skip) && !nightupd::Skipped(L"A", L"B", L""));
    CHECK(nightupd::Why(0, L"").empty());
    CHECK(nightupd::Why((int)0x8A150101u, L"") == L"программа была запущена");
    CHECK(nightupd::Why(1, L"The package requires license agreement") == L"нужно ваше согласие с условиями");
    CHECK(nightupd::Why(-1, L"").find(L"15 минут") != std::wstring::npos);
    CHECK(nightupd::Why(5, L"") == L"ошибка 0x00000005");
    std::vector<nightupd::Result> r = {
        {L"A", L"a", 0, L""}, {L"B", L"b", 0, L""}, {L"C", L"c", 1, L"нужна перезагрузка"}};
    CHECK(nightupd::Summary(r) == L"обновлено 2: A, B; не вышло 1: C (нужна перезагрузка)");
    CHECK(nightupd::Summary({}) == L"обновлять было нечего");
    std::vector<nightupd::Result> many(6, {L"P", L"p", 0, L""});
    CHECK(nightupd::Summary(many) == L"обновлено 6: P, P, P, P и ещё 2");
}

void TestBtBattery() {
    CHECK(btbattery::BaseName(L"realme Buds Air7 Pro Hands-Free") == L"realme Buds Air7 Pro");
    CHECK(btbattery::BaseName(L"Транспорт AVRCP realme Buds Air7 Pro") == L"realme Buds Air7 Pro");
    CHECK(btbattery::BaseName(L"S21 пользователя Максим Hands-Free HF") == L"S21 пользователя Максим");
    CHECK(btbattery::BaseName(L"WH-1000XM4 Stereo") == L"WH-1000XM4");
    CHECK(btbattery::BaseName(L"Stereo") == L"Stereo");  // имя из одного хвоста не съедается
    btbattery::LowWatch w;
    CHECK(!w.Feed(L"A", 40) && w.Feed(L"A", 15) && !w.Feed(L"A", 10));  // один раз
    CHECK(!w.Feed(L"A", 20) && !w.Feed(L"A", 14));                      // подзарядили чуть-чуть - молчим
    CHECK(!w.Feed(L"A", 80) && w.Feed(L"A", 12));                       // зарядили - снова скажем
    CHECK(w.Feed(L"B", 5));
    CHECK(btbattery::Line({{L"A", 64}, {L"B", 20}}) == L"A: 64%, B: 20%");
}

}  // namespace

void TestLife(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestBtBattery();
    TestNightUpdate();
    TestRemind();
    TestPicker();
    TestAwake();
    TestPhoneHttp();
    TestPhoneNames();
    TestPhoneRoute();
}
