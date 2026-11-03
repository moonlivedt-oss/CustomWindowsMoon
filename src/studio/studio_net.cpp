// Раздел «Сеть»: состояние, проверка карточками, починка, права, сторож, прокси-процессы, история.
// Всё долгое идёт в своём потоке (NetJob), страница только смотрит на результат.
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>

#include "app/netui.h"
#include "core/bg.h"
#include "core/fx.h"
#include "core/text.h"
#include "gfx/draw.h"
#include "net/net.h"
#include "net/vpnguard.h"
#include "core/elevate.h"
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

// Живой шар связи по сторожу (netui::State): картинка images\net\*.png, пинг крупно, страна выхода или туннель,
// и точки последних проб. Смена состояния - растворением одного шара в другом, «есть связь» - мягкое дыхание.
void Studio::NetOrb(const RectF& r) {
    const UiColors& c = ui.C();
    net::Light l = netui::State();
    if (Demo())  // снимки: RADIAL_NETORB=0..3 - есть / медленно / нет / VPN
        if (const char* d = getenv("RADIAL_NETORB")) {
            static const ULONGLONG demoAt = GetTickCount64() - 9000;  // одна проба, а не новая в каждом кадре
            int k = std::clamp(atoi(d), 0, 3);
            l.known = true, l.online = k != 2, l.tunnel = k == 3, l.pingMs = k == 1 ? 240 : k == 3 ? 61 : 24;
            l.country = L"NL", l.route = k == 3 ? L"happ-tun" : L"", l.probedAt = demoAt;
        }
    int now = !l.known ? -1 : !l.online ? 2 : l.tunnel ? 3 : l.pingMs > 150 ? 1 : 0;
    ULONGLONG t = GetTickCount64();
    if (now != netOrb_) {
        netOrbWas_ = netOrb_;
        netOrb_ = now;
        netOrbAt_ = t;
    }
    if (l.known && l.probedAt && l.probedAt != netProbeAt_) {  // новая проба сторожа - точка в ленту
        netProbeAt_ = l.probedAt;
        netProbes_.push_back({now, l.online ? l.pingMs : -1});
        if (netProbes_.size() > 10) netProbes_.erase(netProbes_.begin());
    }
    static const wchar_t* const kFile[] = {L"online.png", L"slow.png", L"offline.png", L"vpn.png"};
    auto orbColor = [&](int k) {
        return k == 0 ? Color(255, 120, 205, 160) : k == 1 ? c.warn : k == 2 ? c.accent : c.accent2;
    };
    float side = std::min(ui.S(116), r.Height);
    RectF o(r.X + ui.S(8), r.Y + ui.S(8), side, side);  // сверху: свечение не выходит за край карточки
    float mix = fx::Game() ? 1.0f : std::min(1.0f, (float)((t - netOrbAt_) / fx::Dur(450)));
    if (now >= 0) {
        // дыхание и свечение под шаром - эллипс, сам шар не масштабируется (картинка кэшируется по размеру)
        float breath = now == 0 ? 0.5f + 0.5f * std::sin((float)(t % 4000) / 4000.0f * 6.2832f) : 1.0f;
        SolidBrush glow(WithAlpha(orbColor(now), (BYTE)((now == 2 ? 26 : 34 + 30 * breath) * mix)));
        float g = side * (0.04f + 0.03f * breath);
        ui.G().FillEllipse(&glow, o.X - g, o.Y - g, o.Width + 2 * g, o.Height + 2 * g);
    }
    if (netOrbWas_ >= 0 && mix < 1)
        if (Bitmap* b = Img(ExeDir() + L"\\images\\net\\" + kFile[netOrbWas_], 256))
            ui.Picture(b, o, 0, true, (BYTE)(255 * (1 - mix)));
    if (Bitmap* b = Img(ExeDir() + L"\\images\\net\\" + kFile[now < 0 ? 0 : now], 256))
        ui.Picture(b, o, 0, true, (BYTE)((now < 0 ? 90 : 255) * mix));
    if (!fx::Game() && (mix < 1 || now == 0 || now == 2)) RepaintIn(mix < 1 ? 16 : 66);

    float tx = o.X + side + ui.S(16), tw = r.X + r.Width - tx;
    std::wstring big, line, sub, ago;
    Color bigC = c.text;
    switch (now) {
        case -1:
            big = L"…";
            line = L"Сторож ещё не проверял связь";
            break;
        case 2:
            big = L"Нет связи";
            bigC = c.accent;
            line = L"Интернет не отвечает";
            break;
        default:
            big = l.pingMs >= 0 ? std::to_wstring(l.pingMs) + L" мс" : L"есть связь";
            bigC = l.pingMs < 0 || l.pingMs < 80 ? Color(255, 120, 205, 160) : l.pingMs <= 150 ? c.text : c.warn;
            line = now == 3   ? L"через туннель " + l.route
                   : now == 1 ? L"медленно: пинг выше 150 мс"
                              : L"связь в порядке";
            if (!l.country.empty()) sub = L"выход: " + net::CountryName(l.country);
    }
    if (now >= 0 && l.probedAt) {
        ULONGLONG sec = (t - l.probedAt) / 1000;
        ago = L"проверено " + (sec < 60 ? std::to_wstring(sec) + L" с" : std::to_wstring(sec / 60) + L" мин") + L" назад";
    }
    if (sub.empty()) std::swap(sub, ago);  // без страны выхода - время пробы строкой выше
    float ty = o.Y;
    ui.Text(big, RectF(tx, ty, tw, ui.S(34)), bigC, 22, true);
    ui.Text(line, RectF(tx, ty + ui.S(36), tw, ui.S(20)), c.text, 12);
    if (!sub.empty()) ui.Text(sub, RectF(tx, ty + ui.S(56), tw, ui.S(18)), c.muted, 11);
    if (!ago.empty()) ui.Text(ago, RectF(tx, ty + ui.S(74), tw, ui.S(18)), c.faint, 11);
    // последние пробы сторожа: точки по цвету шара, свежая - справа и крупнее
    float dy = ty + ui.S(100), d = ui.S(7), gap = ui.S(5);
    for (size_t i = 0; i < netProbes_.size(); ++i) {
        bool last = i + 1 == netProbes_.size();
        float s = last ? d + ui.S(3) : d;
        SolidBrush br(netProbes_[i].first < 0 ? c.border : orbColor(netProbes_[i].first));
        ui.G().FillEllipse(&br, tx + i * (d + gap) - (s - d) / 2, dy - (s - d) / 2, s, s);
    }
    if (netProbes_.empty())
        ui.Text(L"точки проб появятся здесь", RectF(tx, dy - ui.S(5), tw, ui.S(16)), c.faint, 10.5f);
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
    if (!netReport_ && !netJob_ && !Demo()) NetCheck();  // снимок (RADIAL_DEMO) - без настоящей проверки
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
        // справа под схемой - живой шар связи; совет ниже не залезает под него
        RectF orb(b.X + b.Width - ui.S(330), by - ui.S(6), ui.S(330), b.Y + b.Height - by + ui.S(6));
        NetOrb(orb);
        RectF noteB(b.X, b.Y, b.Width - ui.S(340), b.Height);
        if (netOrb_ == 2) {  // связи нет - «Починить умно» мягко пульсирует, туда и смотреть
            float p = 0.5f + 0.5f * std::sin((float)(GetTickCount64() % 1600) / 1600.0f * 6.2832f);
            RectF fr(b.X + bw + ui.S(10) - ui.S(3), by - ui.S(3), bw + ui.S(6), ui.S(42));
            ui.Stroke(fr, WithAlpha(c.accent, (BYTE)(60 + 140 * p)), ui.S(11), ui.S(2));
        }
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
            note(noteB, 3.2f + routeGrow, L"Совет: " + netReport_->fixLabel + L". " + netReport_->fixReason, c.text);
        else if (netReport_)
            note(noteB, 3.2f + routeGrow, L"Всё в порядке, чинить нечего.", c.muted);
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
    card(L"Сторож", 6.6f, [&](const RectF& b) {
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
        toggle(row(b, 5, L"Следить за безопасностью сети"), ID_NET_SECURITY, cfg.netSecurity);
        note(b, 6,
             L"Пропал интернет - справа снизу карточка с причиной и кнопкой починки, сама сеть не сбрасывается. "
             L"Безопасность: брандмауэр Windows и открытая/устаревшая Wi-Fi, без изменения настроек. "
             L"Автозамер скорости — только на безлимитном подключении. "
             L"Кольцо со всеми действиями - на углу (Стол - Углы, действие «Кольцо сети»).",
             c.muted);
    });

    // --- прокси-процессы ---
    auto guard = netui::State();
    card(L"Защита и фоновые передачи", 3.3f, [&](const RectF& b) {
        toggle(row(b, 0, L"Наблюдать за DNS, прокси и hosts"), ID_NET_CHANGES, cfg.netChanges);
        toggle(row(b, 1, L"Бережные передачи с телефоном"), ID_NET_GENTLE, cfg.transferGentle);
        note(b, 2,
             L"Настройки сравниваются с локальной базой после двух одинаковых наблюдений. "
             L"Бережный режим снижает скорость при росте задержки и откладывает автоматические фото на батарее/лимите.",
             c.muted);
    });
    card(L"Ожидаемый VPN", 8.9f, [&](const RectF& b) {
        auto rr = cols.Row(b, 0, cfg.netVpnRoute.empty() ? L"Туннель ещё не выбран" : cfg.netVpnRoute, 0.55f);
        if (ui.Button(ID_NET_VPN_CURRENT, RectF(rr.GetRight() - ui.S(180), rr.Y + ui.S(4), ui.S(180), ui.S(36)),
                      L"Текущий туннель")) {
            if (!guard.tunnel || guard.route.empty())
                Toast(L"Сейчас нет подтверждённого маршрута через туннель");
            else {
                Change();
                cfg.netVpnRoute = guard.route;
                cfg.netVpnWatch = true;
                cfg.netVpnBlock = false;
                Save();
            }
        }
        toggle(row(b, 1, L"Сообщать о потере выбранного туннеля"), ID_NET_VPN_WATCH, cfg.netVpnWatch);
        toggle(row(b, 2, L"Блокировать выбранные программы"), ID_NET_VPN_BLOCK, cfg.netVpnBlock);
        rr = cols.Row(b, 3, L"Программ: " + std::to_wstring(Split(cfg.netVpnApps, L'|').size()), 0);
        if (ui.Button(ID_NET_VPN_ADD, RectF(rr.X, rr.Y + ui.S(4), rr.Width * .55f, ui.S(36)), L"Добавить программу…"))
            Defer([this] {
                wchar_t file[32768]{};
                OPENFILENAMEW of{sizeof(of)};
                of.hwndOwner = wnd;
                of.lpstrFile = file;
                of.nMaxFile = 32768;
                of.lpstrFilter = L"Программы (*.exe)\0*.exe\0\0";
                of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameW(&of)) {
                    if (Split(cfg.netVpnApps, L'|').size() >= 16) return Toast(L"Можно выбрать до 16 программ");
                    auto why = tools::CutRefuse(file);
                    if (!why.empty()) return Toast(why);
                    Change();
                    cfg.netVpnApps += (cfg.netVpnApps.empty() ? L"" : L"|") + std::wstring(file);
                    cfg.netVpnBlock = false;
                    Save();
                }
            });
        if (ui.Button(ID_NET_VPN_CLEAR, RectF(rr.X + rr.Width * .58f, rr.Y + ui.S(4), rr.Width * .42f, ui.S(36)),
                      L"Очистить список")) {
            Change();
            cfg.netVpnApps.clear();
            cfg.netVpnBlock = false;
            Save();
        }
        rr = row(b, 4, L"");
        if (ui.Button(ID_NET_VPN_SETUP, RectF(rr.X, rr.Y + ui.S(4), rr.Width, ui.S(36)), L"Разрешить список (UAC)")) {
            auto route = cfg.netVpnRoute, apps = cfg.netVpnApps;
            NetRun(L"Разрешение защиты VPN", [route, apps](const net::Log& log, NetJob&) {
                elevate::Put(L"VpnGuardRoute", route);
                elevate::Put(L"VpnGuardApps", apps);
                return net::RunAdmin(net::JOB_VPN_GUARD_SETUP, log);
            });
        }
        rr = row(b, 5, L"");
        if (ui.Button(ID_NET_VPN_OFF, RectF(rr.X, rr.Y + ui.S(4), rr.Width, ui.S(36)),
                      L"Выключить и снять блокировки")) {
            Change();
            cfg.netVpnBlock = false;
            Save();
            NetRun(L"Снятие блокировок VPN",
                   [](const net::Log& log, NetJob&) { return net::RunAdmin(net::JOB_VPN_GUARD_OFF, log); });
        }
        note(b, 6, netui::VpnBlockStatus(), c.muted);
        note(b, 7,
             L"Проверяется маршрут к 1.1.1.1, а не каждый пакет: до обнаружения смены возможен прямой трафик. "
             L"После сбоя приложения правила могут остаться — снимите их кнопкой выше. Чужие правила не удаляются.",
             c.muted);
    });
    auto vpnApps = Split(cfg.netVpnApps, L'|');
    if (!vpnApps.empty())
        card(L"Программы защиты VPN", (float)vpnApps.size(), [&](const RectF& b) {
            for (size_t i = 0; i < vpnApps.size(); ++i) {
                auto app = vpnApps[i];
                auto rr = cols.Row(b, (float)i, app.substr(app.find_last_of(L'\\') + 1), .65f);
                ui.Tip(24600 + (int)i, app);
                if (ui.Button(24600 + (int)i, RectF(rr.GetRight() - ui.S(110), rr.Y + ui.S(4), ui.S(110), ui.S(36)),
                              L"Убрать")) {
                    Change();
                    cfg.netVpnApps.clear();
                    for (size_t k = 0; k < vpnApps.size(); ++k)
                        if (k != i) cfg.netVpnApps += (cfg.netVpnApps.empty() ? L"" : L"|") + vpnApps[k];
                    cfg.netVpnBlock = false;
                    Save();
                }
            }
        });
    card(L"Состояние защиты", 8.5f, [&](const RectF& b) {
        auto age = [](ULONGLONG t) {
            return t ? std::to_wstring((GetTickCount64() - t) / 1000) + L" с назад" : std::wstring(L"ещё не проверено");
        };
        auto status = [&](unsigned bits) {
            if (!cfg.netSecurity) return std::wstring(L"выключено пользователем");
            if ((guard.securityKnown & bits) != bits) return std::wstring(L"не удалось проверить");
            if ((guard.securityObserved & bits) && !(guard.securityRisks & bits))
                return std::wstring(L"перепроверяется");
            return guard.securityRisks & bits ? std::wstring(L"есть риск") : std::wstring(L"проверено");
        };
        cols.Row(b, 0, L"Брандмауэр Windows: " + status(1), .95f);
        cols.Row(b, 1, L"Шифрование Wi-Fi: " + status(6), .95f);
        cols.Row(b, 2, L"Последняя проверка: " + age(guard.securityChecked), .95f);
        const wchar_t* names[] = {L"DNS", L"Прокси", L"hosts"};
        for (unsigned i = 0; i < 3; ++i)
            cols.Row(b, 3 + (float)i,
                     std::wstring(names[i]) + L": " +
                         (!cfg.netChanges                   ? L"выключено пользователем"
                          : guard.settingsKnown & (1u << i) ? L"проверено"
                                                            : L"не удалось проверить"),
                     .95f);
        cols.Row(b, 6, L"Настройки сети: " + age(guard.settingsChecked), .95f);
        cols.Row(b, 7,
                 !cfg.netVpnWatch  ? L"VPN: выключено пользователем"
                 : !guard.vpnKnown ? L"VPN: не удалось проверить"
                 : guard.vpnLost   ? L"VPN: есть риск"
                                   : L"VPN: проверено",
                 .95f);
    });
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
        netHist_ = net::History(40);
        netHistAt_ = now;
    }
    card(L"История", std::max(1.0f, (float)netHist_.size() * 1.1f), [&](const RectF& b) {
        if (netHist_.empty()) {
            note(b, 0, L"Пока пусто: здесь будут обрывы, починки, смены выхода и проверки игр.", c.muted);
            return;
        }
        for (size_t i = 0; i < netHist_.size(); ++i) {
            auto entry = netHist_[i];
            auto rr = cols.Row(b, (float)i * 1.1f, entry.first + L"   " + entry.second, .72f);
            if (ui.Button(24500 + (int)i, RectF(rr.GetRight() - ui.S(110), rr.Y + ui.S(4), ui.S(110), ui.S(36)),
                          L"Подробнее"))
                Defer([entry] { netui::HistoryDetail(entry.first + L"\n" + entry.second); });
        }
    });

    pageContentH_ = cols.Height();
    ui.G().Restore(st);
}

}  // namespace studio
