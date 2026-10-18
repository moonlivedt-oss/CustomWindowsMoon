// Живые сцены части 2 в студии (gfx/scene): маршрут сети, кабели портов, дорожка входа, колба памяти,
// батарея, табло версий. Данные настоящие: net, tools, слежка toolsui. RADIAL_DEMO=1 - выдуманные, для снимков
// и фильмов без мыши; тогда сцены ничего не завершают и не выключают.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "app/toolsui.h"
#include "core/bg.h"
#include "core/fx.h"
#include "core/text.h"
#include "gfx/color.h"
#include "net/net.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;

namespace studio {

namespace {

bool Demo() {
    static bool d = getenv("RADIAL_DEMO") != nullptr;
    return d;
}

double DemoT() {
    static ULONGLONG t0 = GetTickCount64();
    return (GetTickCount64() - t0) / 1000.0;
}

float Jit(float a) { return (rand() % 2001 - 1000) / 1000.0f * a; }

std::wstring NoExe(const std::wstring& s) { return EndsWithI(s, L".exe") ? s.substr(0, s.size() - 4) : s; }

std::wstring Leaf(const std::wstring& p) {
    std::wstring s = p;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    size_t cut = s.find_last_of(L"\\/");
    return cut == std::wstring::npos ? s : s.substr(cut + 1);
}

void Push(std::vector<float>& h, float v) {
    h.push_back(v);
    if (h.size() > 32) h.erase(h.begin());
}

// Замеры раз в 1,5 с, пока раздел «Сеть» смотрит (seen свежий): пинг шлюза, HTTPS-круг до выхода (раз в
// 4,5 с), TCP до 1.1.1.1 на 53 и 443 - через туннель TUN они отвечают сами за 0 мс, такие замеры не в счёт.
// пока страница смотрит (seen свежее 4 с) и программа не закрывается
void SampleThread(const std::shared_ptr<NetSampler>& s) {
    int n = 0, httpFail = 0;
    float http = -1;
    std::wstring gw, ad, country;
    bool tunnel = false;
    while (GetTickCount64() - s->seen < 4000 && !bg::Quitting()) {
        if (n % 20 == 0) {
            gw = net::Gateway(&ad);
            std::wstring rt;
            tunnel = net::RouteTo(L"1.1.1.1", &rt);
        }
        float g = gw.empty() ? -1.0f : (float)net::Ping(gw, 1, 1000);
        if (n % 3 == 0) {
            int h = net::HttpRtt(3000);
            httpFail = h < 0 ? httpFail + 1 : 0;
            http = h >= 0 ? (float)h : -1.0f;
        }
        int d = net::TcpRtt(L"1.1.1.1", 53, 1500), i = net::TcpRtt(L"1.1.1.1", 443, 1500);
        float fd = d < 0 || net::Faked(d) ? -1.0f : (float)d, fi = i < 0 || net::Faked(i) ? -1.0f : (float)i;
        float wan = http >= 0 ? std::max(1.0f, http - std::max(0.0f, g)) : -1.0f;
        std::vector<float> links =
            tunnel ? std::vector<float>{g, wan, fd, fi}
                   : std::vector<float>{g, fd >= 0 ? std::max(1.0f, fd - std::max(0.0f, g)) : -1.0f, wan};
        if (n % 40 == 2) {
            net::Exit e = net::ExitNode(4000);
            if (e.ok) country = e.country;
        }
        AcquireSRWLockExclusive(&s->lock);
        if (s->hist.size() != links.size()) s->hist.assign(links.size(), {});
        for (size_t q = 0; q < links.size(); ++q) Push(s->hist[q], links[q]);
        s->gateway = gw;
        s->adapter = ad;
        s->tunnel = tunnel;
        s->country = country;
        s->gwDown = !gw.empty() && g < 0;
        s->httpDown = httpFail >= 2;
        s->gen++;
        ReleaseSRWLockExclusive(&s->lock);
        ++n;
        Sleep(1500);
    }
    s->running = false;
}

// выдуманная сеть как в наброске: через 2,6 с выход меняется, через 5,2 с рвётся DNS, «Починить» - 1,4 с
struct DemoNet {
    std::vector<std::vector<float>> hist = std::vector<std::vector<float>>(4);
    double sampled = 0, fixAt = -1;
    bool fixed = false, swapped = false;
    int gen = 0;
};
DemoNet g_demoNet;

}  // namespace

scene::Palette Studio::ScenePalette() {
    const UiColors& c = ui.C();
    return scene::Palette{c.text, c.muted, c.faint, c.accent, c.accent2, c.gold, c.card};
}

// Сцена берёт курсор и кнопку, пока курсор над ней или её тянут; каждое движение мыши над ней - кадр.
scene::Input Studio::SceneInput(int id, const RectF& r) {
    scene::Input in;
    ui.Hover(id, r);
    ui.Track(id);
    if (ui.interactive && (ui.Hot(id) || ui.Active(id))) {
        in.m = ui.Mouse();
        in.down = ui.Down() && ui.Active(id);
    }
    return in;
}

void Studio::LiveData() {
    ULONGLONG now = GetTickCount64();
    if (liveAt_ && now - liveAt_ < 3000) return;
    liveAt_ = now;
    liveMem_ = tools::Memory();
    liveApps_ = tools::HeavyApps(6);
    liveBat_ = tools::Battery();
    liveWatch_ = toolsui::Watch();
}

// ---------------- сеть ----------------

void Studio::RouteLive(const RectF& r, bool busy) {
    scene::RouteData d;
    std::wstring title;
    if (netJob_) title = netJob_->title;
    if (Demo()) {
        DemoNet& dn = g_demoNet;
        double t = DemoT();
        bool exitSE = t > 2.6;
        if (t - dn.sampled > 0.18) {
            dn.sampled = t;
            float base[4] = {3, 11, exitSE ? 38.0f : 42.0f, 18};
            for (int i = 0; i < 4; ++i) {
                float v = base[i] + Jit(3);
                if (i == 1 && rand() % 20 == 0) v += 60 + Jit(40) + 40;
                if (i == 2 && exitSE && !dn.swapped) v += 70, dn.swapped = true;
                Push(dn.hist[i], v);
            }
            dn.gen++;
        }
        bool fixing = dn.fixAt >= 0 && t - dn.fixAt < 1.4;
        if (dn.fixAt >= 0 && !fixing) dn.fixed = true;
        d.nodes = {{L"Этот ПК", L"Wi-Fi 5 ГГц", L"ПК"},
                   {L"Роутер", L"192.168.1.1", L"Р"},
                   {L"Туннель", exitSE ? L"Швеция" : L"Финляндия", exitSE ? L"SE" : L"FI"},
                   {L"DNS", L"1.1.1.1", L"DNS"},
                   {L"Интернет", L"github.com", L"www"}};
        d.nodes[3].ok = !(t > 5.2 && !dn.fixed);
        d.hist = dn.hist;
        d.every = 0.18f;
        d.tunnel = 2;
        d.fixing = fixing;
        d.gen = dn.gen;
    } else {
        ULONGLONG now = GetTickCount64();
        if (!sampler_ || !sampler_->running) {
            sampler_ = std::make_shared<NetSampler>();
            sampler_->running = true;
            sampler_->seen = now;
            std::shared_ptr<NetSampler> s = sampler_;
            if (!bg::Run([s] { SampleThread(s); })) sampler_->running = false;
        }
        sampler_->seen = now;
        AcquireSRWLockShared(&sampler_->lock);
        std::vector<std::vector<float>> hist = sampler_->hist;
        std::wstring gw = sampler_->gateway, ad = sampler_->adapter, country = sampler_->country;
        bool tunnel = sampler_->tunnel, gwDown = sampler_->gwDown, httpDown = sampler_->httpDown;
        int gen = sampler_->gen;
        ReleaseSRWLockShared(&sampler_->lock);
        const net::Report* rep = netReport_ ? netReport_.get() : nullptr;
        if (rep && hist.empty()) tunnel = rep->viaTunnel;
        if (country.empty() && rep) country = rep->country;
        d.nodes.push_back({L"Этот ПК", ad.empty() ? L"" : ad, L"ПК"});
        d.nodes.push_back({L"Роутер", gw.empty() ? L"шлюз не найден" : gw, L"Р"});
        if (tunnel) {
            d.tunnel = (int)d.nodes.size();
            d.nodes.push_back({L"Туннель", country.empty() ? L"выход не узнан" : net::CountryName(country),
                               country.empty() ? L"VPN" : country});
        }
        d.nodes.push_back({L"DNS", L"имена сайтов", L"DNS"});
        d.nodes.push_back({L"Интернет", rep && rep->https ? L"сайты открываются" : L"", L"www"});
        // где порвано: шлюз молчит - сразу за ПК; иначе по итогу проверки; иначе HTTPS дважды не прошёл
        int last = (int)d.nodes.size() - 1, dns = last - 1, exitNode = tunnel ? 2 : last;
        int bad = -1;
        if (gwDown)
            bad = 1;
        else if (rep && !busy && !rep->Online())
            bad = !rep->tcp ? exitNode : !rep->dns ? dns : last;
        else if (httpDown)
            bad = exitNode;
        if (bad > 0) d.nodes[bad].ok = false;
        d.hist = hist;
        d.every = 1.5f;
        d.fixing = busy && title.rfind(L"Починить", 0) == 0;
        d.checking = busy && title == L"Проверка сети";
        d.gen = gen * 7 + (int)(netDoneAt_ % 1000);
        if (!busy) RepaintIn(1500);  // новые замеры раз в 1,5 с
    }
    scene::Input in = SceneInput(ID_LIVE_ROUTE, r);
    if (routeScene_.Draw(ui.G(), r, (float)ui.K(), ScenePalette(), in, d)) ui.Tick();
    if (routeScene_.TakeFix()) {
        if (Demo()) {
            g_demoNet.fixAt = DemoT();
        } else if (!(netJob_ && !netJob_->done)) {
            net::Proxy p = net::ReadProxy();
            if (!p.on || !p.alive ||
                MessageBoxW(wnd,
                            (L"Прокси-клиент сейчас работает через " + p.server +
                             L".\nПочинка сбросит системный прокси, и клиент отключится. Продолжить?")
                                .c_str(),
                            L"Починить", MB_YESNO | MB_ICONQUESTION) == IDYES)
                NetRun(L"Починить умно", [](const net::Log& log, NetJob& job) {
                    bool ok = net::SmartFix(log);
                    net::Remember(L"fix", ok ? L"Починить умно: интернет работает." : L"Починить умно: не помогло.");
                    net::Report r = net::Diagnose(false);  // сразу итог: сцена сварит звено или оставит разрыв
                    AcquireSRWLockExclusive(&job.lock);
                    job.report = r;
                    job.hasReport = true;
                    ReleaseSRWLockExclusive(&job.lock);
                    return ok;
                });
        }
    }
}

// ---------------- порты ----------------

bool Studio::Cable(const tools::Listener& l) {
    if (l.udp || tools::ProtectedExe(l.process)) return false;
    if (l.dev || !l.project.empty()) return true;
    std::wstring n = l.process;
    CharLowerBuffW(n.data(), (DWORD)n.size());
    for (const wchar_t* p : {L"xray", L"happ", L"sing-box", L"v2ray", L"clash", L"mihomo"})
        if (n.find(p) != std::wstring::npos) return true;
    return false;
}

void Studio::PortsLive(const RectF& r) {
    std::vector<scene::PortData> rows;
    if (Demo()) {
        rows = {{5173, 101, L"node", L"site-volna", 12, 3},
                {3000, 102, L"node", L"moon-core", 40, 2},
                {8080, 103, L"java", L"unreal-docs", 660, 0},
                {5432, 104, L"postgres", L"", 180, 1},
                {10808, 105, L"Happ", L"туннель", 300, 4}};
    } else {
        static std::map<DWORD, int> minutes;  // сколько работает: раз за обновление списка, не каждый кадр
        static ULONGLONG minutesAt = 0;
        if (minutesAt != toolsAt_) minutes.clear(), minutesAt = toolsAt_;
        for (const tools::Listener& l : toolsPorts_) {
            if (rows.size() >= 8) break;
            if (!Cable(l)) continue;
            auto it = minutes.find(l.pid);
            if (it == minutes.end()) it = minutes.emplace(l.pid, tools::ProcessMinutes(l.pid)).first;
            rows.push_back(
                {l.port, l.pid, NoExe(l.process), l.project.empty() ? L"" : Leaf(l.project), it->second, l.conns});
        }
    }
    scene::Input in = SceneInput(ID_LIVE_PORTS, r);
    if (portsScene_.Draw(ui.G(), r, (float)ui.K(), ScenePalette(), in, rows)) ui.Tick();
    scene::PortData k;
    while (portsScene_.TakeKill(k)) {
        if (Demo()) {
            Toast(L"Проверка: " + k.proc + L" не завершался (выдуманные данные)");
            continue;
        }
        std::wstring exe;
        for (const tools::Listener& l : toolsPorts_)
            if (l.pid == k.pid) exe = l.process;
        if (exe.empty()) continue;             // уже закрылся
        bool ok = tools::KillPid(k.pid, exe);  // номер мог достаться другому - сверка по exe, как в списке
        Toast(ok ? L"Завершён " + exe + L" (порт " + std::to_wstring(k.port) + L")" : L"Не удалось завершить " + exe);
        toolsAt_ = 0;
    }
}

// ---------------- автозагрузка ----------------

void Studio::StartLive(const RectF& r) {
    std::vector<scene::StartData> items;
    float k = (float)ui.K();
    int px = (int)std::lround(48 * k);
    static std::vector<bool> demoOn;
    if (Demo()) {
        struct D {
            const wchar_t *name, *exe;
            float sec;
            int mb;
        } demo[] = {{L"Проводник", L"C:\\Windows\\explorer.exe", 0.6f, 180},
                    {L"Блокнот", L"C:\\Windows\\System32\\notepad.exe", 2.5f, 250},
                    {L"Диспетчер задач", L"C:\\Windows\\System32\\Taskmgr.exe", 8, 420},
                    {L"Терминал", L"C:\\Windows\\System32\\cmd.exe", 12, 380},
                    {L"Paint", L"C:\\Windows\\System32\\mspaint.exe", 4, 300},
                    {L"Редактор реестра", L"C:\\Windows\\regedit.exe", 6, 900},
                    {L"Ножницы", L"C:\\Windows\\System32\\SnippingTool.exe", 3, 120},
                    {L"Калькулятор", L"C:\\Windows\\System32\\calc.exe", 5, 260}};
        if (demoOn.empty()) demoOn.assign(std::size(demo), true);
        for (size_t i = 0; i < std::size(demo); ++i)
            items.push_back({demo[i].name, demo[i].sec, (ULONGLONG)demo[i].mb * 1048576, (bool)demoOn[i], true,
                             ui.IconBitmap(demo[i].exe, px)});
    } else {
        static std::map<std::wstring, std::wstring> exes;  // какой exe у записи: разбор команды - один раз
        for (const tools::StartItem& it : toolsStarts_) {
            auto e = exes.find(it.command);
            if (e == exes.end()) e = exes.emplace(it.command, tools::StartExe(it)).first;
            // честного времени запуска нет: оценка по памяти, которую программа держит после входа
            float mb = (float)(it.memory / 1048576.0);
            float sec = it.memory ? std::clamp(0.4f + mb / 150, 0.4f, 12.0f) : 0.8f;
            items.push_back({it.name, sec, it.memory, it.enabled, it.canToggle,
                             e->second.empty() ? nullptr : ui.IconBitmap(e->second, px)});
        }
    }
    scene::Input in = SceneInput(ID_LIVE_START, r);
    if (startScene_.Draw(ui.G(), r, k, ScenePalette(), in, items, true)) ui.Tick();
    size_t idx;
    bool on;
    while (startScene_.TakeToggle(idx, on)) {
        if (Demo()) {
            if (idx < demoOn.size()) demoOn[idx] = on;
            continue;
        }
        if (idx >= toolsStarts_.size() || (toolsJob_ && !toolsJob_->done)) continue;
        tools::StartItem& it = toolsStarts_[idx];
        std::wstring msg;
        if (tools::SetStartup(it, on, [&](const std::wstring& s) { msg = s; })) it.enabled = on;
        Toast(msg);
    }
}

// ---------------- память ----------------

void Studio::FlaskLive(scene::FlaskScene& s, int id, const RectF& r) {
    scene::MemData d;
    const double GB = 1024.0 * 1024 * 1024;
    if (Demo()) {
        double t = DemoT();
        struct D {
            const wchar_t* name;
            double gb;
            bool leak;
        } demo[] = {{L"UnrealEditor.exe", 3.6, false},
                    {L"browser.exe", 2.4, false},
                    {L"Code.exe", 2.0, true},
                    {L"Figma.exe", 1.0, false},
                    {L"Discord.exe", 0.6, false}};
        d.total = (ULONGLONG)(15.6 * GB);
        d.layers.push_back({L"система", (ULONGLONG)(2.9 * GB), false, true});
        double grow = std::min(t * 0.2, 3.0);  // Code течёт: +0,2 ГБ в секунду, пока не упрётся
        for (const D& x : demo)
            d.layers.push_back({x.name, (ULONGLONG)((x.gb + (x.leak ? grow : 0)) * GB), x.leak, false});
        double used = 12.5 + grow;
        d.swap = (ULONGLONG)(std::max(0.0, used - 15.6 * 0.88) * GB);
    } else {
        LiveData();
        d.total = liveMem_.total;
        ULONGLONG used = liveMem_.total - liveMem_.avail, apps = 0;
        for (const tools::HeavyApp& a : liveApps_) apps += a.bytes;
        d.layers.push_back({L"система", used > apps ? used - apps : 0, false, true});
        for (const tools::HeavyApp& a : liveApps_) {
            bool leak = false;
            for (const tools::ProcHistory::Leak& l : liveWatch_.leaks)
                leak = leak || _wcsicmp(l.name.c_str(), a.name.c_str()) == 0;
            d.layers.push_back({a.name, a.bytes, leak, false});
        }
        // в подкачке - сколько занято в файле подкачки на деле (раз в 10 с: счётчик PDH)
        static ULONGLONG pf = 0, pfAt = 0;
        if (!pfAt || GetTickCount64() - pfAt > 10000) pf = tools::PagefileUsed(), pfAt = GetTickCount64();
        d.swap = pf;
        RepaintIn(3000);  // память меняется - раз в 3 с свежий кадр
    }
    scene::Input in = SceneInput(id, r);
    if (s.Draw(ui.G(), r, (float)ui.K(), ScenePalette(), in, d)) ui.Tick();
    std::wstring name;
    while (s.TakeClose(name)) {
        if (Demo()) {
            Toast(L"Проверка: " + NoExe(name) + L" не закрывался (выдуманные данные)");
            continue;
        }
        for (const tools::HeavyApp& a : liveApps_)
            if (a.name == name && !tools::ProtectedExe(a.name)) toolsui::CloseHeavy(a);  // мягко, ход - в карточке
        liveAt_ = 0;
    }
}

// ---------------- батарея ----------------

void Studio::BatteryLive(const RectF& r) {
    scene::BatData d;
    std::vector<tools::ProcHistory::Hog> hogs;
    if (Demo()) {
        d.present = true, d.ac = false, d.percent = 64, d.minutesLeft = 126;
        d.branches = {{L"wallpaper64.exe", L"обои без окна", 6},
                      {L"browser.exe", L"вкладки в фоне", 3},
                      {L"Discord.exe", L"оверлей", 2},
                      {L"OneDrive.exe", L"синхронизация", 1.2f}};
    } else {
        LiveData();
        d.present = liveBat_.present;
        d.ac = liveBat_.ac;
        d.charging = liveBat_.charging;
        d.percent = liveBat_.percent;
        d.minutesLeft = liveBat_.minutesLeft;
        for (const tools::ProcHistory::Hog& h : liveWatch_.hogs)
            if (h.percent >= 0.5 && hogs.size() < 4 && !tools::ProtectedExe(h.name)) hogs.push_back(h);
        for (const tools::ProcHistory::Hog& h : hogs)
            d.branches.push_back({h.name, L"без окна, " + tools::Bytes(h.mem), (float)h.percent});
        d.empty = liveWatch_.hogsReady ? L"За 10 минут ни одна программа без окна не съела больше 0,5% процессора"
                                       : L"Собираю: нагрузка в фоне считается за 10 минут с запуска RadialMenu";
    }
    scene::Input in = SceneInput(ID_LIVE_BATTERY, r);
    if (batScene_.Draw(ui.G(), r, (float)ui.K(), ScenePalette(), in, d)) ui.Tick();
    std::wstring name;
    while (batScene_.TakeStop(name)) {
        if (Demo()) {
            Toast(L"Проверка: " + NoExe(name) + L" не останавливался (выдуманные данные)");
            continue;
        }
        for (const tools::ProcHistory::Hog& h : hogs)
            if (h.name == name) {
                tools::HeavyApp a;
                a.name = h.name;
                a.pids = h.pids;
                a.bytes = h.mem;
                toolsui::CloseHeavy(a);  // то же, что «2 клика» в секторе «Фон»: мягко, через 5 с - силой
            }
        liveAt_ = 0;
    }
}

// ---------------- версии ----------------

void Studio::VersLive(const RectF& r) {
    std::vector<scene::VerRow> rows;
    if (Demo()) {
        rows = {{L"Python", L"3.10.11", L"3.13.1", {L"3.10", L"3.11", L"3.12", L"3.13"}, L"поддержка до 10.2026"},
                {L"Node.js", L"20.11.0", L"22.12.0", {L"20", L"21", L"22"}, L""},
                {L"CMake", L"3.28.1", L"3.31.2", {L"3.28", L"3.29", L"3.30", L"3.31"}, L""},
                {L"Git", L"2.47.1", L"", {}, L""},
                {L"MSYS2", L"пакеты", L"17 пакетов", {}, L""}};
    } else {
        if (versions_.empty()) versions_ = toolsui::Versions();
        for (const tools::ToolVersion& v : versions_) {
            scene::VerRow row;
            row.name = v.name;
            if (v.name == L"MSYS2") {
                row.from = L"пакеты";
                if (v.newer)
                    row.to =
                        v.latest + L" " +
                        Plural(_wtoi(v.latest.c_str()), L"пакет", L"пакета", L"пакетов").substr(v.latest.size() + 1);
            } else {
                row.from = v.installed.empty() ? L"?" : v.installed;
                if (v.newer && !v.latest.empty()) row.to = v.latest;
                row.train = v.train;
                row.eol = v.eol;
            }
            rows.push_back(row);
        }
    }
    scene::Input in = SceneInput(ID_LIVE_VERS, r);
    if (versScene_.Draw(ui.G(), r, (float)ui.K(), ScenePalette(), in, rows)) ui.Tick();
}

}  // namespace studio
