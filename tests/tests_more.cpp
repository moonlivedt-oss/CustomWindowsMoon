// Тесты ревью 2026-10-21: radial.cfg туда и обратно (экранирование «|», служебные имена групп) и новые правила
// без окон.
#include <windows.h>

#include <string>
#include <vector>

#include "core/config.h"
#include "core/edgecheck.h"
#include "core/procsnap.h"
#include "core/themes.h"
#include "net/speed.h"
#include "tools/tools.h"
#include "tools/weekdiff.h"
#include "gfx/qr.h"
#include "net/wifi.h"
#include "desk/dropundo.h"
#include "app/rules.h"
#include "app/settingsio.h"
#include "tools/healthscore.h"
#include "tools/important.h"

namespace {

void (*g_check)(bool, const char*, int) = nullptr;
#define CHECK(x) g_check((x), #x, __LINE__)

std::wstring TempFile(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

void TestConfigRoundTrip() {
    Config c;
    Item g;
    g.name = L"Settings";  // группа с именем служебного раздела
    g.path = L"C:\\";
    Item ch;
    ch.name = L"a|b";
    ch.path = L"macro:text x|y; enter";
    ch.icon = L"code.png";
    g.children.push_back(ch);
    Item at;
    at.name = L"@left";
    c.left = {g, at};
    Item r;
    r.name = L"Проекты";
    r.path = L"D:\\Desktop\\components";
    Item rc;
    rc.name = L"Папка\nс переводом";
    rc.path = L"D:\\Desktop\\";
    r.children.push_back(rc);
    c.right = {r};
    std::wstring file = TempFile(L"radial_test_roundtrip.cfg");
    CHECK(SaveConfig(file, c));
    Config back;
    CHECK(LoadConfig(file, back));
    CHECK(back.left.size() == 2 && back.right.size() == 1);
    if (back.left.size() == 2) {
        CHECK(back.left[0].name == L"Settings");
        CHECK(back.left[0].children.size() == 1);
        if (!back.left[0].children.empty()) {
            CHECK(back.left[0].children[0].name == L"a|b");
            CHECK(back.left[0].children[0].path == L"macro:text x|y; enter");
            CHECK(back.left[0].children[0].icon == L"code.png");
        }
        CHECK(back.left[1].name == L"@left");
    }
    if (back.right.size() == 1 && back.right[0].children.size() == 1) {
        CHECK(back.right[0].children[0].name == L"Папкас переводом");
        CHECK(back.right[0].children[0].path == L"D:\\Desktop\\");  // «\» в конце пути не съедает разделитель
    }
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".tmp").c_str());
}

void TestEdgeCheck() {
    std::string on = "{\"startup\":true,\"enabled\":{\"AlwaysOnTop\":false,\"CursorWrap\" : true,\"FancyZones\":true}}";
    std::string off = "{\"enabled\":{\"CursorWrap\":false},\"x\":{\"CursorWrap\":true}}";
    CHECK(edgecheck::PowerToysEnabled(on, "CursorWrap"));
    CHECK(edgecheck::PowerToysEnabled(on, "FancyZones"));
    CHECK(!edgecheck::PowerToysEnabled(on, "AlwaysOnTop"));
    CHECK(!edgecheck::PowerToysEnabled(off, "CursorWrap"));  // включён только вне "enabled" - не считается
    CHECK(!edgecheck::PowerToysEnabled("", "CursorWrap"));
}

void TestDropUndo() {
    std::wstring base = TempFile(L"radial_drop_test");
    std::wstring a = base + L"\\a", b = base + L"\\b";
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW(a.c_str(), nullptr);
    CreateDirectoryW(b.c_str(), nullptr);
    auto put = [](const std::wstring& f) {
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        WriteFile(h, "abc", 3, &w, nullptr);
        CloseHandle(h);
    };
    put(a + L"\\moved.txt");
    put(a + L"\\copied.txt");
    put(a + L"\\waiting.txt");
    ULONGLONG t0 = dropundo::Now();
    MoveFileW((a + L"\\moved.txt").c_str(), (b + L"\\moved.txt").c_str());
    CopyFileW((a + L"\\copied.txt").c_str(), (b + L"\\copied.txt").c_str(), TRUE);
    dropundo::Result r = dropundo::Resolve(
        {a + L"\\moved.txt", a + L"\\copied.txt", a + L"\\waiting.txt", b + L"\\moved.txt"}, b + L"\\", t0);
    CHECK(r.pending == 1);  // waiting.txt ещё «копируется»; бросок в ту же папку не считается
    CHECK(r.done.size() == 2);
    if (r.done.size() == 2) {
        CHECK(!r.done[0].copy && r.done[0].to == b + L"\\moved.txt");
        CHECK(r.done[1].copy && r.done[1].from == a + L"\\copied.txt");
        CHECK(dropundo::Title(r.done, b) == L"Перенесено и скопировано 2 файла в «b»");
    }
    for (const wchar_t* f : {L"\\a\\copied.txt", L"\\a\\waiting.txt", L"\\b\\moved.txt", L"\\b\\copied.txt"})
        DeleteFileW((base + f).c_str());
    RemoveDirectoryW(a.c_str());
    RemoveDirectoryW(b.c_str());
    RemoveDirectoryW(base.c_str());
}

void TestRules() {
    std::wstring bad;
    auto r = rules::Parse({L"app:blender.exe+UnrealEditor.exe -> power:perf, volume:40",
                           L"battery & time:23:00-07:00 -> power:eco, mute, theme:crimson", L"ac -> run:net",
                           L"app:x.exe -> fly", L"time:25:00-07:00 -> mute", L"no arrow"},
                          &bad);
    CHECK(r.size() == 3);
    CHECK(bad == L"app:x.exe -> fly; time:25:00-07:00 -> mute; no arrow");
    if (r.size() == 3) {
        CHECK(r[0].power == L"perf" && r[0].volume == 40 && r[0].when.size() == 1);
        CHECK(r[1].mute && r[1].theme == L"crimson" && r[1].when.size() == 2);
        CHECK(r[2].run == L"net");
        rules::Facts f;
        f.procs = {L"explorer.exe", L"UnrealEditor.exe"};
        f.ac = false;
        f.minutes = 23 * 60 + 30;
        CHECK(rules::Holds(r[0], f));
        CHECK(rules::Holds(r[1], f));  // ночь через полночь, от батареи
        f.minutes = 12 * 60;
        CHECK(!rules::Holds(r[1], f));
        f.minutes = 6 * 60 + 59;
        CHECK(rules::Holds(r[1], f));
        CHECK(!rules::Holds(r[2], f));
        f.procs = {L"explorer.exe"};
        CHECK(!rules::Holds(r[0], f));
    }
    auto low = rules::Parse({L"battery<30 -> power:eco"});
    rules::Facts g;
    g.ac = false;
    g.battery = 25;
    CHECK(low.size() == 1 && rules::Holds(low[0], g));
    g.battery = 45;
    CHECK(low.size() == 1 && !rules::Holds(low[0], g));
}

void TestSettingsPack() {
    std::vector<settingsio::Part> in = {{L"radial.cfg", "[settings]\nx = 1\n"},
                                        {L"tools/ignored.tsv", std::string("a\tb\n\0bin", 9)},
                                        {L"data/empty.tsv", ""}};
    std::string blob = settingsio::Pack(in);
    std::vector<settingsio::Part> out;
    CHECK(settingsio::Unpack(blob, out));
    CHECK(out.size() == 3);
    if (out.size() == 3) {
        CHECK(out[0].name == L"radial.cfg" && out[0].bytes == in[0].bytes);
        CHECK(out[1].bytes == in[1].bytes);  // двоичное и переводы строк внутри - как были
        CHECK(out[2].bytes.empty());
    }
    CHECK(!settingsio::Unpack("not ours", out));
    CHECK(!settingsio::Unpack(blob.substr(0, blob.size() - 3), out));  // обрезанный
    std::string evil = settingsio::Pack({{L"../../x.cfg", "a"}});
    CHECK(!settingsio::Unpack(evil, out));  // выход из папки
}

void TestHealthScore() {
    tools::HealthIn in;
    CHECK(tools::HealthScore(in).score == 100 && tools::HealthScore(in).minus.empty());
    in.disks = {{L"C:\\", 7, 10}, {L"D:\\", 50, -1}};
    in.memLoad = 85;
    in.danger = 3;  // штраф за находки - не больше 30
    in.batteryWear = 12;
    tools::HealthOut o = tools::HealthScore(in);
    CHECK(o.score == 100 - 20 - 10 - 4 - 30);
    CHECK(!o.minus.empty() && o.minus[0].points == 30);  // крупные первыми
    in.smartBad = 5;
    CHECK(tools::HealthScore(in).score == 0);  // не ниже нуля
}

// Процессы сеанса одним вызовом: себя видим с родителем, временем создания и процессором; чужих сеансов и System нет.
void TestProcSession() {
    std::vector<procsnap::Info> list = procsnap::Session();
    CHECK(!list.empty());
    const procsnap::Info* self = nullptr;
    for (const procsnap::Info& p : list) {
        CHECK(p.pid > 4);
        if (p.pid == GetCurrentProcessId()) self = &p;
    }
    CHECK(self != nullptr);
    if (!self) return;
    CHECK(_wcsicmp(self->name.c_str(), L"radial_tests.exe") == 0);
    CHECK(self->parent != 0);
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    CHECK(self->created == (((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime));
    CHECK(self->priv > 0);
}

// Цвета по времени суток: днём как тема (чуть ярче), ночью теплее и темнее, вечер - между ними.
void TestDayTint() {
    Config c;
    c.theme = L"wallpaper";  // кромка не смешивается с обоями: проверяем только время
    c.nightFrom = 23, c.nightTo = 7;
    ThemeInfo base = Themes()[0];
    ThemeInfo noon = DayTint(base, c, 13), eve = DayTint(base, c, 21), deep = DayTint(base, c, 2);
    auto warmth = [](COLORREF x) { return (int)GetRValue(x) - (int)GetBValue(x); };
    CHECK(warmth(deep.glow) > warmth(eve.glow) && warmth(eve.glow) > warmth(noon.glow));
    CHECK(GetRValue(deep.bg) + GetGValue(deep.bg) + GetBValue(deep.bg) <
          GetRValue(base.bg) + GetGValue(base.bg) + GetBValue(base.bg) + 1);
    CHECK(noon.bg == base.bg);
    c.night = false;  // ночной режим выключен - ночь не наступает, но вечер теплеет
    CHECK(DayTint(base, c, 2).bg == base.bg);
}

// История скорости: строка туда и обратно (табуляция в имени не ломает), сравнение - медианы по виду.
void TestSpeedLog() {
    net::Speed s;
    s.when = 133000000000000000ull;
    s.down = 87.5, s.up = 21.25, s.pingMs = 45, s.tunnel = true, s.via = L"happ-tun\t, Финляндия";
    net::Speed back;
    CHECK(net::ParseSpeedRow(net::SpeedRow(s), back));
    CHECK(back.when == s.when && back.down == 87.5 && back.up == 21.25 && back.pingMs == 45 && back.tunnel);
    CHECK(back.via == L"happ-tun , Финляндия");
    CHECK(!net::ParseSpeedRow(L"мусор", back));
    std::vector<net::Speed> h;
    CHECK(net::SpeedCompare(h).empty());
    for (double d : {80.0, 100.0, 90.0}) {
        net::Speed x;
        x.when = 1, x.down = d, x.up = 20, x.tunnel = true;
        h.push_back(x);
    }
    CHECK(net::SpeedCompare(h).empty());  // напрямую ещё не мерили - сравнивать не с чем
    net::Speed d;
    d.when = 1, d.down = 140, d.up = 35;
    h.push_back(d);
    std::wstring c = net::SpeedCompare(h);
    CHECK(c.find(L"↓ 90.0") != std::wstring::npos && c.find(L"↓ 140") != std::wstring::npos);
    net::SpeedMedian m = net::SpeedMedianOf(h, true);
    CHECK(m.n == 3 && m.down == 90 && m.up == 20);
    CHECK(net::SpeedMedianOf(h, false).down == 140);
    // по дням: полдень UTC - в любом поясе России тот же день; в одном дне туннель и напрямую - две записи
    auto at = [](int day, int hour) {
        SYSTEMTIME st{2026, 10, 0, (WORD)day, (WORD)hour, 0, 0, 0};
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    };
    std::vector<net::Speed> w;
    for (int day = 1; day <= 20; ++day) {
        net::Speed x;
        x.when = at(day, 9), x.down = day, x.up = 1, x.tunnel = true;
        w.push_back(x);
        x.when = at(day, 10), x.down = day + 2;
        w.push_back(x);  // медиана двух - среднее
        if (day == 20) {
            x.tunnel = false, x.down = 300;
            w.push_back(x);
        }
    }
    std::vector<net::SpeedDay> days = net::SpeedDays(w, 14);
    CHECK(days.size() == 15);  // 14 дней, в последнем - две записи
    CHECK(days.front().day == 7 && days.front().down == 8 && days.front().n == 2);
    CHECK(days.back().day == 20 && days.back().tunnel && days[13].day == 20 && !days[13].tunnel);
    CHECK(days[13].down == 300);

    // О3: выходы туннеля - страна после последней запятой, быстрые первыми, без страны и напрямую не в счёт
    std::vector<net::Speed> e;
    auto add = [&](double down, const wchar_t* via, bool tunnel = true) {
        net::Speed x;
        x.when = 1, x.down = down, x.up = 10, x.tunnel = tunnel, x.via = via;
        e.push_back(x);
    };
    add(40, L"happ-xray, Нидерланды");
    add(60, L"happ-xray, Швеция");
    add(62, L"happ-xray, Швеция");
    add(500, L"Wi-Fi", false);
    add(90, L"happ-xray");
    std::vector<net::SpeedExit> ex = net::SpeedByExit(e);
    CHECK(ex.size() == 2 && ex[0].country == L"Швеция" && ex[0].down == 61 && ex[0].n == 2);
    CHECK(ex[1].country == L"Нидерланды");
    CHECK(net::SpeedExitAdvice(ex).find(L"«Швеция»") != std::wstring::npos);
    ex[1].down = 58;
    CHECK(net::SpeedExitAdvice(ex).find(L"почти равны") != std::wstring::npos);
    CHECK(net::SpeedExitAdvice({ex[0]}).empty());
}

// Неделя: новое, убранное и обновлённое (та же программа с другой версией); строки снимка туда и обратно.
void TestWeekDiff() {
    std::vector<tools::WeekEntry> old = {
        {L"app", L"Git", L"Git 2.44"}, {L"svc", L"OldSvc", L"Старая служба"}, {L"start", L"Run|Discord", L"Discord"}};
    std::vector<tools::WeekEntry> now = {
        {L"app", L"Git", L"Git 2.45"}, {L"start", L"Run|Discord", L"Discord"}, {L"drv", L"newdrv", L"Новый драйвер"}};
    std::vector<tools::WeekChange> d = tools::WeekDiff(old, now);
    CHECK(d.size() == 3);
    if (d.size() == 3) {
        CHECK(d[0].what == tools::WeekChange::ADDED && d[0].entry.key == L"newdrv");
        CHECK(d[1].what == tools::WeekChange::REMOVED && d[1].entry.key == L"OldSvc");
        CHECK(d[2].what == tools::WeekChange::UPDATED && d[2].was == L"Git 2.44");
    }
    std::vector<tools::WeekEntry> back = tools::ParseWeekRows(tools::WeekRows(now));
    CHECK(back.size() == 3 && back[2].label == L"Новый драйвер");
    CHECK(tools::WeekDiff(now, back).empty());
}

// QR: матрица сверена с библиотекой qrcode (Python) бит в бит на версиях 1-10 - здесь её отпечаток, чтобы правка
// кодировщика не сломала его молча. Строка Wi-Fi экранирует спецсимволы; каналы Wi-Fi по частоте.
void TestQr() {
    qr::Code c = qr::Encode(qr::WifiText("MoonNet", "secret123", false));
    CHECK(c.version == 3 && c.mask == 3 && c.size == 29);
    unsigned long long h = 1469598103934665603ULL;
    for (bool d : c.dark) h = (h ^ (d ? 1 : 0)) * 1099511628211ULL;
    CHECK(h == 0xca221449c689ff79ULL);
    CHECK(qr::WifiText("a;b", "p:1", false) == "WIFI:T:WPA;S:a\\;b;P:p\\:1;;");
    CHECK(qr::WifiText("Free", "", true) == "WIFI:T:nopass;S:Free;;");
    CHECK(qr::Encode(std::string(214, 'x')).size == 0);  // больше версии 10 - не берёмся
    CHECK(net::WifiChannel(2437000) == 6 && net::WifiChannel(5180000) == 36 && net::WifiChannel(2484000) == 14);
    CHECK(net::ChannelsOverlap(6, 9) && !net::ChannelsOverlap(1, 6) && !net::ChannelsOverlap(36, 40));
}

// Права расширений по manifest.json: хосты в host_permissions и content_scripts тоже считаются.
void TestExtRights() {
    std::string mv3 = R"({"manifest_version":3,"permissions":["storage","clipboardRead","nativeMessaging"],
        "host_permissions":["<all_urls>"]})";
    int r = tools::ExtRightsOf(mv3);
    CHECK(r == (tools::EXT_ALL_SITES | tools::EXT_CLIPBOARD | tools::EXT_NATIVE));
    CHECK(tools::ExtRisk(r) >= 4);
    std::string cs = R"({"content_scripts":[{"matches":["https://*/*"],"js":["a.js"]}],"permissions":["tabs"]})";
    CHECK(tools::ExtRightsOf(cs) == tools::EXT_ALL_SITES);
    CHECK(tools::ExtRisk(tools::ExtRightsOf(cs)) == 2);
    CHECK(tools::ExtRightsOf(R"({"permissions":["https://example.com/*","storage"]})") == 0);
    CHECK(tools::ExtRightsText(tools::EXT_CLIPBOARD | tools::EXT_COOKIES) ==
          L"читает буфер обмена, читает куки (входы на сайты)");
}

// Ссылка в песочнице: годится только http(s) без кавычек и пробелов - иначе адрес вышел бы из кавычек команды.
void TestSafeUrl() {
    CHECK(tools::SafeUrl(L"  https://example.com/a?b=1&c=2  ") == L"https://example.com/a?b=1&c=2");
    CHECK(tools::SafeUrl(L"HTTP://x.ru") == L"HTTP://x.ru");
    CHECK(tools::SafeUrl(L"https://x.ru/\" & calc").empty());
    CHECK(tools::SafeUrl(L"https://x.ru/a b").empty());
    CHECK(tools::SafeUrl(L"file:///C:/Windows").empty());
    CHECK(tools::SafeUrl(L"javascript:alert(1)").empty());
    CHECK(tools::SafeUrl(L"https://").empty());
}

}  // namespace

// «Важное»: опасное раньше всего, порядок по важности, не больше 6, пустой снимок - пусто.
void TestImportant() {
    tools::ImportantIn in;
    CHECK(tools::ImportantTodos(in).empty());
    in.findScores = {3, 7, 9};  // «странно» в «Важное» не идёт
    in.git = {{6, 0}, {0, 3}};  // второй репозиторий спокойный
    in.freeable = 3ull << 30;
    in.quickPlaces = 1;
    std::vector<tools::ImportantTodo> t = tools::ImportantTodos(in);
    CHECK(t.size() == 4);
    CHECK(t[0].key == L"sec-find:1" && t[0].why == L"Опасное" && t[1].key == L"sec-find:2");
    CHECK(t[2].key == L"clean:" && t[2].why.find(L"Можно освободить") == 0);
    CHECK(t[3].key == L"git:0");
    in.quickPlaces = 3;
    in.freeable = 1ull << 30;  // меньше 2 ГБ - не дело
    in.broken = in.reboot = in.updatesFailed = in.deviceErrors = in.driverNewer = in.balanceLowered = true;
    in.lowDisks = {L"C:\\"};
    t = tools::ImportantTodos(in);
    CHECK(t.size() == 6);  // предел
    CHECK(t[2].key == L"rec-unbreak" && t[3].key == L"sys-reboot" && t[4].key == L"scan:C:\\" && t[5].key == L"sys-wu");
    CHECK(tools::ImportantTodos(in, 100).size() == 10);
    in.freeable = 5ull << 30;
    CHECK(tools::ImportantTodos(in, 100)[5].key == L"clean-all");

    // О6: от батареи ждём, пока прошлый раз моложе суток; от сети, ни разу и с часами назад - не ждём
    const LONGLONG now = 1792700000, day = 24 * 3600;
    CHECK(tools::HeavyDefer(true, now, now - 3600));
    CHECK(!tools::HeavyDefer(true, now, now - day));
    CHECK(!tools::HeavyDefer(false, now, now - 3600));
    CHECK(!tools::HeavyDefer(true, now, 0));
    CHECK(!tools::HeavyDefer(true, now, now + 3600));
}

void TestMore(void (*check)(bool, const char*, int)) {
    g_check = check;
    TestConfigRoundTrip();
    TestEdgeCheck();
    TestDropUndo();
    TestRules();
    TestSettingsPack();
    TestHealthScore();
    TestProcSession();
    TestDayTint();
    TestSpeedLog();
    TestWeekDiff();
    TestQr();
    TestExtRights();
    TestSafeUrl();
    TestImportant();
}
