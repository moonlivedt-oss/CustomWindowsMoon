// Тесты пунктов безопасности (PLAN.md): задачи с правами, ключи телефонов, тормоз неверных ключей и прочее.
#include <windows.h>

#include <string>

#include "core/elevate.h"
#include "phone/phone.h"
#include "system/tools.h"

namespace {

void (*g_check)(bool, const char*, int) = nullptr;
#define CHECK(x) g_check((x), #x, __LINE__)

void TestElevateHelper() {
    std::wstring dir = L"C:\\Program Files\\MoonRadial", hash(64, L'a');
    hash.replace(0, 12, L"0123456789ab");
    CHECK(elevate::HelperName(hash) == L"RadialMenu-0123456789ab.exe");
    CHECK(elevate::InsideDir(dir + L"\\RadialMenu-x.exe", dir));
    CHECK(elevate::InsideDir(L"c:\\program files\\moonradial\\a.exe", dir));  // регистр не важен
    CHECK(!elevate::InsideDir(dir + L"\\data\\a.exe", dir));                  // подпапка (туда пишет задание)
    CHECK(!elevate::InsideDir(dir + L"\\..\\a.exe", dir));
    CHECK(!elevate::InsideDir(dir + L"Evil\\a.exe", dir));  // соседняя папка с тем же началом
    CHECK(!elevate::InsideDir(dir + L"\\a.exe:x", dir));    // поток NTFS
    CHECK(!elevate::InsideDir(dir + L"\\RADIAL~1.EXE", dir));
    CHECK(!elevate::InsideDir(dir, dir));
    CHECK(!elevate::InsideDir(L"D:\\a.exe", L""));
    CHECK(elevate::TrustedHelper(dir + L"\\RadialMenu-0123456789ab.exe", dir, hash));
    CHECK(elevate::TrustedHelper(L"C:\\PROGRAM FILES\\MoonRadial\\radialmenu-0123456789AB.exe", dir, hash));
    CHECK(!elevate::TrustedHelper(dir + L"\\RadialMenu-0123456789ac.exe", dir, hash));  // содержимое не то
    CHECK(!elevate::TrustedHelper(L"D:\\Desktop\\RadialMenu\\RadialMenu-0123456789ab.exe", dir, hash));  // не там
    CHECK(!elevate::TrustedHelper(dir + L"\\RadialMenu.exe", dir, hash));
    CHECK(!elevate::TrustedHelper(dir + L"\\RadialMenu-0123456789ab.exe", dir, L""));  // хэш не посчитался
    // заметное - только после окна UAC; обычное (умный ремонт, DNS, подсчёт MFT) - задачей без вопросов
    for (const wchar_t* k : {L"deep", L"repair", L"wu-reset", L"defender-offline", L"giants", L"sys-item", L"net-cut"})
        CHECK(elevate::ConsentJob(k));
    for (const wchar_t* k : {L"smart", L"dns-cf", L"mft-sizes", L"net-uncut", L"", L"deep "})
        CHECK(!elevate::ConsentJob(k));
}

void TestPhoneHttpsOnly() {
    phone::DeviceRec guest;
    guest.key = "guestkey12345678";
    guest.added = guest.seen = 1000;
    guest.expires = 4600;
    guest.control = false; guest.screen = true; guest.files = false;
    CHECK(phone::DevicePathAllowed(guest, "/screen", 4599));
    CHECK(phone::DevicePathAllowed(guest, "/mons", 4599));
    CHECK(!phone::DevicePathAllowed(guest, "/in", 4599));
    CHECK(!phone::DevicePathAllowed(guest, "/fs", 4599));
    CHECK(!phone::DevicePathAllowed(guest, "/f/0", 4599));
    CHECK(!phone::DevicePathAllowed(guest, "/up", 4599));
    CHECK(!phone::DevicePathAllowed(guest, "/poll", 4600));
    guest.seen = 4599;
    CHECK(phone::DeviceStale(guest, 4600));  // активность не продлевает гостевой срок
    auto guests = phone::ParseDevices(phone::DevicesText({guest}));
    CHECK(guests.size() == 1 && guests[0].screen && !guests[0].control && !guests[0].files && guests[0].expires == 4600);
    auto legacy = phone::ParseDevices("legacykey12345678\tPhone\t1000\t1000\t1\n");
    CHECK(legacy.size() == 1 && legacy[0].control && legacy[0].screen && legacy[0].files && !legacy[0].expires);
    auto corrupt = phone::ParseDevices("guestkey12345678\tGuest\t1000\t1000\t0\t0\t0\tbroken\n");
    CHECK(corrupt.size() == 1 && phone::DeviceStale(corrupt[0], 1000));
    CHECK(elevate::ConsentJob(L"vpn-guard-setup"));
    CHECK(!elevate::ConsentJob(L"vpn-guard-tick"));
    std::vector<std::string> dev = {"devkey0123456789", "otherkey9876543210"};
    int d = 5;
    CHECK(phone::MatchKey("/tok123/poll", "tok123", dev, &d) == 7 && d == -1);
    CHECK(phone::MatchKey("/tok123", "tok123", dev, &d) == 7);
    CHECK(phone::MatchKey("/tok1234/", "tok123", dev, &d) == 0);  // ключ длиннее - чужой
    CHECK(phone::MatchKey("/d/otherkey9876543210/poll", "tok123", dev, &d) == 21 && d == 1);
    CHECK(phone::MatchKey("/d/otherkey98765432", "tok123", dev, &d) == 0 && d == -1);
    CHECK(phone::MatchKey("/d/", "", {""}, &d) == 0);  // пустой ключ ничего не открывает
    CHECK(phone::MatchKey("/", "", dev, nullptr) == 0);
    CHECK(phone::HttpsLocation("192.168.1.5:8780", 8800, "/d/k/") == "https://192.168.1.5:8800/d/k/");
    CHECK(phone::HttpsLocation("pc.local", 8801, "/d/k/") == "https://pc.local:8801/d/k/");
    CHECK(phone::HttpsLocation("evil.com/x", 8800, "/d/k/").empty());  // Host не дописывает в ответ своё
    CHECK(phone::HttpsLocation("a b", 8800, "/d/k/").empty());
    CHECK(phone::HttpsLocation("[::1]:8780", 8800, "/d/k/").empty());
    CHECK(phone::HttpsLocation("", 8800, "/d/k/").empty());
    CHECK(phone::HttpsLocation("192.168.1.5", 0, "/d/k/").empty());  // https не открыт
    CHECK(phone::HttpsLocation("192.168.1.5", 8800, "d/k/").empty());
    CHECK(phone::HttpsLocation("192.168.1.5", 8800, std::string("/d/k\r\nX: 1")).empty());
}

void TestPhoneDevices() {
    // старый файл: ключ, имя, когда запомнен
    std::vector<phone::DeviceRec> v = phone::ParseDevices(
        "abcdefgh12345678\tPixel 7\t1700000000\r\nbad key\tx\t1\n"
        "abcdefgh12345678\tповтор\t1\nqwertyuiop123456\tSM-A525F\t1700000100\t1700500000\t1\n");
    CHECK(v.size() == 2);
    CHECK(v[0].key == "abcdefgh12345678" && v[0].name == L"Pixel 7" && v[0].added == 1700000000);
    CHECK(v[0].seen == 1700000000 && !v[0].control);  // «заходил» нет - с момента знакомства
    CHECK(v[1].seen == 1700500000 && v[1].control);
    std::string text = phone::DevicesText(v);
    std::vector<phone::DeviceRec> back = phone::ParseDevices(text);
    CHECK(back.size() == 2 && back[1].name == L"SM-A525F" && back[1].added == 1700000100 && back[1].control);
    phone::DeviceRec d;
    d.key = "abcdefgh12345678";
    d.name = L"имя\tс\nпереводом";
    CHECK(phone::ParseDevices(phone::DevicesText({d}))[0].name == L"имя с переводом");  // табуляция не ломает файл
    long long day = 86400;
    d.added = 1000 * day;
    d.seen = 1000 * day;
    CHECK(!phone::DeviceStale(d, 1059 * day));
    CHECK(phone::DeviceStale(d, 1061 * day));
    d.seen = 1050 * day;  // заходил позже - считается от захода
    CHECK(!phone::DeviceStale(d, 1100 * day));
    d.added = d.seen = 0;  // время не записано - не забывать
    CHECK(!phone::DeviceStale(d, 1100 * day));
    CHECK(phone::ParseDevices("").empty());
}

void TestPhoneBadGate() {
    CHECK(phone::SameKey("abc", "abc"));
    CHECK(!phone::SameKey("abd", "abc"));
    CHECK(!phone::SameKey("ab", "abc"));
    CHECK(!phone::SameKey("abcd", "abc"));
    CHECK(!phone::SameKey("", ""));  // пустой ключ не открывает ничего
    CHECK(!phone::SameKey(std::string("abc\0", 4), "abc"));

    // управление - только после «Разрешить» на ПК; буфер, файлы и пульт громкости - без вопроса
    for (const char* c : {"/in", "/type", "/combo", "/tap", "/screen", "/mons", "/wins", "/win", "/ringrun", "/fs",
                          "/open", "/cmd", "/ctlask"})
        CHECK(phone::ControlPath(c));
    for (const char* c : {"/poll", "/clip", "/up", "/", "/in/", "/innn", "/ring", "/pair", "/fsx"})
        CHECK(!phone::ControlPath(c));

    // ключ сессии из QR: первый адрес вписывается сам, другие - только после «Пустить» на ПК
    std::vector<uint32_t> ips;
    CHECK(phone::TokenIpAllowed(ips, 0xC0A80105) && ips.size() == 1);
    CHECK(phone::TokenIpAllowed(ips, 0xC0A80105));
    CHECK(!phone::TokenIpAllowed(ips, 0xC0A80107) && ips.size() == 1);  // подсмотревший ключ - не пущен
    ips.push_back(0xC0A80107);                                          // «Пустить»
    CHECK(phone::TokenIpAllowed(ips, 0xC0A80107));

    // где слушать: только адрес для телефона; общественная сеть - нигде, пока не разрешили именно её
    uint32_t lan = 0xC0A80105;
    CHECK(phone::BindAddress(lan, false, L"", L"") == lan);
    CHECK(phone::BindAddress(lan, false, L"Кафе", L"") == 0);
    CHECK(phone::BindAddress(lan, false, L"Кафе", L"Кафе") == lan);
    CHECK(phone::BindAddress(lan, false, L"Вокзал", L"Кафе") == 0);  // разрешение - только той сети
    CHECK(phone::BindAddress(0, false, L"", L"") == 0);
    CHECK(phone::BindAddress(0, true, L"", L"") == 0x7F000001);

    // отпечаток для сверки глазами - тот же, что у приложения (PureTest.fingerprint)
    CHECK(phone::CaFingerprint("abc") == L"BA78 16BF 8F01 CFEA 4141 40DE");
    CHECK(phone::CaFingerprint("").empty());

    phone::BadGate g;
    uint32_t a = 0xC0A80105, b = 0xC0A80106;
    unsigned long long t = 1000000, ban = 1;
    for (int i = 0; i < phone::BadGate::kFree; ++i) CHECK(g.Bad(a, t + i, &ban) == 0 && ban == 0);  // опечатки
    CHECK(g.Bad(a, t + 10, &ban) == 250);
    CHECK(g.Bad(a, t + 20, &ban) == 500);
    unsigned last = 0;
    for (int i = phone::BadGate::kFree + 3; i < phone::BadGate::kBanAt; ++i) last = g.Bad(a, t + 30 + i, &ban);
    CHECK(last == phone::BadGate::kMaxDelayMs && ban == 0 && !g.Banned(a, t + 100));
    CHECK(g.Bad(a, t + 100, &ban) == 0 && ban == phone::BadGate::kBanMs);  // бан
    CHECK(g.Banned(a, t + 101) && !g.Banned(b, t + 101));                  // другой адрес не задет
    CHECK(!g.Banned(a, t + 100 + phone::BadGate::kBanMs));
    unsigned long long t2 = t + 100 + phone::BadGate::kBanMs;
    for (int i = 0; i < phone::BadGate::kBanAt; ++i) g.Bad(a, t2 + i, &ban);
    CHECK(ban == 2 * phone::BadGate::kBanMs);  // второй бан вдвое дольше
    // долгая тишина - счёт с нуля
    CHECK(g.Bad(b, t, &ban) == 0);
    for (int i = 0; i < phone::BadGate::kFree; ++i) g.Bad(b, t + i, &ban);
    CHECK(g.Bad(b, t + phone::BadGate::kForgetMs + 10, &ban) == 0);
}

void TestOwnExe() {
    std::wstring a(64, L'a'), b(64, L'b');
    CHECK(tools::OwnExeVerdict(a, a, L"") == tools::OWN_OK);
    CHECK(tools::OwnExeVerdict(a, b, L"") == tools::OWN_CHANGED);
    CHECK(tools::OwnExeVerdict(a, b, a) == tools::OWN_CHANGED);  // вшитое важнее отметки: её может переписать любой
    CHECK(tools::OwnExeVerdict(a, std::wstring(64, L'A'), L"") == tools::OWN_OK);  // регистр hex не важен
    CHECK(tools::OwnExeVerdict(a, L"", a) == tools::OWN_OK);                       // без вшитого - по отметке
    CHECK(tools::OwnExeVerdict(a, L"", b) == tools::OWN_CHANGED);
    CHECK(tools::OwnExeVerdict(a, L"", L"") == tools::OWN_OK);  // сверять не с чем
    CHECK(tools::OwnExeVerdict(L"", a, a) == tools::OWN_UNREADABLE);
    CHECK(std::wstring(tools::OwnHash(L"RadialTools.exe")).empty());  // в тестах вшитого нет
}

}  // namespace

void TestSec(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestElevateHelper();
    TestPhoneHttpsOnly();
    TestPhoneDevices();
    TestPhoneBadGate();
    TestOwnExe();
}
