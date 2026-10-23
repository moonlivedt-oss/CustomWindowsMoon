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
#include "tools/journal.h"
#include "tools/builderr.h"
#include "tools/devserv.h"
#include "tools/drain.h"
#include "core/cfgdiff.h"
#include "app/weekreport.h"
#include "app/resume.h"
#include "app/afterwork.h"
#include "app/gameprep.h"
#include "app/focus.h"
#include "desk/appvol.h"
#include "desk/cardlog.h"

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
    // простой, сеть по имени, «не»
    auto more = rules::Parse(
        {L"idle>15 & !net:Bubuka* -> power:eco", L"net:Office+Work -> mute", L"!! ac -> mute", L"idle>0 -> mute"},
        &bad);
    CHECK(more.size() == 2);
    if (more.size() == 2) {
        rules::Facts g;
        g.idleMin = 20;
        g.networks = {L"Bubuka_5G 2"};
        CHECK(!rules::Holds(more[0], g));  // дома - нет
        g.networks = {L"Cafe"};
        CHECK(rules::Holds(more[0], g));
        g.idleMin = 3;
        CHECK(!rules::Holds(more[0], g));
        g.networks = {L"Cafe", L"Work"};
        CHECK(rules::Holds(more[1], g));
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
// О8: диапазон Wi-Fi по скорости связи, когда канал не отдают
void TestWifiBand() {
    CHECK(net::WifiBandGuess(1201) == L"скорее 5 или 6 ГГц");
    CHECK(net::WifiBandGuess(144) == L"скорее 2,4 ГГц");
    CHECK(net::WifiBandGuess(300).empty());  // бывает и там, и там
    CHECK(net::WifiBandGuess(0).empty());
}

// О5: итог обхода на диске - туда и обратно, табуляция в именах не ломает строку
void TestScanCache() {
    tools::ScanCache c;
    c.when = 1792700000, c.ddcOld = 123;
    tools::Junk j;
    j.name = L"Кэш\tnpm", j.path = L"C:\\npm", j.bytes = 7, j.fresh = 2, j.slow = true;
    c.junk.push_back(j);
    tools::ProjectJunk p;
    p.project = L"D:\\proj", p.name = L"proj", p.path = L"D:\\proj\\node_modules", p.kind = L"node_modules";
    p.bytes = 99, p.idleDays = 40;
    c.projects.push_back(p);
    tools::ScanCache b;
    CHECK(tools::ParseScanCache(tools::ScanCacheRows(c), b));
    CHECK(b.when == c.when && b.ddcOld == 123 && b.junk.size() == 1 && b.projects.size() == 1);
    CHECK(b.junk[0].name == L"Кэш npm" && b.junk[0].bytes == 7 && b.junk[0].fresh == 2 && b.junk[0].slow);
    CHECK(b.projects[0].path == p.path && b.projects[0].idleDays == 40 && b.projects[0].bytes == 99);
    CHECK(!tools::ParseScanCache(L"junk\tбез времени\t\t1\t0\t0\t0", b));
}

// П11: radial.cfg по смыслу - настройки, секторы, пункты по подписи, порядок
void TestCfgDiff() {
    std::wstring a =
        L"# комментарий\n[settings]\ntheme = crimson\nsounds = 1\n[@left]\n[Код]\npath = D:\\Код\n"
        L"item = web | D:\\web | icons/web.png\nitem = java | D:\\java\n[Учёба]\npath = D:\\У\n"
        L"[@right]\n[Музыка]\npath = D:\\M\n";
    std::wstring b =
        L"[settings]\ntheme = sunset\nsounds = 1\nvolume = 35\n[@left]\n[Учёба]\npath = D:\\У\n"
        L"[Код]\npath = D:\\Код\nitem = web | D:\\web2 | icons/web.png\nitem = C++ | D:\\cpp\n"
        L"[@right]\n[Minecraft]\npath = C:\\Prism\n";
    std::vector<cfgdiff::Change> d = cfgdiff::Diff(a, b);
    auto has = [&](cfgdiff::Change::Kind k, const std::wstring& where, const std::wstring& what) {
        for (const cfgdiff::Change& c : d)
            if (c.kind == k && c.where == where && c.what.find(what) != std::wstring::npos) return true;
        return false;
    };
    CHECK(has(cfgdiff::Change::CHANGED, L"Настройки", L"theme: crimson -> sunset"));
    CHECK(has(cfgdiff::Change::ADDED, L"Настройки", L"volume: 35"));
    CHECK(has(cfgdiff::Change::CHANGED, L"Левый край", L"порядок секторов"));
    CHECK(has(cfgdiff::Change::CHANGED, L"Левый край / Код", L"пункт «web»"));
    CHECK(has(cfgdiff::Change::ADDED, L"Левый край / Код", L"пункт «C++»"));
    CHECK(has(cfgdiff::Change::REMOVED, L"Левый край / Код", L"пункт «java»"));
    CHECK(has(cfgdiff::Change::ADDED, L"Правый край", L"сектор «Minecraft»"));
    CHECK(has(cfgdiff::Change::REMOVED, L"Правый край", L"сектор «Музыка»"));
    CHECK(d.size() == 8);
    CHECK(cfgdiff::Summary(d) == L"+3, -2, изменено 3");
    CHECK(cfgdiff::Diff(a, a).empty() && cfgdiff::Summary({}).empty());
}

// П12: числа отчёта туда и обратно, разница к прошлой неделе - без шума
void TestWeekReport() {
    const ULONGLONG GB = 1024ull * 1024 * 1024;
    weekreport::Data a;
    a.when = 1792604800, a.to = L"23.10", a.drops = 3, a.tunnelDown = 46, a.directDown = 105, a.batteryWear = 11;
    a.disks = {{L"C:\\", 140 * GB, 475 * GB}, {L"D:\\", 211 * GB, 476 * GB}};
    weekreport::Data b;
    CHECK(weekreport::ParseNumbers(weekreport::NumbersRow(a), b));
    CHECK(b.when == a.when && b.to == L"23.10" && b.drops == 3 && b.tunnelDown == 46 && b.batteryWear == 11);
    CHECK(b.disks.size() == 2 && b.disks[1].free == 211 * GB && b.disks[0].root == L"C:\\");
    weekreport::Data prev = a;
    prev.when -= 7 * 24 * 3600, prev.drops = 1, prev.tunnelDown = 38, prev.directDown = 100, prev.batteryWear = 10;
    prev.disks[0].free = 152 * GB, prev.disks[1].free = 211 * GB + GB / 2;  // D: - меньше гигабайта, шум
    std::vector<std::wstring> d = weekreport::Deltas(a, prev);
    CHECK(d.size() == 4);
    CHECK(d[0].find(L"C: свободно -12") == 0);
    CHECK(d[1] == L"обрывов интернета 3 (было 1)");
    CHECK(d[2].find(L"через туннель быстрее на 21%") == 0);  // напрямую +5% - шум, не пишется
    CHECK(d[3] == L"износ батареи 11% (было 10%)");
    CHECK(weekreport::Deltas(a, weekreport::Data()).empty());  // сравнивать не с чем
}

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

// Здоровье программы (Л27): поломки отдельно от выключенного нарочно, одной строкой - что отвалилось
void TestAppHealth() {
    tools::AppHealthIn in;
    in.mouseWanted = in.mouseHooked = true;
    in.autostartOn = true;
    in.adminReady = true;
    in.watchAgeMs = 30000;
    in.uptimeMs = 3600000;
    std::vector<tools::AppPart> v = tools::AppHealth(in);
    bool allOk = v.size() == 5;
    for (const tools::AppPart& p : v) allOk = allOk && p.state == tools::AppPart::OK;
    CHECK(allOk);
    CHECK(tools::AppHealthLine(v).rfind(L"Всё работает: перехват мыши", 0) == 0);
    in.mouseSilent = true;
    in.watchAgeMs = 7 * 60000;
    v = tools::AppHealth(in);
    CHECK(v[0].state == tools::AppPart::BAD && v[0].fix == 1);
    CHECK(v[4].state == tools::AppPart::BAD && v[4].why == L"молчит 7 мин");
    CHECK(tools::AppHealthLine(v).rfind(L"Отвалилось: перехват мыши", 0) == 0);
    in = {};  // в игре перехват снят нарочно, автозапуск не хотели - не поломки
    in.gameMode = true;
    in.uptimeMs = 10000;
    v = tools::AppHealth(in);
    CHECK(v[0].state == tools::AppPart::OFF && v[1].state == tools::AppPart::OFF && v[4].state == tools::AppPart::OK);
    in.autostartWanted = 1;
    in.toolsExe = false;
    in.uptimeMs = 3600000;  // за час слежка ни разу не прошла
    v = tools::AppHealth(in);
    CHECK(v[1].state == tools::AppPart::BAD && v[2].state == tools::AppPart::BAD && v[4].state == tools::AppPart::BAD);
}

// Память карточек (Л10/Л11): вид без чисел, третье закрытие подряд - вопрос один раз, действие обнуляет счёт
void TestCardLog() {
    CHECK(cardlog::KindOf(L"Освободить ~1.2 ГБ перед игрой?") == L"Освободить");
    CHECK(cardlog::KindOf(L"Загрузки: 2,4 ГБ лишнего") == L"Загрузки");
    CHECK(cardlog::KindOf(L"Пропал интернет - починить?") == L"Пропал интернет - починить?");
    CHECK(cardlog::KindOf(L"7 дней без копии") == L"7 дней без копии");  // число в начале - вид весь заголовок
    cardlog::Strikes st;
    CHECK(!st.Closed(L"a", false) && !st.Closed(L"a", false));
    CHECK(st.Closed(L"a", false));   // третье подряд
    CHECK(!st.Closed(L"a", false));  // дальше не спрашиваем снова
    CHECK(!st.Closed(L"b", false) && !st.Closed(L"b", false) && !st.Closed(L"b", true));
    CHECK(!st.Closed(L"b", false) && !st.Closed(L"b", false) && st.Closed(L"b", false));  // после действия - с нуля
    std::set<std::wstring> m{L"Загрузки", L"Выход сменился"};
    CHECK(cardlog::ParseMutes(cardlog::MutesText(m)) == m);
    CHECK(cardlog::ParseMutes(L"a\r\n\nb\r\n") == (std::set<std::wstring>{L"a", L"b"}));
}

// Отмена записи журнала (Л3): упаковка туда и обратно, путь с «|» не упаковывается, третий столбец читается
void TestJournalUndo() {
    std::wstring u = tools::UndoPack(L"recycle", {L"C:\\Dl\\a.exe", L"C:\\Dl\\bad|name.zip", L"C:\\Dl\\b (1).zip"});
    CHECK(u == L"recycle|C:\\Dl\\a.exe|C:\\Dl\\b (1).zip");
    tools::Undo p;
    CHECK(tools::UndoParse(u, p) && p.kind == L"recycle" && p.args.size() == 2 && p.args[1] == L"C:\\Dl\\b (1).zip");
    CHECK(tools::UndoParse(L"power|1\r", p) && p.kind == L"power" && p.args[0] == L"1");
    CHECK(!tools::UndoParse(L"", p) && !tools::UndoParse(L"cfg", p) && !tools::UndoParse(L"|x", p));
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring d = std::wstring(dir) + L"radial_undo_test";
    CreateDirectoryW(d.c_str(), nullptr);
    DeleteFileW((d + L"\\journal.tsv").c_str());
    DeleteFileW((d + L"\\journal.old.tsv").c_str());
    tools::JournalAdd(d, L"23.10 10:00", L"без отмены");
    tools::JournalAdd(d, L"23.10 10:01", L"Питание: экономия", 64 * 1024, L"power|1");
    std::vector<tools::JournalEntry> e = tools::JournalRead(d, 10);
    CHECK(e.size() == 2 && e[0].text == L"Питание: экономия" && e[0].undo == L"power|1" && e[1].undo.empty());
    DeleteFileW((d + L"\\journal.tsv").c_str());
    RemoveDirectoryW(d.c_str());
}

// Сломанная сборка (Л31): первая настоящая ошибка у MSVC/UBT, GCC/Clang, линкера и CMake, подсказки, место для VS Code
void TestBuildErr() {
    builderr::Error e;
    std::wstring ubt =
        L"Building MyGameEditor...\r\n"
        L"[1/3] Compile [x64] Player.cpp\r\n"
        L"D:\\Games\\My\\Source\\My\\Player.cpp(42,17): error C2065: 'Hp': undeclared identifier\r\n"
        L"D:\\Games\\My\\Source\\My\\Player.cpp(50): error C2143: syntax error: missing ';' before '}'\r\n"
        L"Result: Failed (OtherCompilationError)\r\n";
    CHECK(builderr::First(ubt, e) && e.line == 42 && e.col == 17 && e.code == L"C2065" &&
          e.file == L"D:\\Games\\My\\Source\\My\\Player.cpp" && e.text == L"'Hp': undeclared identifier");
    CHECK(builderr::Plain(e) == L"Player.cpp, строка 42: C2065 'Hp': undeclared identifier");
    CHECK(builderr::Hint(e).find(L"не объявлено") != std::wstring::npos);
    CHECK(builderr::GotoArg(e, L"") == L"D:\\Games\\My\\Source\\My\\Player.cpp:42:17");
    std::wstring gcc =
        L"[3/10] Building CXX object CMakeFiles/app.dir/src/main.cpp.obj\r\n"
        L"FAILED: CMakeFiles/app.dir/src/main.cpp.obj\r\n"
        L"D:/Proj/src/main.cpp: In function 'int main()':\r\n"
        L"D:/Proj/src/main.cpp:12:5: error: 'foo' was not declared in this scope\r\n"
        L"ninja: build stopped: subcommand failed.\r\n";
    CHECK(builderr::First(gcc, e) && e.file == L"D:/Proj/src/main.cpp" && e.line == 12 && e.col == 5 &&
          e.code.empty() && e.text == L"'foo' was not declared in this scope");
    CHECK(builderr::GotoArg(e, L"") == L"D:\\Proj\\src\\main.cpp:12:5");
    std::wstring rel = L"../src/a.cpp:7: fatal error: b.h: No such file or directory\n";
    CHECK(builderr::First(rel, e) && e.file == L"../src/a.cpp" && e.line == 7 && e.col == 0);
    CHECK(builderr::GotoArg(e, L"D:\\Proj\\build") == L"D:\\Proj\\src\\a.cpp:7");
    CHECK(builderr::Hint(e).find(L"заголовок") != std::wstring::npos);
    std::wstring lnk =
        L"Module.My.cpp.obj : error LNK2019: unresolved external symbol \"void Foo()\"\r\n"
        L"D:\\Games\\My\\Binaries\\Win64\\UnrealEditor-My.dll : fatal error LNK1120: 1 unresolved externals\r\n";
    CHECK(builderr::First(lnk, e) && e.code == L"LNK2019" && e.file.empty() && e.line == 0);
    CHECK(builderr::Hint(e).find(L"Build.cs") != std::wstring::npos && builderr::GotoArg(e, L"").empty());
    std::wstring ld =
        L"collect2.exe: error: ld returned 1 exit status\nmain.cpp:(.text+0x1e): undefined reference to `bar()'\n";
    CHECK(builderr::First(ld, e) && e.text == L"undefined reference to `bar()'");  // итоговая строка не в счёт
    std::wstring cm =
        L"-- Configuring incomplete\nCMake Error at CMakeLists.txt:12 (add_executable):\n  Cannot find source file:\n";
    CHECK(builderr::First(cm, e) && e.file == L"CMakeLists.txt" && e.line == 12 &&
          e.text == L"Cannot find source file:");
    std::wstring live =
        L"Unable to build while Live Coding is active. Exit the editor and game, or press Ctrl+Alt+F11\n"
        L"ERROR: Unable to build while Live Coding is active.\n";
    CHECK(builderr::First(live, e) && builderr::Hint(e).find(L"Live Coding") != std::wstring::npos);
    CHECK(!builderr::First(L"Result: Succeeded\nTotal execution time: 3.1 seconds\n", e));
}

// Забытые серверы (Л19): проект виден в заголовке - не забыт; не виден час и ест память - забыт, один на проект
void TestDevServers() {
    CHECK(tools::DevServerName(L"node.exe") && tools::DevServerName(L"Python3.11.exe") &&
          !tools::DevServerName(L"Code.exe"));
    CHECK(tools::ProjectInTitle(L"D:\\Web\\shop", L"App.tsx - shop - Visual Studio Code"));
    CHECK(!tools::ProjectInTitle(L"D:\\Web\\shop", L"workshop - Visual Studio Code"));  // целым словом
    CHECK(tools::ProjectInTitle(L"D:\\Web\\my-site\\", L"PowerShell D:\\Web\\my-site"));
    std::vector<tools::DevServer> s = {{10, L"node.exe", L"D:\\Web\\shop", 5173, 300ull << 20},
                                       {10, L"node.exe", L"D:\\Web\\shop", 24678, 300ull << 20},  // тот же процесс
                                       {11, L"python.exe", L"D:\\Py\\bot", 8000, 30ull << 20}};
    std::map<std::wstring, ULONGLONG> seen;
    ULONGLONG h = 3600 * 1000;
    CHECK(tools::ForgottenServers(s, {L"shop - Visual Studio Code"}, seen, 1000).empty());  // первая встреча
    CHECK(tools::ForgottenServers(s, {}, seen, 1000 + h - 60000).empty());                  // ещё не час
    std::vector<tools::Forgotten> f = tools::ForgottenServers(s, {}, seen, 1000 + h + 60000);
    CHECK(f.size() == 1 && f[0].project == L"D:\\Web\\shop" && f[0].servers.size() == 2 &&
          f[0].bytes == (300ull << 20) && f[0].minutes == 61);  // бот ест мало - не в счёт, память процесса - один раз
    CHECK(tools::ForgottenServers(s, {L"Terminal - shop"}, seen, 1000 + 2 * h).empty());  // проект снова открыт
}

// Порт занят (Л18): на каком порту проект собирался слушать - явный флаг первым, потом скрипты, .env, обычные порты
void TestProjectPorts() {
    CHECK(tools::ProjectPorts(L"node server.js --port 4000", L"", L"") == std::vector<int>{4000});
    CHECK(tools::ProjectPorts(L"python -m http.server 9001", L"", L"") == (std::vector<int>{9001, 8000}));
    CHECK(tools::ProjectPorts(L"python manage.py runserver 0.0.0.0:8100", L"", L"") == (std::vector<int>{8100, 8000}));
    std::wstring pkg = L"{\"name\":\"shop\",\"scripts\":{\"dev\":\"vite --host\",\"build\":\"vite build\"}}";
    CHECK(tools::ProjectPorts(L"node node_modules\\vite\\bin\\vite.js", pkg, L"") == std::vector<int>{5173});
    std::wstring pkg2 = L"{\"scripts\": {\"start\": \"next start -p 3100\"}}";
    CHECK(tools::ProjectPorts(L"node server.js", pkg2, L"DEBUG=1\nPORT=3200\n") ==
          (std::vector<int>{3100, 3200, 3000}));
    CHECK(tools::ProjectPorts(L"node index.js", L"{\"scripts\":{\"test\":\"jest\"}}", L"").empty());  // не угадать
}

// Что посадило батарею (Л38): программы складываются в группы, мелочь не в счёт, коротко - молчим
void TestDrain() {
    CHECK(tools::DrainGroup(L"msedge.exe") == L"браузер" && tools::DrainGroup(L"ShaderCompileWorker.exe") == L"Unreal");
    CHECK(tools::DrainGroup(L"Telegram.exe") == L"Telegram" && tools::DrainGroup(L"foo.exe") == L"foo");
    tools::DrainIn in;
    in.minutes = 125;
    in.fromPct = 95;
    in.toPct = 35;
    in.cpu = {{L"chrome.exe", 400},
              {L"msedge.exe", 100},
              {L"UnrealEditor.exe", 300},
              {L"Discord.exe", 160},
              {L"svchost.exe", 40}};
    tools::DrainOut o;
    CHECK(tools::DrainSummary(in, o));
    CHECK(o.title == L"От батареи 2 ч 5 мин: ушло 60%");
    CHECK(o.top.size() == 3 && o.top[0].first == L"браузер" && o.top[0].second == 50 && o.top[1].first == L"Unreal" &&
          o.top[2].first == L"Discord");  // svchost - 4%, не в счёт
    CHECK(o.text == L"Больше всего процессора: браузер (50%), Unreal (30%) и Discord (16%). Расход 28% в час.");
    in.minutes = 15;
    CHECK(!tools::DrainSummary(in, o));  // меньше 20 минут
    in.minutes = 60;
    in.toPct = 93;
    CHECK(!tools::DrainSummary(in, o));  // ушло 2%
}

// Громкость по программам (Л36): новый сеанс получает запомненное, перемену уровня у знакомого - запомнить
void TestAppVolume() {
    std::map<DWORD, float> last;
    std::map<std::wstring, float> saved{{L"discord", 0.3f}};
    // первый проход после старта: только смотрим
    appvol::Plan p = appvol::PlanVolumes({{10, L"discord", 1.0f}, {11, L"game", 0.8f}}, last, saved, true);
    CHECK(p.set.empty() && p.save.empty() && last.size() == 2);
    // вы сделали игру тише - запомнить; Discord как был - ничего
    p = appvol::PlanVolumes({{10, L"discord", 1.0f}, {11, L"game", 0.5f}}, last, saved, false);
    CHECK(p.set.empty() && p.save.size() == 1 && p.save[L"game"] == 0.5f);
    // Discord перезапустился (новый pid) - вернуть 0.3; игра закрылась - забыта в last; системные звуки (pid 0) мимо
    p = appvol::PlanVolumes({{20, L"discord", 1.0f}, {0, L"", 1.0f}}, last, saved, false);
    CHECK(p.set.size() == 1 && p.set[0].first == 20 && p.set[0].second == 0.3f && last.size() == 1 && last[20] == 0.3f);
    // следующий проход видит уже 0.3 - не «поменяли вы»
    p = appvol::PlanVolumes({{20, L"discord", 0.3f}}, last, saved, false);
    CHECK(p.set.empty() && p.save.empty());
    std::map<std::wstring, float> m{{L"discord", 0.3f}, {L"nfs14", 0.85f}};
    std::map<std::wstring, float> back = appvol::ParseSaved(appvol::SavedText(m));
    CHECK(back.size() == 2 && std::fabs(back[L"nfs14"] - 0.85f) < 0.001f);
}

// «Продолжить, где остановился» (Л4): снимок туда и обратно, папки VS Code, чего нет сейчас, текст карточки
void TestResume() {
    CHECK(resume::FromFileUri(L"file:///d%3A/Desktop/C%2B%2B") == L"D:\\Desktop\\C++");
    CHECK(resume::FromFileUri(L"file:///c%3A/Users/%D0%9C%D0%BE%D1%8F") == L"C:\\Users\\Моя");  // UTF-8 в URI
    CHECK(resume::FromFileUri(L"vscode-remote://wsl/x").empty());
    std::wstring json =
        L"{\"backupWorkspaces\": {\"workspaces\": [], \"folders\": [{\"folderUri\": \"file:///d%3A/Web/shop\"}], "
        L"\"emptyWindows\": []}, \"profileAssociations\": {\"workspaces\": {\"file:///d%3A/Web/shop\": "
        L"\"__default__profile__\", \"file:///d%3A/Games/MyGame\": \"__default__profile__\", \"file:///c%3A/a/tools\": "
        L"\"__default__profile__\", \"file:///d%3A/b/tools\": \"__default__profile__\"}}}";
    std::vector<std::wstring> f = resume::CodeFolders(
        json, {L"main.cpp - MyGame - Visual Studio Code", L"x.py - tools - Visual Studio Code", L"Telegram"});
    CHECK(f.size() == 2 && f[0] == L"D:\\Web\\shop" &&
          f[1] == L"D:\\Games\\MyGame");  // «tools» - две папки, не угадываем
    resume::Snapshot s;
    s.when = 1792700000;
    s.things = {{resume::Thing::CODE, L"D:\\Web\\shop", L"shop"},
                {resume::Thing::FOLDER, L"C:\\Users\\Me\\Downloads", L"Downloads"},
                {resume::Thing::UNREAL, L"D:\\Games\\MyGame\\MyGame.uproject", L"MyGame"},
                {resume::Thing::APP, L"C:\\Program Files\\Blender\\blender.exe", L"blender"}};
    resume::Snapshot back;
    CHECK(resume::Parse(resume::Save(s), back) && back.when == s.when && back.things.size() == 4 &&
          back.things[2].kind == resume::Thing::UNREAL && back.things[3].label == L"blender");
    std::vector<resume::Thing> miss = resume::Missing(s, {{resume::Thing::CODE, L"d:\\web\\SHOP", L"shop"}});
    CHECK(miss.size() == 3 && miss[0].kind == resume::Thing::FOLDER);
    CHECK(resume::Describe(miss) == L"Unreal: MyGame; папки: Downloads; программы: blender");
    CHECK(resume::Describe(s.things, 30).size() == 30);
}

// Сон после работы (Л6): что считается работой и отсчёт - ждём тишины, потом минута, работа снова пошла - с начала
void TestAfterWork() {
    std::vector<afterwork::ProcNow> ps = {{L"explorer.exe", 0.01}, {L"blender.exe", 0.05}};
    CHECK(afterwork::BusyReason(ps, L"ninja.exe|UnrealBuildTool.exe", 0, false).empty());
    ps[1].cpu = 0.6;  // рендер
    CHECK(afterwork::BusyReason(ps, L"ninja.exe", 0, false) == L"рендер Blender");
    ps.push_back({L"UnrealBuildTool.exe", 0.3});
    CHECK(afterwork::BusyReason({{L"UnrealBuildTool.exe", 0}}, L"ninja.exe|UnrealBuildTool.exe", 0, false) ==
          L"UnrealBuildTool.exe");
    CHECK(afterwork::BusyReason({}, L"", 2, false) == L"загрузка в браузере");
    CHECK(afterwork::BusyReason({}, L"", 0, true) == L"загрузка Steam");
    afterwork::Countdown c;
    CHECK(c.Feed(true, 0) == afterwork::Countdown::WAIT);
    CHECK(c.Feed(false, 1000) == afterwork::Countdown::WAIT);     // затихло - ждём 2 минуты
    CHECK(c.Feed(false, 121000) == afterwork::Countdown::COUNT);  // отсчёт пошёл
    CHECK(c.SecondsLeft(121000) == 60);
    CHECK(c.Feed(true, 150000) == afterwork::Countdown::WAIT);  // работа снова - с начала
    CHECK(c.Feed(false, 151000) == afterwork::Countdown::WAIT && c.Feed(false, 271000) == afterwork::Countdown::COUNT);
    CHECK(c.Feed(false, 331000) == afterwork::Countdown::ACT);
}

// Подготовка к игре (Л21): мешает - строками, кнопка - самое полезное (обновления важнее Steam), нечему - молчим
void TestGamePrep() {
    gameprep::In in;
    gameprep::Out o;
    CHECK(!gameprep::Check(in, o));
    in.downMbps = 5;  // мелочь
    CHECK(!gameprep::Check(in, o));
    in.battery = true;
    in.percent = 41;
    CHECK(gameprep::Check(in, o) && o.title == L"Игра от батареи" && o.action == gameprep::Out::NONE && o.yes.empty());
    CHECK(o.text.find(L"(41%)") != std::wstring::npos);
    in.steam = true;
    in.partials = 2;  // Steam и браузер - одна строка про загрузку
    CHECK(gameprep::Check(in, o) && o.title == L"Перед игрой" && o.action == gameprep::Out::OPEN_STEAM_DOWNLOADS);
    CHECK(o.text.find(L"Браузер") == std::wstring::npos && o.text.find(L"\n") != std::wstring::npos);
    in.updates = true;
    CHECK(gameprep::Check(in, o) && o.action == gameprep::Out::OPEN_UPDATES && o.yes == L"Отложить обновления");
    in = {};
    in.downMbps = 45;
    CHECK(gameprep::Check(in, o) && o.text.find(L"45 Мбит/с") != std::wstring::npos);
}

// Фокус и шахматы (Л8, Л34): маска в заголовке окна без учёта регистра, строка отсчёта
void TestFocus() {
    std::vector<std::wstring> t = {L"Telegram", L"Партия с Magnus • lichess.org - Google Chrome"};
    CHECK(focus::AutoMatch(t, L"lichess.org|chess.com") == L"lichess.org");
    CHECK(focus::AutoMatch({L"Play Chess Online - Chess.com - Opera"}, L" lichess.org | chess.com ") == L"chess.com");
    CHECK(focus::AutoMatch(t, L"").empty() && focus::AutoMatch(t, L"ab|x").empty());  // короткие маски не в счёт
    CHECK(focus::LeftText(23 * 60000 + 1) == L"Фокус: ещё 24 мин");
    CHECK(focus::LeftText(50 * 60000) == L"Фокус: ещё 50 мин");
    CHECK(focus::LeftText(59000) == L"Фокус: меньше минуты");
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
    TestWifiBand();
    TestScanCache();
    TestCfgDiff();
    TestWeekReport();
    TestAppHealth();
    TestCardLog();
    TestJournalUndo();
    TestBuildErr();
    TestDevServers();
    TestProjectPorts();
    TestDrain();
    TestAppVolume();
    TestResume();
    TestAfterWork();
    TestGamePrep();
    TestFocus();
}
