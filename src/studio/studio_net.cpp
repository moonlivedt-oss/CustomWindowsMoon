// Раздел «Сеть»: состояние, проверка карточками, починка, права, сторож, прокси-процессы, история.
// Всё долгое идёт в своём потоке (NetJob), страница только смотрит на результат.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "app/netui.h"
#include "core/bg.h"
#include "core/fx.h"
#include "core/text.h"
#include "gfx/draw.h"
#include "net/net.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

void NetThread(const std::shared_ptr<NetJob>& job) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    net::Log log = [job](const std::wstring& s) {
        AcquireSRWLockExclusive(&job->lock);
        job->log.push_back(s);
        ReleaseSRWLockExclusive(&job->lock);
        if (job->wnd) InvalidateRect(job->wnd, nullptr, FALSE);
    };
    bool ok = job->work(log, *job);
    AcquireSRWLockExclusive(&job->lock);
    job->ok = ok;
    job->done = true;
    ReleaseSRWLockExclusive(&job->lock);
    if (job->wnd) InvalidateRect(job->wnd, nullptr, FALSE);
    CoUninitialize();
}

Color LevelColor(const UiColors& c, net::Level l) {
    switch (l) {
        case net::OK:
            return Color(255, 120, 205, 160);
        case net::INFO:
            return c.muted;
        case net::WARN:
            return c.gold;
        default:
            return c.warn;
    }
}

}  // namespace

// Запустить работу сети в фоне. Пока идёт одна, вторая не начинается.
void Studio::NetRun(const std::wstring& title, std::function<bool(const net::Log&, NetJob&)> work) {
    if (netJob_ && !netJob_->done) return;
    auto job = std::make_shared<NetJob>();
    job->title = title;
    job->work = std::move(work);
    job->wnd = wnd;
    netJob_ = job;
    if (!bg::Run([job] { NetThread(job); })) job->done = true;
}

void Studio::NetCheck() {
    NetRun(L"Проверка сети", [](const net::Log& log, NetJob& job) {
        log(L"Проверяю прокси, туннели, связь и остатки VPN...");
        net::Report r = net::Diagnose(true);
        AcquireSRWLockExclusive(&job.lock);
        job.report = r;
        job.hasReport = true;
        ReleaseSRWLockExclusive(&job.lock);
        log(net::Summary(r));
        return true;
    });
}

void Studio::NetPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(Column(in, ui.S(900)), L"Сеть",
           L"Проверка, починка и сторож связи. Прокси, туннели, остатки VPN - как NetworkDoctor, только внутри колец.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    CardColumns cols(ui, cardGrow_, view, off, ui.S(900));
    float rowH = cols.RowH();
    auto card = [&](const wchar_t* title, float rows, auto&& body) { cols.Card(title, rows, body); };
    auto row = [&](const RectF& b, float n, const wchar_t* label) { return cols.Row(b, n, label, 0.62f); };
    auto toggle = [&](const RectF& rr, int id, bool& v) {
        bool val = v;
        if (ui.Toggle(id, RectF(rr.X + rr.Width - ui.S(42), rr.Y + ui.S(7), ui.S(42), ui.S(30)), val)) {
            Change();
            v = val;
            Save();
        }
    };
    auto note = [&](const RectF& b, float n, const std::wstring& s, Color col) { cols.Note(b, n, s, col); };

    // первая проверка - сама при открытии раздела
    if (!netReport_ && !netJob_) NetCheck();
    bool busy = netJob_ && !netJob_->done;
    if (netJob_) {
        AcquireSRWLockShared(&netJob_->lock);
        if (netJob_->hasReport) netReport_ = std::make_shared<net::Report>(netJob_->report);
        ReleaseSRWLockShared(&netJob_->lock);
    }
    if (busy) RepaintIn(120);

    // --- состояние ---
    if (netWasBusy_ && !busy) netDoneAt_ = GetTickCount64();
    netWasBusy_ = busy;
    // маршрут - живая сцена: пакеты по звеньям, обрыв и сварка (studio_live.cpp)
    float routeH = scene::RouteScene::Height((float)ui.K()), routeGrow = (routeH - ui.S(64)) / rowH;
    cols.Wide(L"Состояние", 4.6f + routeGrow, [&](const RectF& b) {
        std::wstring head = netReport_ && !busy ? net::Summary(*netReport_) : L"Проверяю";
        Color hc = !netReport_ || busy ? c.muted : netReport_->Online() ? Color(255, 120, 205, 160) : c.warn;
        ui.Text(head, RectF(b.X, b.Y, b.Width, ui.S(30)), hc, 13.5f, true);
        if (!netReport_ || busy)
            ui.BounceDots(b.X + ui.Measure(head, 13.5f, true) + ui.S(6), b.Y + ui.S(17), hc, ui.S(4));
        RouteLive(RectF(b.X, b.Y + ui.S(34), b.Width, routeH), busy);
        float by = b.Y + ui.S(46) + routeH;
        float bw = ui.S(170);
        if (ui.Button(ID_NET_CHECK, RectF(b.X, by, bw, ui.S(36)), busy ? L"Проверяю" : L"Проверить", Kind::Secondary,
                      L"", busy) &&
            !busy)
            NetCheck();
        if (ui.Button(ID_NET_SMART, RectF(b.X + bw + ui.S(10), by, bw, ui.S(36)), L"Починить умно", Kind::Primary) &&
            !busy) {
            net::Proxy p = net::ReadProxy();
            if (!p.on || !p.alive ||
                MessageBoxW(wnd,
                            (L"Прокси-клиент сейчас работает через " + p.server +
                             L".\nПочинка сбросит системный прокси, и клиент отключится. Продолжить?")
                                .c_str(),
                            L"Починить умно", MB_YESNO | MB_ICONQUESTION) == IDYES)
                NetRun(L"Починить умно", [](const net::Log& log, NetJob&) {
                    bool ok = net::SmartFix(log);
                    net::Remember(L"fix", ok ? L"Починить умно: интернет работает." : L"Починить умно: не помогло.");
                    return ok;
                });
        }
        if (netReport_ && netReport_->fix != net::FIX_NONE)
            note(b, 3.2f + routeGrow, L"Совет: " + netReport_->fixLabel + L". " + netReport_->fixReason, c.text);
        else if (netReport_)
            note(b, 3.2f + routeGrow, L"Всё в порядке, чинить нечего.", c.muted);
    });

    // --- скорость и Wi-Fi: живые карточки (studio_net_live.cpp) ---
    SpeedCard(cols);
    WifiCard(cols);

    // --- что нашла проверка ---
    if (netReport_) {
        card(L"Проверка", 1.0f, [&](const RectF& b) {
            float y = b.Y;
            for (const net::Check& ch : netReport_->checks) {
                float d = ui.S(9);
                ui.Fill(RectF(b.X, y + ui.S(7), d, d), LevelColor(c, ch.level), d / 2);
                ui.Text(ch.title, RectF(b.X + ui.S(18), y, b.Width - ui.S(18), ui.S(22)), c.text, 12.5f, true);
                float h = ui.MeasureWrapped(ch.detail, 11.5f, false, b.Width - ui.S(18)) + ui.S(4);
                ui.Text(ch.detail, RectF(b.X + ui.S(18), y + ui.S(21), b.Width - ui.S(18), h), c.muted, 11.5f, false,
                        ui::Left, true);
                y += ui.S(21) + h + ui.S(8);
            }
            note(b, (y - b.Y) / rowH, L"", c.muted);  // карточка вырастает под список
        });
    }

    // --- починка ---
    card(L"Починка", 5.3f, [&](const RectF& b) {
        float bw = (b.Width - ui.S(10)) / 2, bh = ui.S(36);
        struct Btn {
            int id;
            const wchar_t* label;
            const wchar_t* key;
        } btns[] = {{ID_NET_PROXY, L"Сбросить прокси", L"proxy"},
                    {ID_NET_VPN, L"Остатки VPN", L"vpn"},
                    {ID_NET_IP, L"Обновить IP", L"ip"},
                    {ID_NET_ADAPTER, L"Перезапустить адаптер", L"adapter"},
                    {ID_NET_FW_RESTORE, L"Вернуть брандмауэр", L"fw-restore"},
                    {ID_NET_DEEP, L"Глубокий сброс", L"deep"}};
        for (int i = 0; i < 6; ++i) {
            RectF br(b.X + (i % 2) * (bw + ui.S(10)), b.Y + (i / 2) * (bh + ui.S(10)), bw, bh);
            if (!ui.Button(btns[i].id, br, btns[i].label, btns[i].id == ID_NET_DEEP ? Kind::Danger : Kind::Secondary) ||
                busy)
                continue;
            std::wstring key = btns[i].key, title = btns[i].label;
            if (key == L"deep" &&
                MessageBoxW(wnd,
                            L"Сброс Winsock и TCP/IP вернёт сетевой стек Windows к началу. Нужна перезагрузка, "
                            L"VPN-клиенты, возможно, придётся переустановить. Продолжить?",
                            L"Глубокий сброс", MB_YESNO | MB_ICONWARNING) != IDYES)
                continue;
            if (key == L"proxy") {
                net::Proxy p = net::ReadProxy();
                if (p.on && p.alive &&
                    MessageBoxW(wnd, (L"Прокси " + p.server + L" сейчас работает. Сбросить всё равно?").c_str(),
                                L"Сбросить прокси", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    continue;
                NetRun(title, [](const net::Log& log, NetJob&) {
                    net::ResetProxy(log);
                    if (!net::ReadProxy().winhttp.empty()) net::RunAdmin(net::JOB_WINHTTP, log);
                    net::Remember(L"fix", L"Сброс прокси.");
                    return true;
                });
                continue;
            }
            net::Job job = key == L"vpn"          ? net::JOB_VPN
                           : key == L"ip"         ? net::JOB_IP
                           : key == L"adapter"    ? net::JOB_ADAPTER
                           : key == L"fw-restore" ? net::JOB_FW_RESTORE
                                                  : net::JOB_DEEP;
            NetRun(title, [job, title](const net::Log& log, NetJob&) {
                bool ok = net::RunAdmin(job, log);
                net::Remember(L"fix", title + (ok ? L": готово." : L": не получилось."));
                return ok;
            });
        }
        // DNS адаптера
        RectF dr = row(b, 3.0f, L"DNS адаптера");
        int sel = -1;
        if (ui.Segmented(ID_NET_DNS, RectF(dr.X + dr.Width * 0.30f, dr.Y + ui.S(3), dr.Width * 0.70f, ui.S(38)),
                         {L"Cloudflare", L"Google", L"От роутера"}, sel) &&
            sel >= 0 && !busy) {
            net::Job job = sel == 0 ? net::JOB_DNS_CF : sel == 1 ? net::JOB_DNS_GOOGLE : net::JOB_DNS_AUTO;
            NetRun(L"DNS", [job](const net::Log& log, NetJob&) { return net::RunAdmin(job, log); });
        }
        RectF tr = row(b, 4.0f, L"Какой DNS быстрее");
        if (ui.Button(ID_NET_DNS_TEST, RectF(tr.X + tr.Width - ui.S(170), tr.Y + ui.S(4), ui.S(170), ui.S(36)),
                      L"Замерить") &&
            !busy)
            NetRun(L"Замер DNS", [](const net::Log& log, NetJob&) {
                std::vector<net::DnsResult> r = net::TestDns();
                std::wstring route;
                if (net::RouteTo(L"1.1.1.1", &route) && !r.empty() && net::Faked(r.back().ms)) {
                    log(L"Туннель " + route + L" отвечает на DNS сам за 0 мс - замер не покажет разницы.");
                    log(L"Выключи туннель и замерь снова.");
                    return false;
                }
                for (const net::DnsResult& d : r)
                    log(d.name + L" (" + d.ip + L"): " + (d.ms >= 0 ? std::to_wstring(d.ms) + L" мс" : L"не ответил"));
                return true;
            });
        if (netJob_) {
            AcquireSRWLockShared(&netJob_->lock);
            std::wstring s = netJob_->title + (netJob_->done ? (netJob_->ok ? L": готово" : L": не получилось") : L"");
            bool going = !netJob_->done;
            float titleW = ui.Measure(netJob_->title, 11.5f);
            for (size_t i = netJob_->log.size() > 6 ? netJob_->log.size() - 6 : 0; i < netJob_->log.size(); ++i)
                s += L"\n" + netJob_->log[i];
            ReleaseSRWLockShared(&netJob_->lock);
            note(b, 5.0f, s, c.muted);
            if (going) ui.BounceDots(b.X + titleW + ui.S(5), b.Y + 5.0f * rowH + ui.S(10), c.muted, ui.S(3));
        }
    });

    // --- права ---
    card(L"Права администратора", 1.9f, [&](const RectF& b) {
        bool ready = net::AdminReady();
        RectF rr = row(b, 0, ready ? L"Задача MoonRadial\\RadialMenuNet готова" : L"Задачи пока нет");
        if (!ready &&
            ui.Button(ID_NET_ADMIN, RectF(rr.X + rr.Width - ui.S(170), rr.Y + ui.S(4), ui.S(170), ui.S(36)),
                      L"Настроить", Kind::Primary) &&
            !busy)
            NetRun(L"Права", [](const net::Log& log, NetJob&) {
                bool ok = net::SetupAdmin();
                log(ok ? L"Задача создана: дальше без вопросов." : L"Разрешение не дано.");
                return ok;
            });
        note(b, 1,
             L"Остатки VPN, DNS, обновление IP и маршруты требуют прав. Один раз Windows спросит разрешение, дальше "
             L"задача выполняет только эти действия - ничего другого через неё не запустить.",
             c.muted);
    });

    // --- сторож ---
    card(L"Сторож", 5.6f, [&](const RectF& b) {
        toggle(row(b, 0, L"Следить за интернетом"), ID_NET_WATCH, cfg.netWatch);
        toggle(row(b, 1, L"Сообщать о смене выходного узла"), ID_NET_EXIT, cfg.netExit);
        toggle(row(b, 2, L"Проверять сеть при запуске игры"), ID_NET_GAME, cfg.netGame);
        toggle(row(b, 3, L"Замерять скорость раз в сутки"), ID_NET_SPEED_AUTO, cfg.speedAuto);
        RectF ir = row(b, 4, L"Проверять раз в");
        int sel = cfg.netInterval <= 10 ? 0 : cfg.netInterval <= 20 ? 1 : 2;
        if (ui.Segmented(ID_NET_INTERVAL, RectF(ir.X + ir.Width * 0.45f, ir.Y + ui.S(3), ir.Width * 0.55f, ui.S(38)),
                         {L"10 с", L"20 с", L"60 с"}, sel)) {
            Change();
            cfg.netInterval = sel == 0 ? 10 : sel == 1 ? 20 : 60;
            Save();
        }
        note(b, 5,
             L"Пропал интернет - справа снизу карточка с причиной и кнопкой починки, сама сеть не сбрасывается. "
             L"Скорость меряется, пока компьютер простаивает, - так копится сравнение «через туннель / напрямую». "
             L"Кольцо со всеми действиями - на углу (Стол - Углы, действие «Кольцо сети»).",
             c.muted);
    });

    // --- прокси-процессы ---
    ULONGLONG now = GetTickCount64();
    if (now - netProcsAt_ > 3000) {
        netProcs_ = net::ProxyProcesses();
        netProcsAt_ = now;
    }
    card(L"Прокси-процессы", std::max<float>(1, (float)netProcs_.size()), [&](const RectF& b) {
        if (netProcs_.empty()) {
            ui.Text(L"Прокси-клиентов не видно.", RectF(b.X, b.Y, b.Width, ui.S(30)), c.muted, 12);
            return;
        }
        for (size_t i = 0; i < netProcs_.size() && i < 12; ++i) {
            const net::ProxyProc& p = netProcs_[i];
            std::wstring ports;
            for (size_t j = 0; j < p.ports.size() && j < 5; ++j)
                ports += (j ? L", " : L"") + std::to_wstring(p.ports[j]);
            RectF rr = cols.Row(b, (float)i, p.name + L"   " + (ports.empty() ? L"" : L"порты " + ports), 0.75f);
            if (ui.Button(ID_NET_KILL + (int)i, RectF(rr.X + rr.Width - ui.S(130), rr.Y + ui.S(4), ui.S(130), ui.S(36)),
                          L"Завершить", Kind::Ghost)) {
                bool ok = net::Kill(p.pid);
                Toast(ok ? L"Завершён " + p.name : L"Не удалось завершить " + p.name);
                netProcsAt_ = 0;
            }
        }
    });

    // --- кто качает и отрезанные (И15, И16) ---
    netCut_ = netui::CutList();
    ULONGLONG topAt = 0;
    std::vector<netui::TopRow> top = netui::TopRows(&topAt);
    float topRows = 2.0f + (float)top.size() + (netCut_.empty() ? 0 : 0.6f + netCut_.size());
    card(L"Кто качает", topRows, [&](const RectF& b) {
        // подпись своей строкой, кнопки - под ней: в узкой колонке они иначе наезжают на текст
        ui.Text(topAt ? L"Замер " + std::to_wstring((now - topAt) / 60000) + L" мин назад, за 3 секунды"
                      : L"Ещё не замеряли (TCP по программам, нужны права)",
                RectF(b.X, b.Y, b.Width, ui.S(22)), c.muted, 11.5f);
        float by = b.Y + ui.S(28);
        if (ui.Button(ID_NET_TOP, RectF(b.X, by, ui.S(140), ui.S(36)), L"Замерить")) Defer([] { netui::Top(); });
        if (ui.Button(ID_NET_TOP + 50, RectF(b.X + ui.S(150), by, ui.S(140), ui.S(36)), L"За 30 дней", Kind::Ghost))
            Defer([] { ShellExecuteW(nullptr, L"open", L"ms-settings:datausage", nullptr, nullptr, SW_SHOWNORMAL); });
        ui.Tip(ID_NET_TOP + 50, L"Сколько каждая программа потратила за 30 дней - считает сама Windows");
        float share = std::max(0.3f, (b.Width - ui.S(140)) / b.Width);
        for (size_t i = 0; i < top.size(); ++i) {
            const netui::TopRow& t = top[i];
            wchar_t rate[64];
            swprintf(rate, 64, L"  %.1f / %.1f МБ/с", t.down / 1048576.0, t.up / 1048576.0);
            RectF row = cols.Row(b, 1.4f + i, t.name + rate, share);
            ui.Tip(ID_NET_TOP + 1 + (int)i, L"Приём / отдача за 3 секунды замера. " + t.exe);
            bool can = !t.exe.empty() && tools::CutRefuse(t.exe).empty();
            if (can && ui.Button(ID_NET_TOP + 1 + (int)i,
                                 RectF(row.GetRight() - ui.S(130), row.Y + ui.S(4), ui.S(130), ui.S(36)), L"Отрезать",
                                 Kind::Ghost)) {
                std::wstring exe = t.exe;
                Defer([exe] { netui::Cut(exe); });
            }
        }
        float y0 = 1.4f + top.size() + 0.2f;
        if (!netCut_.empty())
            ui.Text(L"Отрезаны от интернета:", RectF(b.X, b.Y + y0 * cols.RowH(), b.Width, ui.S(22)), c.muted, 11.5f);
        for (size_t i = 0; i < netCut_.size(); ++i) {
            const std::wstring& exe = netCut_[i].exe;
            RectF row = cols.Row(b, y0 + 0.5f + i, exe.substr(exe.find_last_of(L'\\') + 1), share);
            ui.Tip(ID_NET_TOP + 20 + (int)i, exe);
            if (ui.Button(ID_NET_TOP + 20 + (int)i,
                          RectF(row.GetRight() - ui.S(130), row.Y + ui.S(4), ui.S(130), ui.S(36)), L"Вернуть")) {
                std::wstring e = exe;
                Defer([e] { netui::Uncut(e); });
            }
        }
    });

    // --- история ---
    if (now - netHistAt_ > 5000) {
        netHist_ = net::History(12);
        netHistAt_ = now;
    }
    card(L"История", 1.0f, [&](const RectF& b) {
        if (netHist_.empty()) {
            note(b, 0, L"Пока пусто: здесь будут обрывы, починки, смены выхода и проверки игр.", c.muted);
            return;
        }
        std::wstring s;
        for (const auto& [t, text] : netHist_) s += (s.empty() ? L"" : L"\n") + t + L"   " + text;
        note(b, 0, s, c.muted);
    });

    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
