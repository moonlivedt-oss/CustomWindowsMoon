// Колба памяти (см. scene.h, FlaskScene): слои программ, кран утечки, перелив в барабан подкачки.
#include <algorithm>
#include <cmath>

#include "gfx/color.h"
#include "gfx/scene.h"

using namespace Gdiplus;

namespace scene {

namespace {

constexpr float kLim = 0.88f;  // пунктир «предел»: выше Windows начинает сбрасывать в подкачку
constexpr double kGB = 1024.0 * 1024 * 1024;

std::wstring Gb(float v) {
    wchar_t b[32];
    swprintf(b, 32, v >= 10 ? L"%.0f" : L"%.1f", v);
    for (wchar_t* c = b; *c; ++c)
        if (*c == L'.') *c = L',';
    return std::wstring(b) + L" ГБ";
}

std::wstring NoExe(const std::wstring& s) {
    return s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0 ? s.substr(0, s.size() - 4) : s;
}

}  // namespace

bool FlaskScene::TakeClose(std::wstring& name) {
    if (close_.empty()) return false;
    name = close_.front();
    close_.erase(close_.begin());
    return true;
}

bool FlaskScene::Draw(Graphics& g, const RectF& r, float k, const Palette& p, const Input& in, const MemData& d) {
    float dt = Step(r, k, in);
    float W = r.Width, H = r.Height, ox = r.X, oy = r.Y;
    float TOT = std::max(1.0f, (float)(d.total / kGB));
    float fx = 50 * k, fw = 170 * k, fy = 30 * k, fh = H - 74 * k;
    // слои по данным: имя - ключ, высота дотягивается плавно
    for (Layer& l : layers_) l.seen = false;
    for (size_t i = 0; i < d.layers.size(); ++i) {
        const MemLayer& m = d.layers[i];
        auto it = std::find_if(layers_.begin(), layers_.end(), [&](const Layer& l) { return l.name == m.name; });
        if (it == layers_.end()) {
            Layer l;
            l.name = m.name;
            l.system = m.system;
            l.h = layers_.empty() ? -1 : 0;  // самый первый кадр - сразу на месте
            it = layers_.insert(layers_.begin() + std::min(i, layers_.size()), l);
            Wake(2);
        }
        it->seen = true;
        it->leak = m.leak;
        if (it->boil < 0) it->gb = (float)(m.bytes / kGB);
    }
    for (Layer& l : layers_)
        if (!l.seen) l.gb = 0;
    layers_.erase(std::remove_if(layers_.begin(), layers_.end(), [](const Layer& l) { return !l.seen && l.h < 0.01f; }),
                  layers_.end());
    {
        BYTE c = 1;
        for (Layer& l : layers_) l.col = l.system ? 0 : (BYTE)(1 + (c++ - 1) % 6);
    }
    bool moving = false;
    float used = 0;
    for (Layer& l : layers_) {
        if (l.h < 0 || game_) l.h = l.gb;
        if (l.leak && l.boil < 0 && Awake() && Rnd(0, 1) < dt * 3) {
            Dot q;
            q.y = fy - 4 * k;
            drops_.push_back(q);
        }
        if (l.boil >= 0) {
            l.boil += dt;
            if (Rnd(0, 1) < 0.6f && l.boil < 1.5f) {
                Dot b;
                b.x = fx + Rnd(10, fw / k - 10) * k, b.y = l.top > 0 ? l.top : fy + fh / 2;
                b.r = Rnd(2, 5) * k, b.vy = Rnd(50, 100) * k, b.a = 1;
                bubbles_.push_back(b);
            }
            if (l.boil > 0.45f) l.gb = 0;
            if (l.boil > 6) l.boil = -1;  // программа не закрылась - слой вернётся по данным
            moving = moving || l.boil < 2;
        }
        float to = l.gb;
        if (std::fabs(to - l.h) > 0.005f) moving = true;
        l.h += (to - l.h) * std::min(1.0f, dt * (l.boil >= 0 ? 2.5f : 6));
        used += l.h;
    }
    // колба вбок: жидкость плещется на пружине
    if (pressed_ && mx_ > fx && mx_ < fx + fw && my_ > fy && my_ < fy + fh) drag_ = true, dragX_ = mx_;
    if (drag_ && down_) {
        tv_ += (mx_ - dragX_) / k * 0.02f;
        dragX_ = mx_;
    }
    if (released_) drag_ = false;
    tv_ += (-tilt_ * 60 - tv_ * 4) * dt;
    tilt_ += tv_ * dt;
    if (game_) tilt_ = tv_ = 0;
    if (std::fabs(tilt_) + std::fabs(tv_) > 0.002f) moving = true;
    float cap = std::min(used, TOT * kLim);
    float surf = fy + fh - cap / TOT * fh;
    for (Dot& q : drops_) q.vy += 600 * k * dt, q.y += q.vy * dt;
    drops_.erase(std::remove_if(drops_.begin(), drops_.end(), [&](const Dot& q) { return q.y >= surf; }), drops_.end());
    if (Awake() && Rnd(0, 1) < dt * 7) {
        Dot b;
        b.x = fx + Rnd(10, fw / k - 10) * k, b.y = fy + fh, b.r = Rnd(1.5f, 3) * k, b.vy = Rnd(20, 40) * k, b.a = 1;
        bubbles_.push_back(b);
    }
    for (Dot& b : bubbles_) b.y -= b.vy * dt, b.a -= dt * 0.6f;
    bubbles_.erase(std::remove_if(bubbles_.begin(), bubbles_.end(), [](const Dot& b) { return b.a <= 0; }),
                   bubbles_.end());
    if (!drops_.empty() || !bubbles_.empty()) moving = true;
    float swapGb = (float)(d.swap / kGB);
    if (pf_ < 0 || game_) pf_ = swapGb;
    if (std::fabs(swapGb - pf_) > 0.005f) moving = true;
    pf_ += (swapGb - pf_) * std::min(1.0f, dt * 3);
    bool ov = used >= TOT * kLim - 0.01f;  // упёрлись в предел - льётся через носик
    // слои: границы снизу вверх
    {
        float y = fy + fh, acc = 0;
        for (Layer& l : layers_) {
            float take = std::max(0.0f, std::min(l.h, cap - acc));
            acc += take;
            l.hh = take / TOT * fh;
            l.bot = y;
            l.top = y - l.hh;
            y -= l.hh;
        }
    }
    hot_ = -1;
    if (over_ && !drag_ && mx_ > fx && mx_ < fx + fw)
        for (size_t i = 0; i < layers_.size(); ++i)
            if (!layers_[i].system && layers_[i].boil < 0 && my_ <= layers_[i].bot && my_ > layers_[i].top)
                hot_ = (int)i;
    if (click_ && hot_ >= 0) {
        layers_[hot_].boil = 0;
        close_.push_back(layers_[hot_].name);
        Wake(2);
    }

    // ---- рисование ----
    Color pale = Pale(p), deep = Deep(p);
    Color cols[7] = {p.faint,
                     p.accent,
                     pale,
                     p.accent2,
                     MixColor(p.accent, pale, 0.45),
                     MixColor(p.accent, p.accent2, 0.5),
                     MixColor(p.accent2, p.text, 0.35)};
    GraphicsState gs = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    RectF flask(ox + fx, oy + fy, fw, fh);
    {
        GraphicsState cs = g.Save();
        GraphicsPath clip;
        {
            float rr = 18 * k, dd = rr * 2;
            clip.AddArc(flask.X, flask.Y, dd, dd, 180, 90);
            clip.AddArc(flask.GetRight() - dd, flask.Y, dd, dd, 270, 90);
            clip.AddArc(flask.GetRight() - dd, flask.GetBottom() - dd, dd, dd, 0, 90);
            clip.AddArc(flask.X, flask.GetBottom() - dd, dd, dd, 90, 90);
            clip.CloseFigure();
        }
        g.SetClip(&clip, CombineModeIntersect);
        SolidBrush back(deep);
        g.FillRectangle(&back, flask);
        float slope = std::tan(std::clamp(tilt_, -0.35f, 0.35f));
        // заливка сверху вниз: каждый слой льётся до самого дна, верхний поверх - щелей между волнами нет
        for (int i = (int)layers_.size() - 1; i >= 0; --i) {
            const Layer& l = layers_[i];
            if (l.hh < 0.3f) continue;
            bool on = hot_ == i;
            std::vector<PointF> pts;
            pts.push_back(PointF(ox + fx, oy + fy + fh + 4 * k));
            for (float x = 0; x <= fw + 0.1f; x += 6 * k)
                pts.push_back(PointF(ox + fx + x, oy + l.top + slope * (x - fw / 2) +
                                                      std::sin(x / (16 * k) + (float)t_ * 2.5f + i) * 1.8f * k));
            pts.push_back(PointF(ox + fx + fw, oy + fy + fh + 4 * k));
            SolidBrush lb(MixColor(cols[l.col], deep, on ? 0 : l.leak ? 0.1 : 0.22));
            g.FillPolygon(&lb, pts.data(), (INT)pts.size());
        }
        for (const Dot& b : bubbles_) {
            Pen bp(WithAlpha(pale, (BYTE)(150 * std::clamp(b.a, 0.0f, 1.0f))), 1);
            g.DrawEllipse(&bp, ox + b.x - b.r, oy + b.y - b.r, b.r * 2, b.r * 2);
        }
        g.Restore(cs);
    }
    StrokeRound(g, flask, 18 * k, WithAlpha(p.accent2, 115), 2 * k);
    float ly = fy + fh * (1 - kLim);
    {
        Pen lim(WithAlpha(p.accent, 150), 1);
        lim.SetDashStyle(DashStyleDash);
        g.DrawLine(&lim, ox + fx - 6 * k, oy + ly, ox + fx + fw + 6 * k, oy + ly);
        Label(g, L"предел", RectF(ox, oy + ly - 8 * k, fx - 8 * k, 16 * k), p.accent, 10 * k, false, 2);
    }
    // кран над колбой и капли утечки
    const Layer* leak = nullptr;
    for (const Layer& l : layers_)
        if (l.leak && l.gb > 0) leak = &l;
    {
        SolidBrush tap(MixColor(p.card, p.text, 0.1));
        FillRound(g, RectF(ox + fx + fw / 2 - 16 * k, oy + fy - 16 * k, 32 * k, 10 * k), 3 * k,
                  MixColor(p.card, p.text, 0.1));
        g.FillRectangle(&tap, ox + fx + fw / 2 - 3 * k, oy + fy - 8 * k, 6 * k, 8 * k);
        if (leak) {
            SolidBrush db(cols[leak->col]);
            for (const Dot& q : drops_)
                g.FillEllipse(&db, ox + fx + fw / 2 - 2.5f * k, oy + q.y - 2.5f * k, 5 * k, 5 * k);
            Label(g, NoExe(leak->name) + L" растёт",
                  RectF(ox + fx + fw / 2 + 22 * k, oy + fy - 19 * k, 200 * k, 16 * k), p.accent, 10.5f * k, true);
        }
    }
    // подкачка - барабан диска: то, что выше предела, переливается струйкой и ложится тёмным медленным слоем
    float px = fx + fw + 16 * k, pw = 44 * k, ph = 84 * k, py = fy + fh - ph, ry = 6 * k;
    const Layer* top = nullptr;
    for (const Layer& l : layers_)
        if (l.hh > 0.3f) top = &l;
    Color topCol = top ? cols[top->col] : p.faint;
    float drumCap = std::max(2.0f, TOT * 0.25f);
    float lvl = std::min(ph - 10 * k, pf_ / drumCap * ph), sy = py + ph - lvl;
    {
        GraphicsPath body;
        body.AddArc(ox + px, oy + py + ph - ry, pw, ry * 2, 0, 180);
        body.AddLine(ox + px, oy + py + ph, ox + px, oy + py);
        body.AddArc(ox + px, oy + py - ry, pw, ry * 2, 180, 180);
        body.CloseFigure();
        SolidBrush bb(deep);
        g.FillPath(&bb, &body);
        if (lvl > 1) {
            GraphicsState cs = g.Save();
            g.SetClip(&body, CombineModeIntersect);
            SolidBrush liq(MixColor(topCol, deep, 0.45));
            g.FillRectangle(&liq, ox + px, oy + sy, pw, lvl + ry * 2);
            g.Restore(cs);
            SolidBrush sf(MixColor(topCol, deep, 0.25));
            float wob = std::sin((float)t_ * 5) * 0.6f * k;
            g.FillEllipse(&sf, ox + px + 1, oy + sy - (ry - 1 - wob), pw - 2, (ry - 1 - wob) * 2);
            if (ov)
                for (int q = 0; q < 2; ++q) {
                    float ph_ = (float)std::fmod(t_ * 1.2 + q * 0.5, 1.0), rr = (4 + ph_ * 12) * k;
                    Pen ring(WithAlpha(pale, (BYTE)(128 * (1 - ph_))), 1);
                    g.DrawEllipse(&ring, ox + px + pw * 0.6f - rr, oy + sy - rr * 0.3f, rr * 2, rr * 0.6f);
                }
            for (int q = 0; q < 3; ++q) {
                float u = (float)std::fmod(t_ * 0.35 + q * 0.33, 1.0);
                Pen bp(WithAlpha(pale, (BYTE)(90 * (1 - u))), 1);
                g.DrawEllipse(&bp, ox + px + 8 * k + q * 12 * k - 1.6f * k, oy + py + ph - 2 * k - u * lvl - 1.6f * k,
                              3.2f * k, 3.2f * k);
            }
        }
        Pen edge(WithAlpha(p.accent2, 115), 1.5f * k);
        g.DrawEllipse(&edge, ox + px, oy + py - ry, pw, ry * 2);
        g.DrawLine(&edge, ox + px, oy + py, ox + px, oy + py + ph);
        g.DrawLine(&edge, ox + px + pw, oy + py, ox + px + pw, oy + py + ph);
        g.DrawArc(&edge, ox + px, oy + py + ph - ry, pw, ry * 2, 0, 180);
        SolidBrush tick(WithAlpha(p.accent2, 90));
        for (int q = 1; q < 4; ++q) g.FillRectangle(&tick, ox + px + pw - 8 * k, oy + py + ph * q / 4, 6 * k, 1.0f);
    }
    // носик на кромке колбы и струйка
    {
        SolidBrush sp(WithAlpha(p.accent2, 140));
        PointF nose[4] = {PointF(ox + fx + fw - 2 * k, oy + ly - 3 * k), PointF(ox + fx + fw + 9 * k, oy + ly - 6 * k),
                          PointF(ox + fx + fw + 9 * k, oy + ly - 2 * k), PointF(ox + fx + fw - 2 * k, oy + ly + 3 * k)};
        g.FillPolygon(&sp, nose, 4);
        if (ov && !game_) {
            PointF p0(fx + fw + 9 * k, ly - 4 * k), p1(px + pw * 0.6f + 10 * k, ly - 10 * k),
                p2(px + pw * 0.6f, sy - 2 * k);
            auto B = [&](float u) {
                float v = 1 - u;
                return PointF(ox + v * v * p0.X + 2 * u * v * p1.X + u * u * p2.X,
                              oy + v * v * p0.Y + 2 * u * v * p1.Y + u * u * p2.Y);
            };
            PointF pts[17];
            for (int q = 0; q <= 16; ++q) pts[q] = B(q / 16.0f);
            Pen st(MixColor(topCol, deep, 0.2), 2 * k);
            g.DrawLines(&st, pts, 17);
            SolidBrush db(topCol);
            for (int q = 0; q < 7; ++q) {
                float u = std::pow((float)std::fmod(t_ * 1.4 + q / 7.0, 1.0), 1.6f);
                PointF c = B(u);
                float rh = (2.6f + u * 1.5f) * k;
                g.FillEllipse(&db, c.X - 2 * k, c.Y - rh, 4 * k, rh * 2);
            }
            moving = true;
        }
    }
    Label(g, L"подкачка", RectF(ox + px - 20 * k, oy + py + ph + 8 * k, pw + 40 * k, 15 * k), ov ? p.accent : p.faint,
          10 * k, ov, 1);
    Label(g, pf_ > 0.02f ? Gb(pf_) + L", на диске" : std::wstring(L"пусто"),
          RectF(ox + px - 30 * k, oy + py + ph + 22 * k, pw + 60 * k, 14 * k), p.faint, 9.5f * k, false, 1);
    // справа: итог и список слоёв
    float lx = px + pw + 22 * k, lw = W - lx - 16 * k;
    Label(g, L"занято " + Gb(used) + L" из " + Gb(TOT), RectF(ox + lx, oy + 20 * k, lw, 20 * k), p.text, 14 * k, true);
    Label(g, ov ? L"не влезает: лишнее уходит в подкачку" : L"свободно " + Gb(std::max(0.0f, TOT - used)),
          RectF(ox + lx, oy + 41 * k, lw, 18 * k), ov ? p.accent : p.accent2, 11.5f * k);
    float yy = 80 * k;
    for (size_t i = 0; i < layers_.size() && yy < H - 50 * k; ++i) {
        const Layer& l = layers_[i];
        if (l.system) continue;
        BYTE a = (BYTE)(l.gb <= 0 ? 90 : hot_ >= 0 && hot_ != (int)i ? 128 : 255);
        SolidBrush sq(WithAlpha(cols[l.col], a));
        g.FillRectangle(&sq, ox + lx, oy + yy - 6 * k, 12 * k, 12 * k);
        Label(g, NoExe(l.name), RectF(ox + lx + 20 * k, oy + yy - 9 * k, lw - 110 * k, 18 * k), WithAlpha(p.text, a),
              12 * k, hot_ == (int)i);
        Label(g, l.gb <= 0 ? std::wstring(L"закрываю") : Gb(l.h) + (l.leak ? L" +" : L""),
              RectF(ox + lx + lw - 100 * k, oy + yy - 9 * k, 100 * k, 18 * k),
              WithAlpha(l.leak ? p.accent : p.muted, a), 11.5f * k, true, 2);
        yy += 26 * k;
    }
    std::wstring hint = hot_ >= 0 ? L"щелчок - закрыть " + NoExe(layers_[hot_].name) + L" (мягко, окна сохранят своё)"
                                  : std::wstring(L"щелчок по слою - закрыть; потяни колбу вбок - плеснёт");
    Label(g, hint, RectF(ox + lx, oy + H - 22 * k, lw, 16 * k), hot_ >= 0 ? p.muted : p.faint, 10.5f * k);
    g.Restore(gs);
    return (moving || Awake()) && !game_;
}

}  // namespace scene
