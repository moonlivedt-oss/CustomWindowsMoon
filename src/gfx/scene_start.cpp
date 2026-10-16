// Дорожка входа (см. scene.h, StartScene): значки автозагрузки висят на гирлянде, тяжёлые прогибают её.
#include <algorithm>
#include <cmath>

#include "gfx/color.h"
#include "gfx/scene.h"

using namespace Gdiplus;

namespace scene {

namespace {

std::wstring Sec(float s) {
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", s);
    for (wchar_t* c = b; *c; ++c)
        if (*c == L'.') *c = L',';
    return b;
}

// значок с прозрачностью и, для выключенного, серый и темнее
void DrawIcon(Graphics& g, Bitmap* bmp, const RectF& r, float alpha, bool grey) {
    if (!bmp) return;
    float a = std::clamp(alpha, 0.0f, 1.0f);
    float s = grey ? 0.55f : 1.0f;
    ColorMatrix m = {};
    if (grey) {
        for (int i = 0; i < 3; ++i) m.m[0][i] = 0.299f * s, m.m[1][i] = 0.587f * s, m.m[2][i] = 0.114f * s;
    } else {
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    }
    m.m[3][3] = a;
    m.m[4][4] = 1;
    ImageAttributes ia;
    ia.SetColorMatrix(&m);
    g.DrawImage(bmp, r, 0, 0, (REAL)bmp->GetWidth(), (REAL)bmp->GetHeight(), UnitPixel, &ia);
}

}  // namespace

bool StartScene::TakeToggle(size_t& index, bool& on) {
    if (toggles_.empty()) return false;
    index = toggles_.front().first;
    on = toggles_.front().second;
    toggles_.erase(toggles_.begin());
    return true;
}

float StartScene::Sag(float x) const {
    float s = 0;
    for (const Bead& b : beads_)
        if (b.on && !b.air) {
            float u = (x - b.x) / (34 * k_);
            s += (float)(b.bytes / 1048576.0) / 40 * k_ * std::exp(-u * u);
        }
    return std::min(s, 46 * k_);
}

// Места на дорожке: по порядку запуска, каждое дальше на своё время; выключенные - в рамке внизу.
// Значок, который налез бы на соседа, висит на нитке подлиннее (ярусы).
void StartScene::Layout() {
    float k = k_, L = 40 * k, R = r_.Width - 40 * k;
    std::vector<Bead*> on, off;
    for (Bead& b : beads_) (b.on ? on : off).push_back(&b);
    std::stable_sort(on.begin(), on.end(), [](Bead* a, Bead* b) { return a->sec < b->sec; });
    float tot = 2;
    for (Bead* b : on) tot += b->sec * 0.9f;
    est_ = tot;
    float span = std::max(10.0f, std::ceil((tot + 3) / 10) * 10);
    float acc = 2;
    for (Bead* b : on) {
        acc += b->sec * 0.9f;
        b->tx = L + (R - L) * std::min(1.0f, acc / span);
    }
    for (size_t i = 0; i < off.size(); ++i) {
        off[i]->tx = L + 26 * k + i * 46 * k;
        off[i]->ty = r_.Height - 48 * k;
    }
    float lv[3] = {-1e9f, -1e9f, -1e9f};
    std::vector<Bead*> byX = on;
    std::sort(byX.begin(), byX.end(), [](Bead* a, Bead* b) { return a->tx < b->tx; });
    for (Bead* b : byX) {
        int t = 0;
        while (t < 2 && b->tx - b->s / 2 < lv[t] + 4 * k) ++t;
        b->tier = (float)t;
        lv[t] = b->tx + b->s / 2;
    }
    for (Bead* b : off) b->tier = 0;
}

bool StartScene::Draw(Graphics& g, const RectF& r, float k, const Palette& p, const Input& in,
                      const std::vector<StartData>& items, bool estimated) {
    bool resized = std::fabs(r.Width - r_.Width) > 1;
    float dt = Step(r, k, in);
    float W = r.Width, H = r.Height, ox = r.X, oy = r.Y;
    float L = 40 * k, R = W - 40 * k, lane = 104 * k;
    // бусины по данным: имя - ключ; включённость берётся из данных (после щелчка данные поправит вызывающий)
    bool relayout = resized;
    for (Bead& b : beads_) b.seen = false;
    for (const StartData& d : items) {
        auto it = std::find_if(beads_.begin(), beads_.end(), [&](const Bead& b) { return b.name == d.name; });
        if (it == beads_.end()) {
            Bead b;
            b.name = d.name;
            b.x = L, b.y = lane;
            b.on = d.on;
            beads_.push_back(b);
            it = beads_.end() - 1;
            relayout = true;
            Wake(3);
        }
        it->seen = true;
        it->sec = d.sec;
        it->bytes = d.bytes;
        it->s = (22 + std::sqrt((float)(d.bytes / 1048576.0)) / 1.7f) * k;
        it->s = std::min(it->s, 44 * k);
        if (it->on != d.on) {  // включили или выключили (щелчком или снаружи): значок спрыгивает
            it->on = d.on;
            it->air = !game_;
            it->vy = (d.on ? -460 : -320) * k;
            relayout = true;
            Wake(2);
        }
    }
    beads_.erase(std::remove_if(beads_.begin(), beads_.end(), [](const Bead& b) { return !b.seen; }), beads_.end());
    if (relayout) Layout();
    if (shown_ < 0 || game_) shown_ = est_;
    float span = std::max(10.0f, std::ceil((est_ + 3) / 10) * 10);

    // где висит значок: под точкой крепления, на нитке длиной по ярусу, с покачиванием
    auto tileAt = [&](const Bead& b) {
        float len = (8 + b.tierShown * 34) * k;
        return PointF(b.x + std::sin(b.sw) * b.hang * (len + b.s / 2) * 0.3f, b.y + b.hang * (len + b.s / 2));
    };
    bool moving = false;
    for (Bead& b : beads_) {
        if (b.on && !b.air) b.ty = lane + Sag(b.tx);
        float x0 = b.x;
        if (game_) {
            b.x = b.tx, b.y = b.ty, b.vx = b.vy = 0, b.air = false, b.sw = b.swv = 0;
        } else if (b.air) {
            b.vy += 1000 * k * dt;
            b.x += (b.tx - b.x) * std::min(1.0f, dt * 3.5f);
            b.y += b.vy * dt;
            if (b.vy > 0 && b.y >= b.ty) b.y = b.ty, b.air = false, b.vy = -b.vy * 0.25f;
        } else {
            Dot d;
            d.x = b.x, d.y = b.y, d.vx = b.vx, d.vy = b.vy, d.tx = b.tx, d.ty = b.ty;
            Spring(d, dt, 80, 11);
            b.x = d.x, b.y = d.y, b.vx = d.vx, b.vy = d.vy;
        }
        // подвеска качается от движения нитки
        b.swv += (-(b.x - x0) / std::max(dt, 0.001f) / k * 0.004f - b.sw * 30 - b.swv * 3) * dt;
        b.sw += b.swv * dt;
        float f = std::min(1.0f, dt * 8);
        b.hang += ((b.on && !b.air ? 1.0f : 0.0f) - b.hang) * (game_ ? 1 : f);
        b.tierShown += (b.tier - b.tierShown) * (game_ ? 1 : f);
        b.pop = std::max(0.0f, b.pop - dt * 1.8f);
        if (b.air || std::fabs(b.x - b.tx) + std::fabs(b.y - b.ty) > 0.3f || std::fabs(b.vx) + std::fabs(b.vy) > 1 ||
            std::fabs(b.sw) + std::fabs(b.swv) > 0.004f || b.pop > 0 || std::fabs(b.tier - b.tierShown) > 0.01f)
            moving = true;
    }
    if (std::fabs(est_ - shown_) > 0.02f) moving = true;
    shown_ += (est_ - shown_) * std::min(1.0f, dt * 5);
    float ex = L + (R - L) * std::min(1.0f, shown_ / span);
    if (replay_ >= 0) {
        replay_ += dt / 2.6f;
        float sx = L + (ex - L) * replay_;
        for (Bead& b : beads_)
            if (b.on) {
                float was = b.lit;
                b.lit = b.x <= sx ? 1.0f : 0.3f;
                if (was < 1 && b.lit == 1) b.pop = 1, b.swv += 4;
            }
        if (replay_ >= 1.25f) {
            replay_ = -1;
            for (Bead& b : beads_) b.lit = 1;
        }
        moving = true;
    }
    // мышь
    hot_ = -1;
    if (over_)
        for (size_t i = 0; i < beads_.size(); ++i) {
            PointF c = tileAt(beads_[i]);
            float hs = (beads_[i].on ? beads_[i].s : 30 * k) / 2 + 3 * k;
            if (std::fabs(mx_ - c.X) < hs && std::fabs(my_ - c.Y) < hs) hot_ = (int)i;
        }
    if (hot_ >= 0 && hot_ != lastHot_ && beads_[hot_].on) beads_[hot_].swv += 2.5f;
    lastHot_ = hot_;
    RectF btn(12 * k, 8 * k, 138 * k, 28 * k);
    bool hotBtn = In(btn);
    if (click_) {
        if (hotBtn && !game_) {
            replay_ = 0;
            for (Bead& b : beads_) b.lit = b.on ? 0.3f : 1;
        } else if (hot_ >= 0) {
            const Bead& b = beads_[hot_];
            size_t idx = 0;
            while (idx < items.size() && items[idx].name != b.name) ++idx;
            if (idx < items.size() && items[idx].canToggle) {
                toggles_.push_back({idx, !b.on});
            } else {
                shake_ = b.name + L": запись для всех, переключается в Диспетчере задач (нужны права)";
                shakeT_ = 3;
            }
        }
    }
    if (shakeT_ > 0) shakeT_ -= dt, moving = true;

    // ---- рисование ----
    GraphicsState gs = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    Color pale = Pale(p);
    {
        std::vector<PointF> lanePts;
        for (float x = L; x <= R + 0.1f; x += 4 * k) lanePts.push_back(PointF(ox + x, oy + lane + Sag(x)));
        Pen lp(WithAlpha(p.accent2, 90), 2.5f * k);
        g.DrawLines(&lp, lanePts.data(), (INT)lanePts.size());
        SolidBrush endB(p.faint);
        g.FillEllipse(&endB, ox + L - 4 * k, oy + lane - 4 * k, 8 * k, 8 * k);
        g.FillEllipse(&endB, ox + R - 4 * k, oy + lane - 4 * k, 8 * k, 8 * k);
        for (int s = 0; s <= 5; ++s) {
            float x = L + (R - L) * s / 5;
            Label(g, std::to_wstring((int)(span * s / 5)) + L" с",
                  RectF(ox + x - 30 * k, oy + lane - 24 * k, 60 * k, 15 * k), p.faint, 10 * k, false, 1);
        }
    }
    if (replay_ >= 0 && replay_ <= 1) {
        float sx = L + (ex - L) * replay_;
        SolidBrush glow(WithAlpha(p.accent2, 26)), line(p.accent2);
        g.FillRectangle(&glow, ox + L, oy + lane - 30 * k, sx - L, 130 * k);
        g.FillRectangle(&line, ox + sx - 1 * k, oy + lane - 30 * k, 2 * k, 130 * k);
    }
    {
        Pen pole(p.accent, 2 * k);
        g.DrawLine(&pole, ox + ex, oy + lane - 44 * k, ox + ex, oy + lane + 6 * k);
        if (replay_ < 0 || replay_ >= 1) {
            float wv = std::sin((float)t_ * 4) * 2 * k;
            PointF fl[3] = {PointF(ox + ex, oy + lane - 44 * k), PointF(ox + ex + 16 * k, oy + lane - 39 * k + wv),
                            PointF(ox + ex, oy + lane - 33 * k)};
            SolidBrush fb(p.accent);
            g.FillPolygon(&fb, fl, 3);
        }
        float tx = std::clamp(ex, 130 * k, W - 130 * k);
        Label(g, L"стол готов через " + std::wstring(estimated ? L"≈" : L"") + Sec(shown_) + L" с",
              RectF(ox + tx - 130 * k, oy + lane - 68 * k, 260 * k, 20 * k), p.text, 13 * k, true, 1);
    }
    float offSec = 0;
    ULONGLONG offB = 0;
    int nOff = 0;
    for (const Bead& b : beads_)
        if (!b.on) offSec += b.sec * 0.9f, offB += b.bytes, ++nOff;
    Label(g,
          nOff ? L"выключено: вход быстрее на " + Sec(offSec) + L" с, свободнее на " +
                     std::to_wstring((int)(offB / 1048576)) + L" МБ"
               : std::wstring(L"выключено при входе"),
          RectF(ox + L, oy + H - 94 * k, W - 2 * L, 16 * k), nOff ? p.accent2 : p.faint, 11 * k);
    {
        Pen dash(WithAlpha(p.faint, 100), 1);
        dash.SetDashStyle(DashStyleDash);
        GraphicsPath path;
        RectF box(ox + L - 8 * k, oy + H - 74 * k, R - L + 16 * k, 52 * k);
        float d = 20 * k;
        path.AddArc(box.X, box.Y, d, d, 180, 90);
        path.AddArc(box.GetRight() - d, box.Y, d, d, 270, 90);
        path.AddArc(box.GetRight() - d, box.GetBottom() - d, d, d, 0, 90);
        path.AddArc(box.X, box.GetBottom() - d, d, d, 90, 90);
        path.CloseFigure();
        g.DrawPath(&dash, &path);
    }
    for (size_t i = 0; i < beads_.size(); ++i) {
        const Bead& b = beads_[i];
        bool on = (int)i == hot_;
        PointF c = tileAt(b);
        float sz = (b.on ? b.s : 30 * k) * (on ? 1.1f : 1);
        if (b.hang > 0.05f) {
            Pen th(WithAlpha(pale, (BYTE)(90 * b.hang)), 1);
            g.DrawLine(&th, ox + b.x, oy + b.y, ox + c.X, oy + c.Y - sz / 2);
            SolidBrush knot(p.faint);
            g.FillEllipse(&knot, ox + b.x - 2.5f * k, oy + b.y - 2.5f * k, 5 * k, 5 * k);
        }
        if (b.pop > 0) {
            float e = (1 - b.pop) * 10 * k;
            StrokeRound(g, RectF(ox + c.X - sz / 2 - e, oy + c.Y - sz / 2 - e, sz + 2 * e, sz + 2 * e), 10 * k,
                        WithAlpha(pale, (BYTE)(255 * b.pop)), 1.5f * k);
        }
        float al = b.on ? b.lit : 1;
        RectF tile(ox + c.X - sz / 2, oy + c.Y - sz / 2, sz, sz);
        FillRound(g, RectF(tile.X + 1 * k, tile.Y + 3 * k, sz, sz), sz * 0.24f, Color((BYTE)(80 * al), 0, 0, 0));
        FillRound(g, tile, sz * 0.24f, WithAlpha(b.on ? MixColor(p.card, p.text, 0.07) : Deep(p), (BYTE)(255 * al)));
        Bitmap* icon = nullptr;
        for (const StartData& d : items)
            if (d.name == b.name) icon = d.icon;
        float pad = sz * 0.14f;
        if (icon)
            DrawIcon(g, icon, RectF(tile.X + pad, tile.Y + pad, sz - 2 * pad, sz - 2 * pad), al * (b.on ? 1 : 0.8f),
                     !b.on);
        else
            Label(g, b.name.substr(0, 1), tile, WithAlpha(b.on ? pale : p.faint, (BYTE)(255 * al)), sz * 0.45f, true,
                  1);
        if (on)
            Label(g, b.name, RectF(ox + c.X - 90 * k, oy + c.Y + sz / 2 + 2 * k, 180 * k, 16 * k), p.text, 11 * k, true,
                  1);
    }
    FillRound(g, RectF(ox + btn.X, oy + btn.Y, btn.Width, btn.Height), 8 * k,
              hotBtn ? MixColor(p.card, p.text, 0.12) : p.card);
    StrokeRound(g, RectF(ox + btn.X, oy + btn.Y, btn.Width, btn.Height), 8 * k, MixColor(p.card, p.text, 0.18), 1);
    Label(g, replay_ >= 0 ? L"вход идёт..." : L"Повторить вход", RectF(ox + btn.X, oy + btn.Y, btn.Width, btn.Height),
          p.text, 12 * k, true, 1);
    std::wstring hint;
    Color hc = p.faint;
    if (shakeT_ > 0) {
        hint = shake_, hc = p.accent;
    } else if (hot_ >= 0) {
        const Bead& b = beads_[hot_];
        hint = b.name + L": +" + Sec(b.sec) + L" с ко входу" + (estimated ? L" (оценка)" : L"") + L", " +
               std::to_wstring((int)(b.bytes / 1048576)) + L" МБ; щелчок - " + (b.on ? L"выключить" : L"включить");
        hc = p.muted;
    } else {
        hint = estimated ? L"время входа - оценка по памяти программ; тяжёлые прогибают дорожку"
                         : L"тяжёлые программы прогибают дорожку";
    }
    Label(g, hint, RectF(ox, oy + H - 18 * k, W - 8 * k, 16 * k), hc, 11 * k, false, 2);
    g.Restore(gs);
    return (moving || Awake()) && !game_;
}

}  // namespace scene
