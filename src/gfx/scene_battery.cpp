// Батарея (см. scene.h, BatteryScene): шина и ветки к фоновым программам, импульсы, рубильники, «сядет в».
#include <algorithm>
#include <cmath>

#include "gfx/color.h"
#include "gfx/scene.h"

using namespace Gdiplus;

namespace scene {

namespace {

constexpr size_t kMaxBranches = 4;

std::wstring Clock(double minutes) {
    int m = ((int)std::lround(minutes) % 1440 + 1440) % 1440;
    wchar_t b[16];
    swprintf(b, 16, L"%d:%02d", m / 60, m % 60);
    return b;
}

std::wstring NoExe(const std::wstring& s) {
    return s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0 ? s.substr(0, s.size() - 4) : s;
}

std::wstring Pct(float v) {
    wchar_t b[16];
    swprintf(b, 16, v >= 10 ? L"%.0f" : L"%.1f", v);
    for (wchar_t* c = b; *c; ++c)
        if (*c == L'.') *c = L',';
    return b;
}

}  // namespace

bool BatteryScene::TakeStop(std::wstring& name) {
    if (stop_.empty()) return false;
    name = stop_.front();
    stop_.erase(stop_.begin());
    return true;
}

bool BatteryScene::Draw(Graphics& g, const RectF& r, float k, const Palette& p, const Input& in, const BatData& d) {
    float dt = Step(r, k, in);
    float W = r.Width, H = r.Height, ox = r.X, oy = r.Y;
    float bx = 30 * k, by = 46 * k, bw = 62 * k, bh = H - 150 * k;
    float busX = bx + bw + 44 * k, cardX = W - 196 * k, swX = (busX + cardX) / 2;
    float outY = by + bh * 0.35f;
    // ветки по данным: имя - ключ
    for (Branch& b : br_) b.seen = false;
    for (size_t i = 0; i < d.branches.size() && i < kMaxBranches; ++i) {
        const BatBranch& bb = d.branches[i];
        auto it = std::find_if(br_.begin(), br_.end(), [&](const Branch& b) { return b.b.name == bb.name; });
        if (it == br_.end()) {
            Branch b;
            b.b = bb;
            br_.push_back(b);
            it = br_.end() - 1;
            Wake(3);
        }
        it->seen = true;
        it->b.cpu = bb.cpu;
        it->b.why = bb.why;
    }
    br_.erase(std::remove_if(br_.begin(), br_.end(), [](const Branch& b) { return !b.seen; }), br_.end());
    for (size_t i = 0; i < br_.size(); ++i) br_[i].y = 44 * k + i * (bh - 20 * k) / 3;
    // разомкнутая ветка, чья программа так и не остановилась, через 8 с замыкается обратно
    for (Branch& b : br_)
        if (!b.on && (b.acc += dt) > 8) b.on = true, b.acc = 0;
    auto drain = [&](bool all) {
        float s = 8;
        for (const Branch& b : br_)
            if (all || b.on) s += std::min(b.b.cpu, 25.0f) * 1.6f;
        return s;
    };
    float level = d.percent >= 0 ? d.percent / 100.0f : 1.0f;
    bool charging = d.present && d.charging;
    // часы до «сядет»: от оценки Windows, с поправкой на разомкнутые ветки
    auto hoursIf = [&](float dr) {
        float base = d.minutesLeft > 0 ? d.minutesLeft / 60.0f : level * 90 / drain(true);
        return base * drain(true) / dr;
    };
    float target = charging ? (1 - level) * 90 / 40 : d.present && !d.ac ? hoursIf(drain(false)) : 0;
    if (shownH_ < 0 || game_) shownH_ = target;
    bool moving = std::fabs(target - shownH_) > 0.005f;
    shownH_ += (target - shownH_) * std::min(1.0f, dt * 4);
    // импульсы: частота - нагрузка; до разомкнутого рубильника не доходят
    bool feeding = Awake();
    for (size_t i = 0; i < br_.size(); ++i) {
        Branch& b = br_[i];
        b.blade += ((b.on ? 0.0f : 1.0f) - b.blade) * (game_ ? 1 : std::min(1.0f, dt * 10));
        if (std::fabs(b.blade - (b.on ? 0 : 1)) > 0.01f) moving = true;
        if (b.on && feeding && !charging) {
            b.acc += dt * std::min(b.b.cpu, 8.0f) * 1.3f + dt * 0.4f;
            if (b.acc > 1) b.acc -= 1, pulses_.push_back({(int)i, 0, 0.55f + std::min(b.b.cpu, 8.0f) * 0.05f});
        }
    }
    if (charging && feeding && Rnd(0, 1) < dt * 10) pulses_.push_back({-1, 0, 0.8f});
    auto path = [&](const Branch& b, PointF* pts) {
        pts[0] = PointF(bx + bw, outY), pts[1] = PointF(busX, outY), pts[2] = PointF(busX, b.y),
        pts[3] = PointF(cardX, b.y);
    };
    auto len = [](const PointF* pts, float* seg) {
        float L = 0;
        for (int q = 1; q < 4; ++q)
            seg[q - 1] = std::hypot(pts[q].X - pts[q - 1].X, pts[q].Y - pts[q - 1].Y), L += seg[q - 1];
        return L;
    };
    for (Pulse& u : pulses_) {
        if (u.br >= 0 && u.br < (int)br_.size() && !br_[u.br].on) {
            PointF pts[4];
            float seg[3];
            path(br_[u.br], pts);
            float L = len(pts, seg), at = (seg[0] + seg[1] + (swX - busX)) / L;
            if (u.u < at && u.u + dt * u.v >= at) u.u = 2;  // упёрся в разомкнутый рубильник
        }
        u.u += dt * u.v;
    }
    pulses_.erase(std::remove_if(pulses_.begin(), pulses_.end(),
                                 [&](const Pulse& u) { return u.u >= 1 || u.br >= (int)br_.size(); }),
                  pulses_.end());
    for (Dot& s : sparks_) s.vy += 500 * k * dt, s.x += s.vx * dt, s.y += s.vy * dt, s.a -= dt;
    sparks_.erase(std::remove_if(sparks_.begin(), sparks_.end(), [](const Dot& s) { return s.a <= 0; }), sparks_.end());
    if (!pulses_.empty() || !sparks_.empty()) moving = true;
    hot_ = -1;
    if (over_)
        for (size_t i = 0; i < br_.size(); ++i)
            if (std::fabs(my_ - br_[i].y) < 20 * k && mx_ > swX - 30 * k) hot_ = (int)i;
    if (click_ && hot_ >= 0 && br_[hot_].on) {
        Branch& b = br_[hot_];
        b.on = false;
        b.acc = 0;
        stop_.push_back(b.b.name);
        for (int q = 0; q < 10; ++q) {
            Dot s;
            s.x = swX + 10 * k, s.y = b.y, s.vx = Rnd(-80, 80) * k, s.vy = Rnd(-160, -20) * k, s.a = Rnd(0.25f, 0.5f);
            sparks_.push_back(s);
        }
        Wake(2);
    }

    // ---- рисование ----
    GraphicsState gs = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Color pale = Pale(p);
    // батарея стоит вертикально, жидкость с волной
    FillRound(g, RectF(ox + bx + bw / 2 - 12 * k, oy + by - 10 * k, 24 * k, 10 * k), 3 * k, WithAlpha(pale, 128));
    RectF body(ox + bx, oy + by, bw, bh);
    StrokeRound(g, body, 12 * k, WithAlpha(pale, 128), 2 * k);
    if (d.present) {
        GraphicsState cs = g.Save();
        GraphicsPath clip;
        RectF inner(body.X + 5 * k, body.Y + 5 * k, bw - 10 * k, bh - 10 * k);
        float dd = 16 * k;
        clip.AddArc(inner.X, inner.Y, dd, dd, 180, 90);
        clip.AddArc(inner.GetRight() - dd, inner.Y, dd, dd, 270, 90);
        clip.AddArc(inner.GetRight() - dd, inner.GetBottom() - dd, dd, dd, 0, 90);
        clip.AddArc(inner.X, inner.GetBottom() - dd, dd, dd, 90, 90);
        clip.CloseFigure();
        g.SetClip(&clip, CombineModeIntersect);
        float top = inner.Y + inner.Height * (1 - level);
        std::vector<PointF> pts{PointF(body.X, body.GetBottom())};
        for (float x = 0; x <= bw + 0.1f; x += 4 * k)
            pts.push_back(PointF(body.X + x, top + std::sin(x / (9 * k) + (float)t_ * 3) * 2.5f * k));
        pts.push_back(PointF(body.GetRight(), body.GetBottom()));
        SolidBrush liq(WithAlpha(charging ? p.accent2 : p.accent, 153));
        g.FillPolygon(&liq, pts.data(), (INT)pts.size());
        g.Restore(cs);
        Label(g, std::to_wstring(d.percent) + L"%",
              RectF(body.X - 10 * k, body.GetBottom() - 32 * k, bw + 20 * k, 22 * k), p.text, 15 * k, true, 1);
    } else {
        Label(g, L"нет", RectF(body.X, body.Y + bh / 2 - 10 * k, bw, 20 * k), p.faint, 12 * k, true, 1);
    }
    if (charging) {
        Pen cab(WithAlpha(p.accent2, 180), 3 * k);
        g.DrawBezier(
            &cab, PointF(body.X + bw / 2, body.GetBottom()), PointF(body.X + bw / 2, body.GetBottom() + 30 * k),
            PointF(body.X - 10 * k, body.GetBottom() + 20 * k), PointF(body.X - 20 * k, body.GetBottom() + 60 * k));
        SolidBrush cp(p.accent2);
        float top = by + 5 * k + (bh - 10 * k) * (1 - level);
        for (const Pulse& u : pulses_)
            if (u.br < 0) {
                float y = by + bh + 40 * k - u.u * 50 * k;
                g.FillRectangle(&cp, ox + bx + bw / 2 - 2 * k, oy + std::max(top, y) - 2 * k, 4 * k, 4 * k);
            }
    }
    // провода: шина и ветки, толщина - нагрузка
    float load = 0;
    for (const Branch& b : br_)
        if (b.on) load += std::min(b.b.cpu, 25.0f);
    if (!br_.empty()) {
        Pen bus(WithAlpha(p.accent2, 128), std::min(8.0f, 1.5f + load * 0.35f) * k);
        g.DrawLine(&bus, ox + bx + bw, oy + outY, ox + busX, oy + outY);
        Pen vbus(WithAlpha(p.accent2, 128), 2 * k);
        float y0 = std::min(outY, br_.front().y), y1 = std::max(outY, br_.back().y);
        g.DrawLine(&vbus, ox + busX, oy + y0, ox + busX, oy + y1);
    }
    float drNow = drain(false);
    for (size_t i = 0; i < br_.size(); ++i) {
        const Branch& b = br_[i];
        bool hl = hot_ == (int)i;
        float w = b.on ? std::clamp(1.5f + b.b.cpu * 0.6f, 1.5f, 7.0f) : 1.5f;
        Pen wire(b.on ? (hl ? p.accent : WithAlpha(p.accent2, 153)) : WithAlpha(p.faint, 153), w * k);
        g.DrawLine(&wire, ox + busX, oy + b.y, ox + swX - 12 * k, oy + b.y);
        g.DrawLine(&wire, ox + swX + 12 * k, oy + b.y, ox + cardX, oy + b.y);
        SolidBrush con(pale);
        g.FillEllipse(&con, ox + swX - 15 * k, oy + b.y - 3 * k, 6 * k, 6 * k);
        g.FillEllipse(&con, ox + swX + 9 * k, oy + b.y - 3 * k, 6 * k, 6 * k);
        float ang = -b.blade * 0.75f;
        Pen blade(b.on ? pale : p.accent, 3 * k);
        blade.SetStartCap(LineCapRound);
        blade.SetEndCap(LineCapRound);
        g.DrawLine(&blade, ox + swX - 12 * k, oy + b.y, ox + swX - 12 * k + 24 * k * std::cos(ang),
                   oy + b.y + 24 * k * std::sin(ang));
        RectF card(ox + cardX, oy + b.y - 19 * k, 184 * k, 38 * k);
        FillRound(g, card, 9 * k, hl ? MixColor(p.card, p.text, 0.08) : MixColor(p.card, p.text, 0.04));
        StrokeRound(g, card, 9 * k, WithAlpha(p.accent2, 50), 1);
        Label(g, NoExe(b.b.name), RectF(card.X + 12 * k, card.Y + 3 * k, 120 * k, 18 * k), b.on ? p.text : p.faint,
              12 * k, true);
        Label(g,
              b.on ? L"процессор " + Pct(b.b.cpu) + L"%" + (b.b.why.empty() ? L"" : L", " + b.b.why)
                   : std::wstring(L"останавливаю"),
              RectF(card.X + 12 * k, card.Y + 20 * k, 166 * k, 16 * k), b.on ? p.muted : p.faint, 10.5f * k);
        if (hl && b.on && d.present && !d.ac) {
            float gain = (hoursIf(drNow - std::min(b.b.cpu, 25.0f) * 1.6f) - hoursIf(drNow)) * 60;
            Label(g, L"+" + std::to_wstring((int)std::lround(gain)) + L" мин",
                  RectF(card.X + 100 * k, card.Y + 3 * k, 76 * k, 18 * k), p.accent2, 10.5f * k, true, 2);
        }
    }
    if (br_.empty())
        Label(g, d.empty, RectF(ox + busX, oy + by + bh / 2 - 10 * k, W - busX - 16 * k, 20 * k), p.muted, 11.5f * k);
    {
        SolidBrush pb(p.accent);
        for (const Pulse& u : pulses_) {
            if (u.br < 0 || u.u > 1) continue;
            PointF pts[4];
            float seg[3];
            path(br_[u.br], pts);
            float L = len(pts, seg), rr = u.u * L;
            PointF at = pts[3];
            for (int q = 0; q < 3; ++q) {
                if (rr <= seg[q]) {
                    float f = seg[q] > 0 ? rr / seg[q] : 0;
                    at = PointF(pts[q].X + (pts[q + 1].X - pts[q].X) * f, pts[q].Y + (pts[q + 1].Y - pts[q].Y) * f);
                    break;
                }
                rr -= seg[q];
            }
            g.FillRectangle(&pb, ox + at.X - 2 * k, oy + at.Y - 2 * k, 4 * k, 4 * k);
        }
        Batch sb;
        for (const Dot& s : sparks_) sb.Add(0, ox + s.x, oy + s.y, 2.5f * k);
        sb.Flush(g, &pale, 1);
    }
    // шкала времени
    {
        float ty = H - 50 * k, tx0 = bx, tx1 = W - 20 * k;
        Pen axis(WithAlpha(p.faint, 153), 1);
        g.DrawLine(&axis, ox + tx0, oy + ty, ox + tx1, oy + ty);
        SYSTEMTIME st;
        GetLocalTime(&st);
        double now = st.wHour * 60 + st.wMinute;
        SolidBrush tick(p.faint);
        for (int h = 0; h <= 6; ++h) {
            float x = tx0 + (tx1 - tx0) * h / 6;
            g.FillRectangle(&tick, ox + x, oy + ty - 3 * k, 1.0f, 6 * k);
            Label(g, Clock(now + h * 60), RectF(ox + x - 30 * k, oy + ty + 6 * k, 60 * k, 15 * k), p.faint, 10 * k,
                  false, 1);
        }
        float mk = tx0 + (tx1 - tx0) * std::clamp(shownH_ / 6, 0.0f, 1.0f);
        SolidBrush mb(charging ? p.accent2 : p.accent);
        g.FillEllipse(&mb, ox + mk - 6 * k, oy + ty - 6 * k, 12 * k, 12 * k);
        std::wstring lab = !d.present ? L"батареи нет: всё от сети"
                           : charging ? L"полная в " + Clock(now + shownH_ * 60)
                           : d.ac     ? L"от сети, заряд держится"
                                      : L"сядет в " + Clock(now + shownH_ * 60);
        float cx = std::clamp(mk, 100 * k, W - 100 * k);
        Label(g, lab, RectF(ox + cx - 100 * k, oy + ty - 26 * k, 200 * k, 18 * k), p.text, 12.5f * k, true, 1);
    }
    Label(g,
          hot_ >= 0 && br_[hot_].on ? L"щелчок - разомкнуть: " + NoExe(br_[hot_].b.name) + L" закроется (мягко)"
                                    : std::wstring(L"щелчок по программе - разомкнуть её провод"),
          RectF(ox, oy + H - 18 * k, W - 8 * k, 16 * k), p.faint, 10.5f * k, false, 2);
    g.Restore(gs);
    return (moving || Awake()) && !game_;
}

}  // namespace scene
