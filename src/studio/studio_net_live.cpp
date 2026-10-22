// Раздел «Сеть», живые карточки: скорость (история по дням и спидометр замера, Н1) и Wi-Fi (схема каналов, QR, Н2).
// Замер и эфир идут в своём потоке (bg::Run), кадр только смотрит на ход. RADIAL_DEMO=1 - выдуманные данные;
// RADIAL_SPEED=live - спидометр посреди замера, RADIAL_WIFI=off - без местоположения (для снимков).
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "app/netui.h"
#include "core/bg.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

const Color kDirect(255, 120, 205, 160);  // напрямую - зелёный, через туннель - акцент темы

ULONGLONG When(int day, int hour) {
    SYSTEMTIME st{2026, 10, 0, (WORD)day, (WORD)hour, 0, 0, 0};
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

std::vector<net::Speed> DemoSpeeds() {
    std::vector<net::Speed> h;
    for (int d = 0; d < 14; ++d) {
        net::Speed s;
        s.when = When(8 + d, 9);
        s.down = 38 + (d * 7) % 15, s.up = 24 + (d * 3) % 8, s.pingMs = 190 + d, s.tunnel = true;
        s.via = L"happ-xray, Финляндия";
        h.push_back(s);
        if (d % 3 == 1) {
            s.when = When(8 + d, 11);
            s.down = 92 + (d * 5) % 22, s.up = 36 + d % 5, s.pingMs = 18, s.tunnel = false, s.via = L"Wi-Fi";
            h.push_back(s);
        }
    }
    return h;
}

net::WifiInfo DemoWifi() {
    net::WifiInfo w;
    w.ok = true;
    w.adapter = L"Wi-Fi";
    w.now = {L"Bubuka_5G", 78, -56, 44, true, false};
    w.all = {{L"TP-Link_2.4", 62, -62, 1, false, false}, {L"Rostelecom_A1", 48, -70, 6, false, false},
             {L"Keenetic-7", 54, -67, 11, false, false}, {L"MGTS_GPON_5F", 31, -79, 6, false, false},
             {L"Beeline_5G", 36, -76, 36, true, false},  {L"DIR-825", 22, -84, 3, false, true},
             {L"Xiaomi_5G", 24, -82, 149, true, false},  {L"Bubuka_5G-ext", 41, -73, 44, true, false}};
    w.same = {w.all.back()};
    w.advice = L"На 44-м канале ещё одна сеть, но 5 ГГц почти свободен: на роутере можно взять 52 или 100.";
    return w;
}

std::wstring Num(double v) {
    wchar_t b[32];
    swprintf(b, 32, v >= 100 || v == std::floor(v) ? L"%.0f" : L"%.1f", v);
    return b;
}

std::wstring TwoDigits(int v) { return (v < 10 ? L"0" : L"") + std::to_wstring(v); }

// шкала спидометра: логарифм, 1 - 1000 Мбит/с на 270°
float GaugeFrac(double mbps) { return (float)std::clamp(std::log10(1 + mbps) / 3.0, 0.0, 1.0); }

const int kCh5[] = {36,  40,  44,  48,  52,  56,  60,  64,  100, 104, 108, 112, 116,
                    120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
constexpr int kCh5Count = sizeof(kCh5) / sizeof(kCh5[0]);

float Ch5Slot(int ch) {  // номер в ряду каналов 5 ГГц (между соседними - дробью)
    for (int i = 0; i < kCh5Count; ++i)
        if (ch <= kCh5[i])
            return i == 0 || ch == kCh5[i] ? (float)i : i - (kCh5[i] - ch) / (float)(kCh5[i] - kCh5[i - 1]);
    return kCh5Count - 1.0f;
}

}  // namespace

// ---------------- скорость (Н1) ----------------
void Studio::SpeedCard(CardColumns& cols) {
    const UiColors& c = ui.C();
    Graphics& g = ui.G();
    ULONGLONG now = GetTickCount64();
    if (Demo() && !speedJob_ && getenv("RADIAL_SPEED")) {  // спидометр посреди загрузки - для снимка
        speedJob_ = std::make_shared<SpeedJob>();
        speedJob_->live.phase = 2;
        speedJob_->live.mbps = 52.4;
        speedJob_->live.part = 0.55;
        speedNeedle_ = 52.4f;
    }
    bool running = speedJob_ && !speedJob_->done;
    if (speedJob_ && speedJob_->done && !speedJob_->seen) {  // замер записан в историю - перечитать
        speedJob_->seen = true;
        speedHistAt_ = 0;
    }
    if (!speedHistAt_ || now - speedHistAt_ > 30000) {
        speedHist_ = Demo() ? DemoSpeeds() : net::SpeedHistory();
        speedHistAt_ = now;
    }
    const std::vector<net::Speed>& hist = speedHist_;
    std::vector<net::SpeedDay> days = net::SpeedDays(hist, 14);
    net::SpeedMedian mt = net::SpeedMedianOf(hist, true), md = net::SpeedMedianOf(hist, false);

    cols.Wide(L"Скорость", 5.0f, [&](const RectF& b) {
        // --- медианы: через туннель и напрямую ---
        float lx = b.X;
        auto legend = [&](Color col, const std::wstring& name, const net::SpeedMedian& m) {
            ui.Fill(RectF(lx, b.Y + ui.S(12), ui.S(10), ui.S(10)), col, ui.S(3));
            std::wstring t = name + (m.n ? L" ↓ " + Num(m.down) + (m.up > 0 ? L" / ↑ " + Num(m.up) : L"") + L" (" +
                                               Plural(m.n, L"замер", L"замера", L"замеров") + L")"
                                         : std::wstring(L": ещё не мерили"));
            ui.Text(t, RectF(lx + ui.S(16), b.Y + ui.S(4), ui.S(330), ui.S(26)), m.n ? c.text : c.muted, 11.5f);
            lx += ui.S(16) + std::min(ui.S(330), ui.Measure(t, 11.5f)) + ui.S(22);
        };
        legend(c.accent, L"Через туннель", mt);
        legend(kDirect, L"Напрямую", md);
        if (ui.Button(ID_NET_SPEED_RUN, RectF(b.GetRight() - ui.S(170), b.Y, ui.S(170), ui.S(36)),
                      running ? L"Замеряю" : L"Замерить", Kind::Primary, L"", running) &&
            !running && !Demo()) {
            auto job = std::make_shared<SpeedJob>();
            speedJob_ = job;
            HWND w = wnd;
            if (!bg::Run([job, w] {
                    net::Speed s = net::MeasureSpeed([](const std::wstring&) {}, nullptr, &job->live);
                    if (s.down > 0) net::SaveSpeed(s);
                    job->result = s;
                    job->done = true;
                    InvalidateRect(w, nullptr, FALSE);
                }))
                job->done = true;
        }
        ui.Tip(ID_NET_SPEED_RUN, L"Задержка, загрузка и отдача через speed.cloudflare.com, около 20 секунд");

        // --- столбики по дням ---
        RectF ch(b.X, b.Y + ui.S(52), b.Width - ui.S(220), ui.S(170));
        RectF gr(ch.GetRight() + ui.S(20), b.Y + ui.S(44), ui.S(200), ui.S(200));
        if (days.empty()) {
            ui.EmptyState(ID_NET_SPEED_CHART, RectF(ch.X, ch.Y + ui.S(20), ch.Width, ch.Height - ui.S(20)),
                          L"Замеров пока нет: «Замерить» - и здесь появятся столбики по дням");
        } else {
            double maxV = 10;
            for (const net::SpeedDay& d : days) maxV = std::max({maxV, d.down, d.up});
            double step = 5;
            for (double s : {5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 250.0, 500.0, 1000.0})
                if (maxV / s <= 4) {
                    step = s;
                    break;
                }
            double top = std::ceil(maxV * 1.08 / step) * step;
            float px = ch.X + ui.S(36), pw = ch.Width - ui.S(36), base = ch.GetBottom() - ui.S(22),
                  ph = base - ch.Y - ui.S(6);
            for (double v = 0; v <= top + 0.01; v += step) {
                float yy = base - (float)(v / top) * ph;
                ui.Fill(RectF(px, yy, pw, std::max(1.0f, ui.S(1))), WithAlpha(c.border, v == 0 ? 255 : 110));
                ui.Text(Num(v), RectF(ch.X, yy - ui.S(9), ui.S(30), ui.S(18)), c.faint, 9.5f, false, ui::Right);
            }
            // место: запись дня - пара столбиков, между днями - промежуток
            size_t dayCount = 1;
            for (size_t i = 1; i < days.size(); ++i)
                dayCount += days[i].day != days[i - 1].day || days[i].month != days[i - 1].month;
            float slots = days.size() + (dayCount - 1) * 0.5f;
            float slotW = std::min(pw / slots, ui.S(46));
            float x0 = px + (pw - slotW * slots) / 2;
            POINT m = ui.Mouse();
            int hot = -1;
            float x = x0;
            std::vector<float> xs(days.size());
            for (size_t i = 0; i < days.size(); ++i) {
                if (i && (days[i].day != days[i - 1].day || days[i].month != days[i - 1].month)) x += slotW * 0.5f;
                xs[i] = x;
                if (ui.interactive && m.x >= x && m.x < x + slotW && m.y >= ch.Y && m.y < base) hot = (int)i;
                x += slotW;
            }
            for (size_t i = 0; i < days.size(); ++i) {
                const net::SpeedDay& d = days[i];
                Color col = d.tunnel ? c.accent : kDirect;
                if ((int)i == hot) ui.Fill(RectF(xs[i], ch.Y, slotW, base - ch.Y), WithAlpha(c.cardHi, 200), ui.S(4));
                float bw = slotW * 0.36f, hd = (float)(d.down / top) * ph, hu = (float)(d.up / top) * ph;
                ui.Fill(RectF(xs[i] + slotW * 0.12f, base - hd, bw, hd), col, ui.S(3));
                ui.Fill(RectF(xs[i] + slotW * 0.12f + bw + ui.S(2), base - hu, bw * 0.8f, hu), WithAlpha(col, 120),
                        ui.S(3));
                bool first = !i || d.day != days[i - 1].day || d.month != days[i - 1].month;
                if (first) {
                    size_t j = i;
                    while (j + 1 < days.size() && days[j + 1].day == d.day && days[j + 1].month == d.month) ++j;
                    float dw = xs[j] + slotW - xs[i];
                    ui.Text(TwoDigits(d.day) + L"." + TwoDigits(d.month),
                            RectF(xs[i] - ui.S(10), base + ui.S(4), dw + ui.S(20), ui.S(18)), c.faint, 9.5f, false,
                            ui::Center);
                }
            }
            if (hot >= 0) {  // подсказка над столбиками
                const net::SpeedDay& d = days[hot];
                std::wstring t = TwoDigits(d.day) + L"." + TwoDigits(d.month) +
                                 (d.tunnel ? L", через туннель" : L", напрямую") +
                                 (d.n > 1 ? L" (медиана " + std::to_wstring(d.n) + L")" : L"") + L"\n↓ " + Num(d.down) +
                                 L" / ↑ " + Num(d.up) + L" Мбит/с" +
                                 (d.pingMs >= 0 ? L", задержка " + std::to_wstring(d.pingMs) + L" мс" : L"");
                float tw = ui.S(250), th = ui.S(46);
                float tx = std::clamp(xs[hot] + slotW / 2 - tw / 2, ch.X, ch.GetRight() - tw);
                RectF tip(tx, ch.Y - ui.S(4), tw, th);
                ui.Fill(tip, c.field, ui.S(8));
                ui.Stroke(tip, c.border, ui.S(8));
                ui.Text(t, Inset(tip, ui.S(10), ui.S(5)), c.text, 10.5f, false, ui::Left, true);
            }
        }

        // --- спидометр: живой ход замера, в покое - последний замер ---
        int phase = running ? speedJob_->live.phase.load() : 0;
        double target = running                        ? (phase >= 2 ? speedJob_->live.mbps.load() : 0)
                        : speedJob_ && speedJob_->done ? speedJob_->result.down
                        : !hist.empty()                ? hist.back().down
                                                       : 0;
        float dt = speedTickAt_ ? std::min(0.1f, (now - speedTickAt_) / 1000.0f) : 1.0f;
        speedTickAt_ = now;
        speedNeedle_ += (float)(target - speedNeedle_) * std::min(1.0f, dt * 5);
        if (std::fabs(target - speedNeedle_) > 0.05) RepaintIn(33);
        if (running) RepaintIn(60);
        float cx = gr.X + gr.Width / 2, cy = gr.Y + ui.S(86), R = ui.S(72);
        Color col = running ? c.accent : !hist.empty() && !hist.back().tunnel ? kDirect : c.accent;
        Pen track(c.field, ui.S(9)), val(col, ui.S(9));
        track.SetStartCap(LineCapRound), track.SetEndCap(LineCapRound);
        val.SetStartCap(LineCapRound), val.SetEndCap(LineCapRound);
        SmoothingMode sm = g.GetSmoothingMode();
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        RectF arc(cx - R, cy - R, R * 2, R * 2);
        g.DrawArc(&track, arc, 135, 270);
        float f = GaugeFrac(speedNeedle_);
        if (f > 0.003f) g.DrawArc(&val, arc, 135, 270 * f);
        for (double mark : {1.0, 10.0, 100.0, 1000.0}) {  // деления шкалы
            double a = (135 + 270 * GaugeFrac(mark)) * 3.14159265 / 180;
            float tx = cx + (float)std::cos(a) * (R + ui.S(16)), ty = cy + (float)std::sin(a) * (R + ui.S(16));
            ui.Text(Num(mark), RectF(tx - ui.S(20), ty - ui.S(8), ui.S(40), ui.S(16)), c.faint, 9, false, ui::Center);
        }
        double a = (135 + 270 * f) * 3.14159265 / 180;
        Pen needle(c.text, ui.S(2.5f));
        needle.SetEndCap(LineCapRound);
        g.DrawLine(&needle, PointF(cx, cy),
                   PointF(cx + (float)std::cos(a) * (R - ui.S(14)), cy + (float)std::sin(a) * (R - ui.S(14))));
        SolidBrush hub(c.text);
        g.FillEllipse(&hub, cx - ui.S(5), cy - ui.S(5), ui.S(10), ui.S(10));
        g.SetSmoothingMode(sm);
        bool any = running || target > 0;
        ui.Text(any ? Num(speedNeedle_) : L"-", RectF(cx - ui.S(60), cy + ui.S(14), ui.S(120), ui.S(30)), c.text, 18,
                true, ui::Center);
        ui.Text(running && phase == 3 ? L"Мбит/с, отдача" : L"Мбит/с",
                RectF(cx - ui.S(60), cy + ui.S(42), ui.S(120), ui.S(18)), c.muted, 10, false, ui::Center);
        // ход: три шага, текущий наполняется
        RectF steps(gr.X, gr.GetBottom() - ui.S(30), gr.Width, ui.S(26));
        if (running) {
            const wchar_t* names[3] = {L"задержка", L"загрузка", L"отдача"};
            float sw = (steps.Width - ui.S(8)) / 3;
            for (int i = 0; i < 3; ++i) {
                RectF s(steps.X + i * (sw + ui.S(4)), steps.Y, sw, ui.S(6));
                ui.Fill(s, c.field, ui.S(3));
                float part = phase > i + 1    ? 1.0f
                             : phase == i + 1 ? (float)std::min(1.0, speedJob_->live.part.load())
                                              : 0;
                if (phase == 1 && i == 0) part = 0.5f + 0.5f * (float)std::sin(now / 180.0);  // задержка - без доли
                if (part > 0) ui.Fill(RectF(s.X, s.Y, s.Width * part, s.Height), c.accent, ui.S(3));
                ui.Text(names[i], RectF(s.X, s.Y + ui.S(8), s.Width, ui.S(16)), phase == i + 1 ? c.text : c.faint, 9.5f,
                        phase == i + 1, ui::Center);
            }
        } else {
            const net::Speed* last = speedJob_ && speedJob_->done && speedJob_->result.down > 0 ? &speedJob_->result
                                     : !hist.empty()                                            ? &hist.back()
                                                                                                : nullptr;
            std::wstring t =
                speedJob_ && speedJob_->done && speedJob_->result.down <= 0 ? L"не получилось: интернет есть?"
                : last ? (speedJob_ && speedJob_->done ? L"только что" : L"последний замер") + std::wstring(L": ↑ ") +
                             Num(last->up) + (last->pingMs >= 0 ? L", " + std::to_wstring(last->pingMs) + L" мс" : L"")
                       : L"ещё не мерили";
            ui.Text(t, steps, c.muted, 10, false, ui::Center);
        }
    });
}

// ---------------- Wi-Fi (Н2) ----------------
void Studio::WifiCard(CardColumns& cols) {
    const UiColors& c = ui.C();
    Graphics& g = ui.G();
    auto listen = [this] {
        auto job = std::make_shared<WifiJob>();
        wifiJob_ = job;
        if (Demo()) {
            if (const char* w = getenv("RADIAL_WIFI"); w && !strcmp(w, "off")) {
                job->info.locationOff = true;
                job->profile = L"Bubuka_5G";
            } else {
                job->info = DemoWifi();
            }
            job->done = true;
            return;
        }
        HWND w = wnd;
        if (!bg::Run([job, w] {
                job->info = net::Wifi(true);
                if (!job->info.ok) job->profile = net::WifiProfileNow();
                job->done = true;
                InvalidateRect(w, nullptr, FALSE);
            }))
            job->done = true;
    };
    if (!wifiJob_) listen();
    bool done = wifiJob_->done;
    if (!done) RepaintIn(120);
    const net::WifiInfo& w = wifiJob_->info;
    std::wstring ssid = w.ok ? w.now.ssid : wifiJob_->profile;
    bool qrOk = !ssid.empty() && !Demo();

    if (done && !w.ok) {
        cols.Wide(L"Wi-Fi", 1.8f, [&](const RectF& b) {
            std::wstring t =
                w.locationOff
                    ? L"Windows 11 показывает сигнал, канал и соседние сети только при включённом "
                      L"определении местоположения (Параметры - Конфиденциальность - Расположение, и "
                      L"«Разрешить классическим приложениям»). Сейчас оно выключено - схемы каналов не будет." +
                          (ssid.empty() ? std::wstring() : L" Подключены к «" + ssid + L"».")
                    : std::wstring(L"Wi-Fi не подключён (или адаптера нет).");
            float th = ui.MeasureWrapped(t, 11.5f, false, b.Width) + ui.S(4);
            ui.Text(t, RectF(b.X, b.Y, b.Width, th), c.muted, 11.5f, false, ui::Left, true);
            float by = b.Y + th + ui.S(12), bx = b.X;
            if (w.locationOff) {
                if (ui.Button(ID_NET_WIFI_LOC, RectF(bx, by, ui.S(200), ui.S(36)), L"Открыть Параметры", Kind::Primary))
                    Defer([] {
                        ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-location", nullptr, nullptr,
                                      SW_SHOWNORMAL);
                    });
                bx += ui.S(210);
            }
            if (!ssid.empty()) {
                if (ui.Button(ID_NET_WIFI_QR, RectF(bx, by, ui.S(200), ui.S(36)), L"QR для телефона") && qrOk)
                    Defer([] { netui::Run(L"wifi-qr"); });
                ui.Tip(ID_NET_WIFI_QR, L"Картинка, которую телефон считывает камерой и подключается без пароля");
                bx += ui.S(210);
            }
            if (ui.Button(ID_NET_WIFI_RUN, RectF(bx, by, ui.S(170), ui.S(36)), L"Проверить снова", Kind::Ghost))
                Defer(listen);
            cols.Note(b, (by + ui.S(40) - b.Y) / cols.RowH(), L"", c.muted);
        });
        return;
    }

    bool five = w.now.band5, two = !w.ok || !w.now.band5;
    for (const net::WifiNet& n : w.all) (n.band5 ? five : two) = true;
    float bandH = ui.S(118);
    float rows = (ui.S(90) + bandH * (five + two) + ui.S(14) * (five && two)) / cols.RowH();
    cols.Wide(L"Wi-Fi", rows, [&](const RectF& b) {
        // --- что слышно ---
        std::wstring head = !done ? std::wstring(L"Слушаю эфир")
                                  : L"«" + w.now.ssid + L"»: сигнал " + std::to_wstring(w.now.signal) + L"%" +
                                        (w.now.rssi ? L" (" + std::to_wstring(w.now.rssi) + L" дБм)" : L"") +
                                        (w.now.channel ? L", канал " + std::to_wstring(w.now.channel) +
                                                             (w.now.band5 ? L", 5 ГГц" : L", 2,4 ГГц")
                                                       : L"") +
                                        L". Рядом " + Plural((int)w.all.size(), L"сеть", L"сети", L"сетей");
        ui.Text(head, RectF(b.X, b.Y, b.Width - ui.S(390), ui.S(24)), c.text, 12.5f, true);
        if (!done) ui.BounceDots(b.X + ui.Measure(head, 12.5f, true) + ui.S(6), b.Y + ui.S(13), c.muted, ui.S(4));
        ui.Text(done ? (w.advice.empty() ? std::wstring(L"Свой канал почти ни с кем не делим.") : w.advice) : L"",
                RectF(b.X, b.Y + ui.S(26), b.Width - ui.S(390), ui.S(40)), c.muted, 11, false, ui::Left, true);
        if (ui.Button(ID_NET_WIFI_QR, RectF(b.GetRight() - ui.S(380), b.Y, ui.S(200), ui.S(36)), L"QR для телефона",
                      Kind::Primary, L"", !done) &&
            done && qrOk)
            Defer([] { netui::Run(L"wifi-qr"); });
        ui.Tip(ID_NET_WIFI_QR, L"Картинка, которую телефон считывает камерой и подключается без пароля");
        if (ui.Button(ID_NET_WIFI_RUN, RectF(b.GetRight() - ui.S(170), b.Y, ui.S(170), ui.S(36)),
                      done ? L"Послушать снова" : L"Слушаю", Kind::Secondary, L"", !done) &&
            done)
            Defer(listen);

        // --- схема каналов: колокол на сеть, ширина - канал, высота - сигнал ---
        float a = (float)EaseOut(ui.Anim(ID_NET_WIFI_CHART, done, 4));
        float y = b.Y + ui.S(82);
        POINT m = ui.Mouse();
        const net::WifiNet* hotNet = nullptr;
        float hotPeak = 0, hotX = 0;
        auto band = [&](bool is5) {
            RectF p(b.X + ui.S(56), y, b.Width - ui.S(60), bandH - ui.S(22));
            float base = p.GetBottom();
            ui.Text(is5 ? L"5 ГГц" : L"2,4 ГГц", RectF(b.X, y + ui.S(2), ui.S(54), ui.S(18)), c.muted, 10.5f, true);
            ui.Fill(RectF(p.X, base, p.Width, std::max(1.0f, ui.S(1))), c.border);
            // ось: 2,4 ГГц - каналы 1-13 (с полями под края колоколов), 5 ГГц - ряд стандартных каналов
            auto xOf = [&](int ch) {
                return is5 ? p.X + (Ch5Slot(ch) + 1) / (kCh5Count + 1) * p.Width : p.X + (ch + 1.5f) / 17.0f * p.Width;
            };
            float half = is5 ? p.Width / (kCh5Count + 1) * 0.62f : p.Width / 17.0f * 2.0f;  // 20 МГц
            if (is5)
                for (int i = 0; i < kCh5Count; ++i)
                    ui.Text(std::to_wstring(kCh5[i]),
                            RectF(xOf(kCh5[i]) - ui.S(14), base + ui.S(3), ui.S(28), ui.S(16)), c.faint, 8.5f, false,
                            ui::Center);
            else
                for (int ch = 1; ch <= 13; ++ch)
                    ui.Text(std::to_wstring(ch), RectF(xOf(ch) - ui.S(12), base + ui.S(3), ui.S(24), ui.S(16)), c.faint,
                            9, false, ui::Center);
            std::vector<const net::WifiNet*> nets;
            if (w.ok && w.now.band5 == is5 && w.now.channel) nets.push_back(&w.now);
            for (const net::WifiNet& n : w.all)
                if (n.band5 == is5 && n.channel) nets.push_back(&n);
            std::sort(nets.begin(), nets.end(), [&](auto* x, auto* z) { return x->signal < z->signal; });
            SmoothingMode sm = g.GetSmoothingMode();
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            std::vector<RectF> labels;
            for (auto it = nets.rbegin(); it != nets.rend(); ++it) {  // подписи: сильные первыми, без наложений
                const net::WifiNet* n = *it;
                float cx = xOf(n->channel), h = p.Height * 0.9f * n->signal / 100 * a;
                float lw = ui.Measure(n->ssid, 9.5f, n == &w.now) + ui.S(8);
                RectF lr(cx - lw / 2, base - h - ui.S(18), lw, ui.S(16));
                bool free = true;
                for (const RectF& o : labels) free = free && !lr.IntersectsWith(o);
                if (free) labels.push_back(lr);
                if (ui.interactive && m.x >= cx - half && m.x <= cx + half && m.y >= base - h && m.y <= base &&
                    (!hotNet || n->signal > hotNet->signal))
                    hotNet = n, hotPeak = base - h, hotX = cx;
            }
            for (const net::WifiNet* n : nets) {  // слабые снизу, сильные поверх
                bool own = n == &w.now;
                float cx = xOf(n->channel), h = p.Height * 0.9f * n->signal / 100 * a;
                GraphicsPath path;
                std::vector<PointF> pts;
                for (int i = 0; i <= 24; ++i) {
                    float t = i / 12.0f - 1;
                    pts.emplace_back(cx + t * half, base - h * 0.5f * (1 + std::cos(t * 3.14159265f)));
                }
                path.AddCurve(pts.data(), (INT)pts.size());
                path.AddLine(pts.back(), pts.front());
                Color col = own ? c.accent : n == hotNet ? c.text : c.muted;
                SolidBrush fill(WithAlpha(col, own ? 120 : n == hotNet ? 60 : 34));
                g.FillPath(&fill, &path);
                Pen pen(WithAlpha(col, own ? 255 : 170), own ? ui.S(2) : ui.S(1.2f));
                g.DrawCurve(&pen, pts.data(), (INT)pts.size());
            }
            g.SetSmoothingMode(sm);
            for (auto it = nets.rbegin(); it != nets.rend(); ++it) {
                const net::WifiNet* n = *it;
                float cx = xOf(n->channel), h = p.Height * 0.9f * n->signal / 100 * a;
                float lw = ui.Measure(n->ssid, 9.5f, n == &w.now) + ui.S(8);
                RectF lr(cx - lw / 2, base - h - ui.S(18), lw, ui.S(16));
                for (const RectF& o : labels)
                    if (o.X == lr.X && o.Y == lr.Y)
                        ui.Text(n->ssid, lr, n == &w.now ? c.text : c.faint, 9.5f, n == &w.now, ui::Center);
            }
            y += bandH + ui.S(14);
        };
        if (two) band(false);
        if (five) band(true);
        if (hotNet) {
            std::wstring t = hotNet->ssid + (hotNet == &w.now ? L" (ваша)" : L"") + L": сигнал " +
                             std::to_wstring(hotNet->signal) + L"%, канал " + std::to_wstring(hotNet->channel) +
                             (hotNet->open ? L", открытая" : L"");
            float tw = ui.Measure(t, 10.5f) + ui.S(20);
            RectF tip(std::clamp(hotX - tw / 2, b.X, b.GetRight() - tw), hotPeak - ui.S(48), tw, ui.S(26));
            ui.Fill(tip, c.field, ui.S(8));
            ui.Stroke(tip, c.border, ui.S(8));
            ui.Text(t, Inset(tip, ui.S(10), ui.S(4)), c.text, 10.5f);
        }
    });
}

}  // namespace studio
