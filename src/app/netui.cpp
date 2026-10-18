#include "app/netui.h"

#include <atomic>
#include <memory>

#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "menu/menu.h"
#include "net/net.h"
#include "net/watch.h"

namespace netui {

namespace {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
BoostCard g_card;
net::Watch g_watch;
std::function<void()> g_openStudio;
std::atomic<bool> g_busy{false};
bool g_downShown = false;  // карточка «пропал интернет» была - «вернулся» имеет смысл

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

void ShowRing(RadialMenu& menu) {
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

void Run(const std::wstring& key) {
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
