// Тесты серии «упростить жизнь» (NEXT.md, список 2026-10-25: Ф - файлы и телефон, Ж - ноутбук, Е - система)
#include <windows.h>

#include <string>
#include <vector>

#include "life/awake.h"
#include "life/picker.h"
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

}  // namespace

void TestLife(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestPicker();
    TestAwake();
    TestPhoneHttp();
    TestPhoneNames();
    TestPhoneRoute();
}
