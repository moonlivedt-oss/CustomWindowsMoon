// Тесты серии «упростить жизнь» (NEXT.md, список 2026-10-25: Ф - файлы и телефон, Ж - ноутбук, Е - система)
#include <windows.h>

#include <string>
#include <vector>

#include "life/appbright.h"
#include "life/awake.h"
#include "life/batcare.h"
#include "life/btbattery.h"
#include "life/capwatch.h"
#include "life/convert.h"
#include "life/gamehot.h"
#include "life/nightupd.h"
#include "life/picker.h"
#include "life/preview.h"
#include "life/record.h"
#include "life/remind.h"
#include "life/shelf.h"
#include "life/tidy.h"
#include "life/vpnexit.h"
#include "phone/cert.h"
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

void TestCapWatch() {
    using capwatch::Use;
    Use discord{L"микрофон", L"Discord.exe", L"C:#Users#me#Discord.exe", L"C:\\Users\\me\\Discord.exe", true, false};
    Use cam{L"камера", L"WindowsCamera", L"Microsoft.WindowsCamera_8wekyb3d8bbwe", L"", true, true};
    Use old{L"микрофон", L"Zoom.exe", L"k", L"C:\\Zoom.exe", false, false};
    CHECK(capwatch::SeenKey(discord) == L"микрофон|c:\\users\\me\\discord.exe");
    CHECK(capwatch::SeenKey(cam) == L"камера|microsoft.windowscamera_8wekyb3d8bbwe");
    std::set<std::wstring> seen = {capwatch::SeenKey(cam)};
    std::vector<Use> f = capwatch::Fresh({discord, cam, old, discord}, seen);
    CHECK(f.size() == 1 && f[0].app == L"Discord.exe");  // знакомая камера и выключенный Zoom - молчим, дубль - один
    seen.insert(capwatch::SeenKey(discord));
    CHECK(capwatch::Fresh({discord, cam}, seen).empty());
}

void TestAppBright() {
    auto rules = appbright::ParseRules(L"Figma.exe=+20 | vlc.exe=90| sumatrapdf.exe=-15|bad|x.exe=-5x|y.exe=-200");
    CHECK(rules.size() == 5 && rules[L"figma.exe"].relative && rules[L"figma.exe"].value == 20);
    CHECK(!rules[L"vlc.exe"].relative && rules[L"vlc.exe"].value == 90 && rules[L"y.exe"].value == -100);
    CHECK(appbright::SaveRules(appbright::ParseRules(L"a.exe=+5|b.exe=40|c.exe=-3")) == L"a.exe=+5|b.exe=40|c.exe=-3");
    CHECK(appbright::Target(90, {20, true}) == 100 && appbright::Target(10, {-15, true}) == 0);
    CHECK(appbright::Target(10, {60, false}) == 60);

    appbright::Planner p;
    p.rules = appbright::ParseRules(L"figma.exe=+20|sumatrapdf.exe=-15");
    CHECK(p.OnApp(L"explorer.exe", 40) == -1);    // обычная программа - не трогаем
    CHECK(p.OnApp(L"Figma.exe", 40) == 60);       // ярче от 40
    CHECK(p.OnApp(L"SumatraPDF.exe", 60) == 25);  // из правила в правило - от исходных 40
    CHECK(p.OnApp(L"code.exe", 25) == 40);        // ушли - как было
    CHECK(p.OnApp(L"code.exe", 40) == -1 && p.Active().empty());
    CHECK(p.OnApp(L"figma.exe", 0) == 20);  // вечером яркость 0 - Figma всё равно ярче
    CHECK(!p.Learn(20) && p.Learn(35));     // поправили руками до 35 - запомнили +35
    CHECK(p.rules[L"figma.exe"].value == 35);
    CHECK(p.OnApp(L"code.exe", 35) == 0);
    CHECK(!p.Learn(50));  // ни в какой программе с правилом - учить нечего
}

void TestBatCare() {
    auto days = batcare::Parse(L"2026-10-20\t7.5\t0\n2026-10-21\t2\t1.25\nмусор\n2026-10-22\t9\n");
    CHECK(days.size() == 3 && days[L"2026-10-21"].limitHours == 1.25 && days[L"2026-10-22"].fullHours == 9);
    CHECK(!batcare::ShouldOffer(days));  // два тяжёлых дня из трёх
    days[L"2026-10-23"] = {6, 0};
    CHECK(batcare::ShouldOffer(days));
    days[L"2026-10-24"] = {1, 0};
    days[L"2026-10-25"] = {1, 0};
    days[L"2026-10-26"] = {0, 0};  // последние пять: 9, 6, 1, 1, 0 - только два тяжёлых
    CHECK(!batcare::ShouldOffer(days));
    CHECK(batcare::SavedHours(days) == 1.25);
    auto back = batcare::Parse(batcare::Save(days, 3));
    CHECK(back.size() == 3 && back.begin()->first == L"2026-10-24");
}

void TestGameHot() {
    const uint64_t s = 10000, m = 60000;
    gamehot::HotWatch w;
    CHECK(!w.Feed(1 * s, 70, false));
    CHECK(!w.Feed(2 * s, 86, false));          // горячо с 20 с
    CHECK(!w.Feed(2 * s + 4 * m, 82, false));  // 80-84 - отсчёт идёт
    CHECK(w.Feed(2 * s + 5 * m, 87, false));   // 5 минут - сказать
    CHECK(!w.Feed(2 * s + 6 * m, 90, true));   // один раз за игру
    w.Reset();
    CHECK(!w.Feed(1 * s, 86, false) && !w.Feed(3 * m, 78, false));                           // остыло - заново
    CHECK(!w.Feed(4 * m, 70, true) && !w.Feed(8 * m, 70, true) && w.Feed(9 * m, 75, true));  // упор в перегрев
}

void TestVpnExit() {
    std::string cfg = R"({"route":{"rules":[{"outbound":"direct","process_name":["xray.exe","Steam.exe"]},
        {"action":"sniff"},{"outbound":"proxy","process_name":["chrome.exe"]}],"final":"proxy"}})";
    std::vector<std::wstring> by = vpnexit::ParseBypass(cfg);
    CHECK(by.size() == 2 && by[0] == L"xray" && by[1] == L"steam");
    CHECK(vpnexit::ParseBypass("{}").empty());
    const long long now = 1792929600, day = 86400;
    std::vector<vpnexit::Sample> l = {{L"CS2", L"DE", 60, now - day},      {L"CS2", L"DE", 64, now - 2 * day},
                                      {L"CS2", L"DE", 300, now - 3 * day}, {L"CS2", L"FI", 110, now - day},
                                      {L"CS2", L"US", 50, now - 40 * day}, {L"Dota", L"SE", 20, now}};
    int ms = 0;
    CHECK(vpnexit::Better(l, L"CS2", L"FI", 115, now, &ms) == L"DE" && ms == 64);  // медиана 60/64/300
    CHECK(vpnexit::Better(l, L"CS2", L"FI", 75, now).empty());            // 64 против 75 - не стоит переключать
    CHECK(vpnexit::Better(l, L"CS2", L"DE", 200, now).empty() == false);  // FI 110 лучше 200
    CHECK(vpnexit::Better(l, L"Minecraft", L"FI", 300, now).empty());
    auto back = vpnexit::Parse(vpnexit::Save(l, 3));
    CHECK(back.size() == 3 && back[0].country == L"FI" && back[2].game == L"Dota");
}

void TestConvert() {
    CHECK(convert::KindOf(L"a.HEIC") == convert::IMAGE && convert::KindOf(L"b.mov") == convert::VIDEO);
    CHECK(convert::KindOf(L"c.Pdf") == convert::PDF && convert::KindOf(L"d.exe") == convert::OTHER);
    CHECK(convert::OutName(L"D:\\a\\clip.mov", L".mp4", L"8 МБ") == L"D:\\a\\clip (8 МБ).mp4");
    CHECK(convert::OutName(L"D:\\a.b\\clip", L".png", L"") == L"D:\\a.b\\clip.png");
    int k = convert::VideoKbps(8ll << 20, 40);  // 8 МБ на 40 с
    CHECK(k > 1400 && k < 1560);
    CHECK(convert::VideoKbps(8ll << 20, 3600) == 64 && convert::VideoKbps(1, 0) == 0);
    std::wstring a = convert::FfmpegArgs(L"fit", L"in.mov", L"out.mp4", 1000);
    CHECK(a.find(L"-b:v 1000k") != std::wstring::npos && a.find(L"\"out.mp4\"") != std::wstring::npos);
    CHECK(convert::FfmpegArgs(L"gif", L"i", L"o.gif", 0).find(L"palettegen") != std::wstring::npos);
}

void TestRecord() {
    RECT r = record::Normalize({500, 400}, {101, 99});
    CHECK(r.left == 101 && r.top == 99 && r.right - r.left == 398 && r.bottom - r.top == 300);  // чётные
    r = record::Normalize({10, 10}, {12, 13});
    CHECK(r.right - r.left == 16 && r.bottom - r.top == 16);
    std::wstring a = record::GrabArgs({100, 100, 600, 400}, L"o.mp4", 600);
    CHECK(a.find(L"-offset_x 100 -offset_y 100 -video_size 500x300") != std::wstring::npos);
    CHECK(a.find(L"-t 600") != std::wstring::npos && a.find(L"\"o.mp4\"") != std::wstring::npos);
    CHECK(record::Clock(7) == L"0:07" && record::Clock(83) == L"1:23");
}

void TestTidy() {
    CHECK(tidy::Candidate(L"shot.png", FILE_ATTRIBUTE_ARCHIVE, 20));
    CHECK(!tidy::Candidate(L"shot.png", FILE_ATTRIBUTE_ARCHIVE, 13));
    CHECK(!tidy::Candidate(L"Discord.lnk", 0, 400) && !tidy::Candidate(L"desktop.ini", 0, 400));
    CHECK(!tidy::Candidate(L"Архив", FILE_ATTRIBUTE_DIRECTORY, 400));
    CHECK(!tidy::Candidate(L"x.txt", FILE_ATTRIBUTE_HIDDEN, 400));
    SYSTEMTIME st{2026, 3, 0, 15, 12, 0, 0, 0};
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);  // середина месяца - в любом поясе тот же месяц
    CHECK(tidy::MonthDir(L"C:\\D", ft) == L"C:\\D\\Архив\\2026-03");
}

void TestPreview() {
    CHECK(preview::KindOf(L"a.WEBP") == preview::IMAGE && preview::KindOf(L"b.cpp") == preview::TEXT);
    CHECK(preview::KindOf(L"c.mp4") == preview::OTHER && preview::KindOf(L"d.pdf") == preview::OTHER);
    RECT r = preview::Fit(1920, 1080, {0, 0, 960, 960});
    CHECK(r.left == 0 && r.right == 960 && r.bottom - r.top == 540 && r.top == 210);
    r = preview::Fit(100, 50, {0, 0, 1000, 1000});  // мелкое - не больше чем вдвое
    CHECK(r.right - r.left == 200 && r.bottom - r.top == 100);
    CHECK(preview::Caption(L"a.png", 96218, 1280, 720, 3, 4) == L"a.png   93 КБ   1280 x 720   4 из 4");
    CHECK(preview::Caption(L"b.txt", 5 << 20, 0, 0, 0, 1) == L"b.txt   5,0 МБ");
}

void TestShelf() {
    std::vector<std::wstring> s = {L"C:\\a.txt"};
    CHECK(shelf::Merge(s, {L"c:\\A.TXT", L"D:\\b.png", L"", L"D:\\b.png"}) == 1 && s.size() == 2);
    CHECK(shelf::Merge(s, {L"x", L"y", L"z"}, 3) == 1 && s.size() == 3);  // не больше max
    CHECK(shelf::Title(s) == L"Полка: 3 файла" && shelf::Title({}) == L"Полка пуста");
}

void TestPhoneMore() {
    CHECK(phone::Crc32(0, "123456789", 9) == 0xCBF43926u);  // эталон CRC-32
    uint32_t c = phone::Crc32(0, "1234", 4);
    CHECK(phone::Crc32(c, "56789", 5) == 0xCBF43926u);  // по кускам - то же
    std::string l = phone::ZipLocal("a/b.txt", 5, 0, 0);
    CHECK(l.size() == 30 + 7 && l.compare(0, 4, "PK\x03\x04") == 0 && (unsigned char)l[7] == 0x08);  // бит 11 - UTF-8
    CHECK(phone::ZipDescriptor(1, 5).size() == 16 && phone::ZipEnd(2, 100, 200).size() == 22);
    phone::ZipEntry e;
    e.name = "x";
    CHECK(phone::ZipCentral(e).size() == 47);
    CHECK(phone::ZipSize({{"ab", 10}, {"c", 0}}) == 22 + (30 + 2 + 10 + 16 + 46 + 2) + (30 + 1 + 0 + 16 + 46 + 1));
    CHECK(phone::ZipFits({{"a", 100}}) && !phone::ZipFits({{"a", 5000000000ll}}));
    CHECK(phone::ZipName(L"\\Отпуск\\день 1\\a.jpg") ==
          "\xD0\x9E\xD1\x82\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA/"
          "\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C 1/a.jpg");
    CHECK(phone::SafeRel("Films/2026/a.mkv") == L"Films\\2026\\a.mkv" && phone::SafeRel("") == L"");
    CHECK(phone::SafeRel("a//b/") == L"a\\b");
    CHECK(phone::SafeRel("..") == L"?" && phone::SafeRel("a/../b") == L"?" && phone::SafeRel("C:/Windows") == L"?");
    CHECK(phone::SafeRel("a\\b") == L"?" && phone::SafeRel("a/./b") == L"?");
    CHECK(phone::UploadId("abcdef0123456789") && !phone::UploadId("short") && !phone::UploadId("abc/def0123456"));
    CHECK(phone::RateText(L"Отдаю", 500, 0).empty());
    CHECK(phone::RateText(L"Отдаю", 2.5 * 1048576, 5 * 1048576) == L"Отдаю 2,5 МБ/с, ещё 2 с");
    CHECK(phone::RateText(L"Принимаю", 100 * 1024, 100 * 1024 * 75) == L"Принимаю 100 КБ/с, ещё 1 мин 15 с");
}

// «Поделиться - на ПК»: multipart кусками любого размера (граница режется между кусками), имена, ссылки
void TestPhoneShare() {
    CHECK(phone::Boundary("multipart/form-data; boundary=----abc") == "----abc");
    CHECK(phone::Boundary("Multipart/Form-Data; charset=utf-8; boundary=\"a b;c\"") == "a b;c");
    CHECK(phone::Boundary("text/plain; boundary=x").empty() && phone::Boundary("multipart/form-data").empty());
    CHECK(phone::Boundary("multipart/form-data; boundary=" + std::string(71, 'x')).empty());
    std::string bin = std::string("\x00\r\n--b\r\n-", 9) + "\r\n--XY" + "tail\r\n-";  // похоже на границу, но не она
    std::string body =
        "preamble\r\n--XYZ\r\nContent-Disposition: form-data; name=\"text\"\r\n\r\nпривет https://ex.com/a?b=1.\r\n"
        "--XYZ\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a%22b.jpg\"\r\n"
        "Content-Type: Image/JPEG\r\n\r\n" +
        bin +
        "\r\n"
        "--XYZ\r\nContent-Disposition: form-data; name=\"f\"; filename=\"x.txt\"; "
        "filename*=UTF-8''%D1%84.txt\r\n\r\n\r\n"
        "--XYZ--\r\nepilogue";
    for (size_t step : {(size_t)1, (size_t)2, (size_t)3, (size_t)7, (size_t)16, body.size()}) {
        phone::Multipart mp("XYZ");
        std::vector<phone::FormPart> parts;
        std::vector<std::string> data;
        int ends = 0;
        mp.onBegin = [&](const phone::FormPart& p) {
            parts.push_back(p);
            data.emplace_back();
            return true;
        };
        mp.onData = [&](const char* p, size_t n) {
            data.back().append(p, n);
            return true;
        };
        mp.onEnd = [&] { return ++ends, true; };
        bool ok = true;
        for (size_t i = 0; i < body.size(); i += step)
            ok = ok && mp.Feed(body.data() + i, std::min(step, body.size() - i));
        CHECK(ok && mp.Done() && ends == 3 && parts.size() == 3);
        if (parts.size() != 3) continue;
        CHECK(parts[0].name == "text" && !parts[0].file && data[0] == "привет https://ex.com/a?b=1.");
        CHECK(parts[1].file && parts[1].filename == "a\"b.jpg" && parts[1].type == "image/jpeg" && data[1] == bin);
        CHECK(parts[2].filename == "\xD1\x84.txt" && data[2].empty());  // filename* важнее filename
    }
    {
        phone::Multipart mp("XYZ");  // за границей не \r\n и не -- - испорчено
        CHECK(!mp.Feed("--XYZ\r\n\r\nabc\r\n--XYZxx", 21));
        phone::Multipart stop("XYZ");  // обработчик прервал (мало места)
        stop.onBegin = [](const phone::FormPart&) { return false; };
        CHECK(!stop.Feed("--XYZ\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n", 51));
        phone::Multipart cut("XYZ");  // тело кончилось без закрывающей границы
        CHECK(cut.Feed("--XYZ\r\n\r\nabc", 12) && !cut.Done());
    }
    CHECK(phone::ShareLink("https://ex.com/x", "Смотри") == "https://ex.com/x");
    CHECK(phone::ShareLink("", "Статья (https://ex.com/a_(b)), читай") == "https://ex.com/a_(b");
    CHECK(phone::ShareLink("", "Смотри: HTTP://Ex.com/p.") == "HTTP://Ex.com/p");
    CHECK(phone::ShareLink("ftp://x", "просто текст") == "" && phone::ShareLink("", "xhttps://ex.com") == "");
    CHECK(phone::ShareLink("", "https://") == "" && phone::ShareLink(" https://a.b ", "") == "https://a.b");
}

// свой корневой сертификат и сертификат сервера: цепочка сходится до корня (только до него), адреса в SAN
void TestPhoneCert() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"radial_cert_test";
    DeleteFileW((dir + L"\\phone-ca.cer").c_str());
    DeleteFileW((dir + L"\\phone-ca.key").c_str());
    std::string ca, again;
    CHECK(phone::cert::LoadOrCreateCa(dir, ca) && ca.size() > 200);
    CHECK(phone::cert::LoadOrCreateCa(dir, again) && again == ca);  // второй раз - тот же, не новый
    const wchar_t* key = L"MoonDesk Phone TLS test";
    PCCERT_CONTEXT srv = phone::cert::IssueServer(dir, {0xC0A80105, 0x7F000001}, key);
    CHECK(srv != nullptr);
    if (srv) {
        PCCERT_CONTEXT root = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)ca.data(), (DWORD)ca.size());
        HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
        CertAddCertificateContextToStore(roots, root, CERT_STORE_ADD_ALWAYS, nullptr);
        CERT_CHAIN_ENGINE_CONFIG ec{};
        ec.cbSize = sizeof(ec);
        ec.hExclusiveRoot = roots;  // доверять только своему корню, системные не при чём
        HCERTCHAINENGINE eng = nullptr;
        CHECK(CertCreateCertificateChainEngine(&ec, &eng));
        CERT_CHAIN_PARA cp{};
        cp.cbSize = sizeof(cp);
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        CHECK(CertGetCertificateChain(eng, srv, nullptr, nullptr, &cp, CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY, nullptr,
                                      &chain));
        if (chain) {
            DWORD err = chain->TrustStatus.dwErrorStatus & ~(DWORD)CERT_TRUST_REVOCATION_STATUS_UNKNOWN &
                        ~(DWORD)CERT_TRUST_IS_OFFLINE_REVOCATION;
            CHECK(err == 0 && chain->rgpChain[0]->cElement == 2);
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
            ssl.cbSize = sizeof(ssl);
            ssl.dwAuthType = AUTHTYPE_SERVER;
            ssl.fdwChecks = 0x80;  // SECURITY_FLAG_IGNORE_REVOCATION (wininet.h): отзыв у своего корня не проверить
            ssl.pwszServerName = (wchar_t*)L"192.168.1.5";
            CERT_CHAIN_POLICY_PARA pp{};
            pp.cbSize = sizeof(pp);
            pp.dwFlags = CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS;
            pp.pvExtraPolicyPara = &ssl;
            CERT_CHAIN_POLICY_STATUS ps{};
            ps.cbSize = sizeof(ps);
            CHECK(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps) && ps.dwError == 0);
            ssl.pwszServerName = (wchar_t*)L"192.168.1.6";  // чужой адрес - не подходит
            CHECK(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps) && ps.dwError != 0);
            CertFreeCertificateChain(chain);
        }
        DWORD n = 0;  // ключ сервера привязан (Schannel найдёт его по имени)
        CHECK(CertGetCertificateContextProperty(srv, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &n) && n > 0);
        CertFreeCertificateChainEngine(eng);
        CertCloseStore(roots, 0);
        CertFreeCertificateContext(root);
        CertFreeCertificateContext(srv);
    }
    // корневой ограничен своей сетью (NameConstraints): сертификат на публичный адрес Windows не примет
    PCCERT_CONTEXT pub = phone::cert::IssueServer(dir, {0x08080808}, key);
    CHECK(pub != nullptr);
    if (pub) {
        PCCERT_CONTEXT root = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)ca.data(), (DWORD)ca.size());
        HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
        CertAddCertificateContextToStore(roots, root, CERT_STORE_ADD_ALWAYS, nullptr);
        CERT_CHAIN_ENGINE_CONFIG ec{};
        ec.cbSize = sizeof(ec);
        ec.hExclusiveRoot = roots;
        HCERTCHAINENGINE eng = nullptr;
        CHECK(CertCreateCertificateChainEngine(&ec, &eng));
        CERT_CHAIN_PARA cp{};
        cp.cbSize = sizeof(cp);
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        CHECK(CertGetCertificateChain(eng, pub, nullptr, nullptr, &cp, CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY, nullptr,
                                      &chain));
        if (chain)
            CHECK((chain->TrustStatus.dwErrorStatus &
                   (CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT | CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT |
                    CERT_TRUST_INVALID_NAME_CONSTRAINTS | CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT)) != 0);
        if (chain) CertFreeCertificateChain(chain);
        CertFreeCertificateChainEngine(eng);
        CertCloseStore(roots, 0);
        CertFreeCertificateContext(root);
        CertFreeCertificateContext(pub);
    }
    phone::cert::DeleteKey(key);
    DeleteFileW((dir + L"\\phone-ca.cer").c_str());
    DeleteFileW((dir + L"\\phone-ca.key").c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestPhoneRemote() {
    RECT m{1920, 0, 3840, 1080};  // второй монитор справа
    POINT p = phone::TapPoint(0.5, 0.5, m);
    CHECK(p.x == 2880 && p.y == 540);
    p = phone::TapPoint(1.0, 1.0, m);
    CHECK(p.x == 3839 && p.y == 1079);  // край - последний пиксель, а не за монитором
    p = phone::TapPoint(-3, 7, m);
    CHECK(p.x == 1920 && p.y == 1079);
    SIZE s = phone::FrameSize(1920, 1200, 800);
    CHECK(s.cx == 800 && s.cy == 500);
    s = phone::FrameSize(1280, 720, 4000);  // шире 2560 не просим, а мельче экрана - как есть
    CHECK(s.cx == 1280 && s.cy == 720);
    s = phone::FrameSize(1920, 1080, 10);
    CHECK(s.cx == 320 && s.cy == 180);
    CHECK(phone::Combo("copy") == std::vector<WORD>({VK_CONTROL, 'C'}) && phone::Combo("rm -rf").empty());
    CHECK(phone::FsOp("move") && phone::FsOp("delete") && !phone::FsOp("format") && !phone::FsOp(""));
    CHECK(phone::OpenAllowed(L"C:\\Users\\u\\Downloads\\отчёт.pdf") && phone::OpenAllowed(L"C:\\Music\\a.MP3"));
    // белый список: без расширения (папки проверяет сервер) и всё неизвестное - нет
    CHECK(!phone::OpenAllowed(L"C:\\Music\\Альбом") && !phone::OpenAllowed(L"C:\\Downloads\\x.py"));
    CHECK(!phone::OpenAllowed(L"C:\\Downloads\\disk.iso") && !phone::OpenAllowed(L"C:\\Downloads\\a.docm"));
    CHECK(!phone::OpenAllowed(L"C:\\Downloads\\a.rdp") && !phone::OpenAllowed(L"C:\\Downloads\\page.html"));
    CHECK(!phone::OpenAllowed(L"C:\\Downloads\\setup.EXE") && !phone::OpenAllowed(L"C:\\Downloads\\run.bat"));
    CHECK(!phone::OpenAllowed(L"C:\\Downloads\\a.exe. .") && !phone::OpenAllowed(L"C:\\Downloads\\x.lnk"));
    CHECK(!phone::OpenAllowed(L"C:\\Downloads\\doc.txt:evil.exe") && !phone::OpenAllowed(L"D:\\a\\b.ps1"));
    CHECK(phone::KeyVk("enter") == VK_RETURN && phone::KeyVk("pgdn") == VK_NEXT && phone::KeyVk("a") == 'A');
    CHECK(phone::KeyVk("7") == '7' && phone::KeyVk("f5") == VK_F5 && phone::KeyVk("f12") == VK_F12);
    CHECK(phone::KeyVk("f13") == 0 && phone::KeyVk("f05") == 0 && phone::KeyVk("A") == 0 && phone::KeyVk("") == 0);
}

void TestPhoneApp() {
    std::map<std::string, std::string> f = phone::ParseForm("a=1&b=%D0%BA%D0%BE%D0%B4+2&a=3&&c&=x");
    CHECK(f.size() == 3 && f["a"] == "1" && f["b"] == "код 2" && f["c"].empty());
    phone::Notice n;
    CHECK(phone::ParseNotice("kind=note&app=Telegram&title=%D0%9C%D0%B0%D0%BC%D0%B0&text=a%0Ab%09%09c&id=tg:42", n));
    CHECK(n.kind == "note" && n.app == L"Telegram" && n.title == L"Мама" && n.text == L"a b c" &&
          n.key == "note|tg:42");
    CHECK(phone::ParseNotice("kind=sms&num=%2B79001234567&text=hi", n) && n.number == L"+79001234567");
    CHECK(n.key == "sms|||hi|+79001234567");                              // без id - из полей
    CHECK(!phone::ParseNotice("kind=note&app=X", n));                     // ни заголовка, ни текста
    CHECK(!phone::ParseNotice("kind=rm&title=x", n));                     // неизвестный вид
    CHECK(phone::ParseNotice("kind=end&id=c1", n) && n.key == "end|c1");  // конец звонка - без текста
    CHECK(phone::ParseNotice("kind=call&num=1&id=%3Cscript%3E", n) && n.key == "call|script");
    CHECK(phone::CleanLine(std::wstring(L"  a") + wchar_t(0x202E) + L"b" + wchar_t(0x2028) + L" c  ", 0) == L"ab c");
    CHECK(phone::CleanLine(L"abcdef", 4) == L"abc…" && phone::CleanLine(L"ab", 4) == L"ab");
    CHECK(phone::CleanLine(std::wstring(L"ab") + wchar_t(0xD83D) + wchar_t(0xDE00) + L"z", 4) == L"ab…");
    CHECK(phone::SmsCode(L"Ваш код: 4821. Никому не сообщайте") == L"4821");
    CHECK(phone::SmsCode(L"Code 123-456 for login") == L"123456");
    CHECK(phone::SmsCode(L"G-552913 is your Google verification code") == L"552913");
    CHECK(phone::SmsCode(L"Код 7731 действует 5 минут") == L"7731");
    CHECK(phone::SmsCode(L"Списано 1500 р. Баланс 23000 руб") == L"");
    CHECK(phone::SmsCode(L"Перезвоните +79001234567") == L"");
    CHECK(phone::SmsCode(L"Заказ 12345 доставлен 2026") == L"");  // два числа, ни слова «код» - не угадываем
    CHECK(phone::SmsCode(L"Заказ 12345, код получения 9911") == L"9911");
    CHECK(phone::SmsCode(L"Встреча в 12:30, сумма 1500.00") == L"");
    CHECK(phone::SmsCode(L"ID12345") == L"" && phone::SmsCode(L"") == L"");
    unsigned char mac[6] = {0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0xff}, nul[6] = {};
    CHECK(phone::MacText(mac, 6) == "0A:1B:2C:3D:4E:FF" && phone::MacText(nul, 6).empty() &&
          phone::MacText(mac, 5).empty());
    CHECK(phone::BroadcastOf(0xC0A80105, 24) == 0xC0A801FF && phone::BroadcastOf(0x0A000001, 8) == 0x0AFFFFFF);
    CHECK(phone::BroadcastOf(0xC0A80105, 32) == 0xC0A80105 && phone::BroadcastOf(1, 0) == 0xFFFFFFFF);
    phone::NoticeGate g;
    CHECK(g.Pass("a", 1000) && !g.Pass("a", 3000) && g.Pass("b", 3000) && g.Pass("a", 7000));
    int passed = 0;
    for (int i = 0; i < 30; ++i) passed += g.Pass("k" + std::to_string(i), 8000);
    CHECK(passed == 17);         // 20 за минуту: три места уже заняты
    CHECK(g.Pass("k0", 70000));  // через минуту - снова можно
}

}  // namespace

void TestLife(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestPhoneApp();
    TestPhoneShare();
    TestPhoneCert();
    TestPhoneRemote();
    TestPhoneMore();
    TestShelf();
    TestPreview();
    TestTidy();
    TestRecord();
    TestConvert();
    TestVpnExit();
    TestGameHot();
    TestBatCare();
    TestAppBright();
    TestCapWatch();
    TestBtBattery();
    TestNightUpdate();
    TestRemind();
    TestPicker();
    TestAwake();
    TestPhoneHttp();
    TestPhoneNames();
    TestPhoneRoute();
}
