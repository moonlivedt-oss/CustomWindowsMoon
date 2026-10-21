#include "app/netui.h"

#include <algorithm>
#include <atomic>
#include <memory>

#include "core/bg.h"
#include "core/config.h"
#include "core/elevate.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "menu/menu.h"
#include "net/net.h"
#include "net/watch.h"
#include "tools/tools.h"

namespace netui {

namespace {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
BoostCard g_card;
net::Watch g_watch;
std::function<void()> g_openStudio;
std::atomic<bool> g_busy{false};
bool g_downShown = false;  // карточка «пропал интернет» была - «вернулся» имеет смысл
SRWLOCK g_topLock = SRWLOCK_INIT;
std::vector<TopRow> g_top;  // под g_topLock: последний замер «Кто качает»
ULONGLONG g_topAt = 0;
std::wstring g_fgExe;  // программа, что была впереди, когда открыли кольцо (для «Отрезать то, что впереди»)
// отрезанные (И16): чтение правил брандмауэра через COM - до сотни мс, поэтому в фоне и с кэшем на 5 с
SRWLOCK g_cutLock = SRWLOCK_INIT;
std::vector<net::CutApp> g_cut;  // под g_cutLock
std::atomic<ULONGLONG> g_cutAt{0};
std::atomic<bool> g_cutBusy{false};

void StoreCut(std::vector<net::CutApp> v) {
    AcquireSRWLockExclusive(&g_cutLock);
    g_cut = std::move(v);
    ReleaseSRWLockExclusive(&g_cutLock);
    g_cutAt = GetTickCount64();
}

void RefreshCut() {
    if (g_cutBusy.exchange(true)) return;
    if (!bg::Run([] {
            StoreCut(net::CutApps());
            g_cutBusy = false;
        }))
        g_cutBusy = false;
}

std::wstring ExeOf(DWORD pid) {
    HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!h) return L"";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring out = QueryFullProcessImageNameW(h, 0, buf, &n) ? std::wstring(buf, n) : L"";
    CloseHandle(h);
    return out;
}

std::wstring Leaf(const std::wstring& p) { return p.substr(p.find_last_of(L'\\') + 1); }

std::wstring Rate(ULONGLONG b) {
    wchar_t s[32];
    if (b >= 1024 * 1024)
        swprintf(s, 32, L"%.1f МБ/с", b / 1048576.0);
    else
        swprintf(s, 32, L"%d КБ/с", (int)(b / 1024));
    return s;
}

struct JobMsg {
    std::wstring title, text;
    bool done = false, ok = false;
};

ThemeInfo Theme() { return CurrentTheme(*g_cfg); }

Item Act(const wchar_t* name, const wchar_t* icon, const std::wstring& key, const std::wstring& note) {
    Item it;
    it.name = name;
    it.iconFull = ExeDir() + L"\\icons\\" + icon;
    it.special = 3;
    it.path = key;
    it.note = note;
    return it;
}

// Действие в фоне: карточка показывает ход (последнюю строку журнала), потом итог.
void RunJob(const std::wstring& title, std::function<bool(const net::Log&, std::wstring&)> work) {
    if (g_busy) {
        g_card.Show(BoostCard::DONE, L"Подождите", L"Предыдущее действие с сетью ещё идёт.", Theme());
        return;
    }
    g_busy = true;
    g_card.Show(BoostCard::WORKING, title, L"Начинаю...", Theme());
    bool started = bg::Run([title, work = std::move(work)] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring last;
        net::Log log = [&](const std::wstring& s) {
            last = s;
            auto* m = new JobMsg{title, s, false, false};
            if (!PostMessageW(g_app, WM_NET_JOB, 0, (LPARAM)m)) delete m;
        };
        std::wstring result;
        bool ok = work(log, result);
        auto* m = new JobMsg{title, result.empty() ? last : result, true, ok};
        if (!PostMessageW(g_app, WM_NET_JOB, 0, (LPARAM)m)) delete m;
        CoUninitialize();
    });
    if (!started) g_busy = false;
}

// Сброс прокси выключит работающий прокси-клиент: если он жив, сначала спросить.
void Guarded(const std::wstring& key, std::function<void()> go) {
    net::Proxy p = net::ReadProxy();
    if (!p.on || !p.alive) return go();
    g_card.yesLabel = L"Всё равно";
    g_card.noLabel = L"Не надо";
    g_card.onYes = [go] {
        g_card.yesLabel.clear();
        g_card.noLabel.clear();
        go();
    };
    g_card.Show(BoostCard::OFFER, L"Прокси-клиент работает",
                L"Через " + p.server + L" сайты сейчас идут. " +
                    (key == L"smart" ? L"Починка сбросит системный прокси" : L"Сброс выключит его") +
                    L" - клиент отключится.",
                Theme());
}

std::wstring FixKey(net::Fix f) {
    switch (f) {
        case net::FIX_SMART:
            return L"smart";
        case net::FIX_PROXY:
            return L"proxy";
        case net::FIX_VPN:
            return L"vpn";
        case net::FIX_IP:
            return L"ip";
        default:
            return L"";
    }
}

}  // namespace

void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below, std::function<void()> openStudio) {
    g_app = app;
    g_cfg = cfg;
    g_openStudio = std::move(openStudio);
    g_card.below = below;
    g_card.Create(inst);
    g_watch.Configure(cfg->netWatch, cfg->netExit, cfg->netInterval);
    g_watch.Start(app, WM_NET_EVENT);
}

void Apply(const Config& cfg) { g_watch.Configure(cfg.netWatch, cfg.netExit, cfg.netInterval); }

std::wstring Status() {
    net::Light l = g_watch.Now();
    if (!l.known) return L"Проверяю сеть...";
    std::wstring s = l.online ? L"Интернет есть" : L"Нет интернета";
    if (l.online && !l.country.empty()) s += L"   |   " + net::CountryName(l.country);
    if (l.pingMs >= 0) s += L"   |   " + std::to_wstring(l.pingMs) + L" мс";
    if (l.tunnel) s += L"   |   через " + l.route;
    return s;
}

std::vector<Item> RingItems() {
    std::vector<Item> ring;
    ring.push_back(Act(L"Починить умно", L"net-fix.png", L"smart",
                       L"Прокси и DNS, потом WinHTTP и маршруты, потом IP - пока интернет не вернётся"));
    ring.push_back(Act(L"Сбросить прокси", L"net-proxy.png", L"proxy",
                       L"Системный прокси, PAC и WPAD, переменные прокси, кэш DNS"));
    ring.push_back(Act(L"Остатки VPN", L"net-tunnel.png", L"vpn",
                       L"Kill-switch (с бэкапом), зависшие маршруты, MTU, ARP, hosts; нужны права"));
    ring.push_back(Act(L"Обновить IP", L"net-renew.png", L"ip",
                       L"Отпустить и заново получить адрес у роутера на физическом адаптере; нужны права"));
    Item dns = Act(L"DNS", L"net-dns.png", L"", L"Сменить DNS адаптера, очистить кэш, найти самый быстрый");
    dns.children = {
        Act(L"Самый быстрый", L"net-ping.png", L"dns-test", L"Замерить Cloudflare, Google, Quad9, Яндекс..."),
        Act(L"Cloudflare", L"net-dns.png", L"dns-cf", L"1.1.1.1 и 1.0.0.1 на подключённом адаптере"),
        Act(L"Google", L"net-dns.png", L"dns-google", L"8.8.8.8 и 8.8.4.4 на подключённом адаптере"),
        Act(L"От роутера", L"net-router.png", L"dns-auto", L"DNS снова выдаёт роутер (DHCP)")};
    ring.push_back(dns);
    Item ping = Act(L"Пинг", L"net-ping.png", L"", L"Задержка, потери и через какой адаптер идёт маршрут");
    ping.children = {Act(L"Cloudflare", L"net-ping.png", L"ping:1.1.1.1", L"1.1.1.1"),
                     Act(L"Google", L"net-ping.png", L"ping:8.8.8.8", L"8.8.8.8"),
                     Act(L"Яндекс", L"net-ping.png", L"ping:ya.ru", L"ya.ru"),
                     Act(L"GitHub", L"net-ping.png", L"ping:github.com", L"github.com")};
    ring.push_back(ping);
    ring.push_back(Act(L"Проверить сеть", L"net-check.png", L"check",
                       L"Полная проверка в студии: прокси, туннели, остатки VPN, история"));
    return ring;
}

void ShowRing(RadialMenu& menu) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    g_fgExe = ExeOf(pid);
    std::vector<Item> ring = RingItems();
    // кто качает и отрезанные (И15, И16)
    Item top = Act(L"Кто качает", L"net-procs.png", L"", L"Трафик по программам: замер 3 с (нужны права), рядом - «Отрезать»");
    top.children.push_back(Act(L"Замерить", L"net-procs.png", L"top", L"Какие программы сейчас качают и отдают больше всего"));
    for (const TopRow& r : TopRows())
        if (!r.exe.empty() && tools::CutRefuse(r.exe).empty())
            top.children.push_back(Act((L"Отрезать " + Leaf(r.name)).c_str(), L"tools-firewall.png", L"cut:" + r.exe,
                                       L"Сейчас: приём " + Rate(r.down) + L", отдача " + Rate(r.up) +
                                           L". 2 клика - запретить ей выход в интернет (правило брандмауэра)"));
    if (!g_fgExe.empty() && tools::CutRefuse(g_fgExe).empty())
        top.children.push_back(Act((L"Отрезать " + Leaf(g_fgExe)).c_str(), L"tools-firewall.png", L"cut:" + g_fgExe,
                                   L"Программа, что была впереди. 2 клика - запретить ей выход в интернет"));
    for (const net::CutApp& c : CutList())
        top.children.push_back(Act((L"Вернуть " + Leaf(c.exe)).c_str(), L"tools-firewall.png", L"uncut:" + c.exe,
                                   L"Отрезана от интернета. 2 клика - убрать правило"));
    for (Item& c : top.children) {
        std::wstring want = ExeDir() + L"\\icons\\tools-netcut.png";
        if (c.path.rfind(L"cut:", 0) == 0 || c.path.rfind(L"uncut:", 0) == 0)
            if (Exists(want)) c.iconFull = want;
    }
    ring.insert(ring.empty() ? ring.end() : ring.end() - 1, top);  // перед последним («Студия»)
    menu.onNet = [](const std::wstring& key) { Run(key); };
    menu.netStatus = [] { return Status(); };
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = GetDpiForWindow(g_app);
    if (!dpi) dpi = 96;
    menu.ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi);
    g_watch.Kick(0);  // строка состояния освежится к тому, как кольцо раскроется
}

std::vector<net::CutApp> CutList() {
    if (GetTickCount64() - g_cutAt > 5000) RefreshCut();
    AcquireSRWLockShared(&g_cutLock);
    std::vector<net::CutApp> v = g_cut;
    ReleaseSRWLockShared(&g_cutLock);
    return v;
}

std::vector<TopRow> TopRows(ULONGLONG* at) {
    AcquireSRWLockShared(&g_topLock);
    std::vector<TopRow> v = g_top;
    if (at) *at = g_topAt;
    ReleaseSRWLockShared(&g_topLock);
    return v;
}

void Top() { Run(L"top"); }

void Cut(const std::wstring& exe) {
    std::wstring why = tools::CutRefuse(exe);
    if (!why.empty()) {
        g_card.Show(BoostCard::DONE, L"Не отрезаю", why + L".", Theme(), 8000, Toast::G_WARN);
        return;
    }
    g_card.yesLabel = L"Отрезать";
    g_card.noLabel = L"Не надо";
    g_card.onYes = [exe] {
        g_card.yesLabel.clear();
        g_card.noLabel.clear();
        RunJob(L"Отрезаю " + Leaf(exe), [exe](const net::Log& log, std::wstring& res) {
            elevate::Put(L"CutExe", exe);
            bool ok = net::RunAdmin(net::JOB_NET_CUT, log);
            StoreCut(net::CutApps());
            net::Remember(L"fix", L"Отрезана от интернета: " + exe + (ok ? L"" : L" (не получилось)"));
            res = ok ? Leaf(exe) + L" больше не выходит в интернет. Вернуть - в кольце «Сеть» или в студии." : L"";
            return ok;
        });
    };
    g_card.Show(BoostCard::OFFER, L"Отрезать " + Leaf(exe) + L" от интернета?",
                L"Исходящее правило брандмауэра на " + exe +
                    L". Программа продолжит работать, но в сеть не выйдет (обновления, синхронизация, вход тоже).",
                Theme(), 30000);
}

void Uncut(const std::wstring& exe) {
    RunJob(L"Возвращаю " + Leaf(exe), [exe](const net::Log& log, std::wstring& res) {
        elevate::Put(L"CutExe", exe);
        bool ok = net::RunAdmin(net::JOB_NET_UNCUT, log);
        StoreCut(net::CutApps());
        net::Remember(L"fix", L"Снова в сети: " + exe);
        res = ok ? Leaf(exe) + L" снова может выходить в интернет." : L"";
        return ok;
    });
}

void Run(const std::wstring& key) {
    if (key.rfind(L"cut:", 0) == 0) return Cut(key.substr(4));
    if (key.rfind(L"uncut:", 0) == 0) return Uncut(key.substr(6));
    if (key == L"top")
        return RunJob(L"Кто качает", [](const net::Log& log, std::wstring& res) {
            std::vector<TopRow> rows;
            bool ok = net::RunAdmin(net::JOB_NET_TOP, [&](const std::wstring& s) {
                std::vector<std::wstring> f = Split(s, L'\t');
                if (f.size() >= 5 && f[0] == L"@net") {
                    TopRow r;
                    r.name = f[2];
                    r.exe = ExeOf((DWORD)_wtoi(f[1].c_str()));
                    r.down = _wcstoui64(f[3].c_str(), nullptr, 10);
                    r.up = _wcstoui64(f[4].c_str(), nullptr, 10);
                    rows.push_back(r);
                } else {
                    log(s);
                }
            });
            std::sort(rows.begin(), rows.end(),
                      [](const TopRow& a, const TopRow& b) { return a.down + a.up > b.down + b.up; });
            if (rows.size() > 5) rows.resize(5);
            AcquireSRWLockExclusive(&g_topLock);
            g_top = rows;
            g_topAt = GetTickCount64();
            ReleaseSRWLockExclusive(&g_topLock);
            for (const TopRow& r : rows)
                res += (res.empty() ? L"" : L"; ") + r.name + L" " + Rate(r.down) + L" / " + Rate(r.up);
            if (ok && res.empty()) res = L"Сейчас почти ничего не качается (по TCP; QUIC браузеров не видно).";
            else if (ok) res += L". «Отрезать» - в кольце «Сеть», группа «Кто качает».";
            return ok;
        });
    if (key == L"check") {
        if (g_openStudio) g_openStudio();
        return;
    }
    if (key == L"smart")
        return Guarded(key, [] {
            RunJob(L"Чиню сеть", [](const net::Log& log, std::wstring& res) {
                bool ok = net::SmartFix(log);
                net::Remember(L"fix", ok ? L"Починить умно: интернет работает." : L"Починить умно: не помогло.");
                res = ok ? L"Интернет работает." : L"Само не починилось. Подробнее - в студии, раздел «Сеть».";
                return ok;
            });
        });
    if (key == L"proxy")
        return Guarded(key, [] {
            RunJob(L"Сбрасываю прокси", [](const net::Log& log, std::wstring& res) {
                net::ResetProxy(log);
                net::Proxy p = net::ReadProxy();
                if (!p.winhttp.empty()) net::RunAdmin(net::JOB_WINHTTP, log);
                net::Remember(L"fix", L"Сброс прокси.");
                res = net::HttpsOk(6000) ? L"Прокси сброшен, сайты открываются."
                                         : L"Прокси сброшен, но сайты пока не открываются.";
                return true;
            });
        });
    struct Admin {
        const wchar_t* key;
        net::Job job;
        const wchar_t* title;
    } admin[] = {{L"vpn", net::JOB_VPN, L"Убираю остатки VPN"},
                 {L"ip", net::JOB_IP, L"Обновляю IP"},
                 {L"dns-cf", net::JOB_DNS_CF, L"DNS: Cloudflare"},
                 {L"dns-google", net::JOB_DNS_GOOGLE, L"DNS: Google"},
                 {L"dns-auto", net::JOB_DNS_AUTO, L"DNS от роутера"},
                 {L"fw-restore", net::JOB_FW_RESTORE, L"Возвращаю брандмауэр"},
                 {L"adapter", net::JOB_ADAPTER, L"Перезапускаю адаптер"},
                 {L"deep", net::JOB_DEEP, L"Глубокий сброс сети"}};
    for (const Admin& a : admin)
        if (key == a.key) {
            net::Job job = a.job;
            std::wstring title = a.title;
            RunJob(title, [job, title](const net::Log& log, std::wstring& res) {
                bool ok = net::RunAdmin(job, log);
                net::Remember(L"fix", title + (ok ? L": готово." : L": не получилось."));
                return ok;
            });
            return;
        }
    if (key == L"dns-flush")
        return RunJob(L"Очищаю кэш DNS", [](const net::Log& log, std::wstring&) {
            net::FlushDns(log);
            return true;
        });
    if (key == L"dns-test")
        return RunJob(L"Замеряю DNS", [](const net::Log&, std::wstring& res) {
            std::vector<net::DnsResult> r = net::TestDns();
            std::wstring route;
            if (net::RouteTo(L"1.1.1.1", &route) && !r.empty() && net::Faked(r.back().ms)) {
                res = L"Туннель " + route +
                      L" отвечает на DNS сам - замер не покажет разницы. Выключи туннель и замерь снова.";
                return false;
            }
            for (size_t i = 0; i < r.size() && i < 3; ++i)
                if (r[i].ms >= 0)
                    res += (res.empty() ? L"" : L", ") + r[i].name + L" " + std::to_wstring(r[i].ms) + L" мс";
            if (res.empty())
                res = L"Ни один DNS не ответил.";
            else
                res = L"Быстрее всех: " + res + L".";
            return true;
        });
    if (key.rfind(L"kill:", 0) == 0) {
        DWORD pid = (DWORD)_wtoi(key.c_str() + 5);
        bool ok = net::Kill(pid);
        g_card.Show(BoostCard::DONE, ok ? L"Процесс завершён" : L"Не получилось",
                    ok ? L"PID " + std::to_wstring(pid) + L". Если прокси остался включён - «Сбросить прокси»."
                       : L"Процесс не даёт себя завершить (возможно, он запущен с правами).",
                    Theme());
        return;
    }
    if (key.rfind(L"ping:", 0) == 0) {
        std::wstring host = key.substr(5);
        return RunJob(L"Пинг " + host, [host](const net::Log&, std::wstring& res) {
            int loss = 100, ms = net::Ping(host, 4, 1500, &loss);
            std::wstring route;
            bool tunnel = net::RouteTo(host, &route);
            if (tunnel && (ms < 0 || net::Faked(ms))) {
                int real = net::HttpRtt(4000);
                res = L"Туннель " + route + L" отвечает на пинг сам. Настоящая задержка до выхода: " +
                      (real >= 0 ? std::to_wstring(real) + L" мс." : L"не измерилась.");
                return real >= 0;
            }
            if (ms < 0) {
                res = L"Не отвечает на пинг (сервер может молчать нарочно).";
                return false;
            }
            res = std::to_wstring(ms) + L" мс, потерь " + std::to_wstring(loss) + L"%" +
                  (route.empty() ? L"." : (tunnel ? L", через туннель " : L", через ") + route + L".");
            return true;
        });
    }
}

void OnJob(LPARAM lp) {
    std::unique_ptr<JobMsg> m((JobMsg*)lp);
    if (!m->done) {
        g_card.Show(BoostCard::WORKING, m->title + L"...", m->text, Theme());
        return;
    }
    g_busy = false;
    g_card.Show(BoostCard::DONE, m->ok ? m->title + L": готово" : m->title + L": не получилось", m->text, Theme(),
                8000);
}

void OnEvent(LPARAM lp) {
    std::unique_ptr<net::Event> e((net::Event*)lp);
    ThemeInfo t = Theme();
    switch (e->kind) {
        case net::Event::DOWN: {
            if (!g_cfg->netWatch) return;
            g_downShown = true;
            const net::Report& r = e->report;
            std::wstring key = FixKey(r.fix);
            std::wstring why = r.fixReason.empty() ? net::Summary(r) : r.fixReason;
            if (key.empty()) {  // чинить у себя нечего: например, молчит сервер туннеля
                g_card.Show(BoostCard::DONE, L"Пропал интернет", why, t, 15000);
                return;
            }
            g_card.yesLabel = r.fixLabel;
            g_card.noLabel = L"Не надо";
            g_card.onYes = [key] {
                g_card.yesLabel.clear();
                g_card.noLabel.clear();
                Run(key);
            };
            g_card.Show(BoostCard::OFFER, L"Пропал интернет", why, t, 30000);
            return;
        }
        case net::Event::UP:
            if (!g_downShown) return;
            g_downShown = false;
            if (!g_busy) g_card.Show(BoostCard::DONE, L"Интернет вернулся", e->text, t, 5000, Toast::G_OK);
            return;
        case net::Event::EXIT:
            if (g_cfg->netExit && !g_busy)
                g_card.Show(BoostCard::DONE, L"Выход сменился", e->text, t, 7000, Toast::G_NET);
            return;
        case net::Event::GAME:
            if (e->worth && !g_busy) g_card.Show(BoostCard::DONE, L"Сеть в игре", e->text, t, 15000, Toast::G_NET);
            return;
    }
}

void Game(bool on, const std::vector<DWORD>& pids, const std::wstring& name) {
    if (on && g_cfg->netGame) net::CheckGame(pids, name, g_app, WM_NET_EVENT);
}

void Wake() { g_watch.Kick(25000); }

void Tick() { g_card.Tick(); }

void Shutdown() {
    g_watch.Stop();
    g_card.Destroy();
}

}  // namespace netui
