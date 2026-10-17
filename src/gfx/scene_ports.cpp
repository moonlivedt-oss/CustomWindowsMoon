// Порты кабелями (см. scene.h, PortsScene): гнездо, провисший кабель на связях, ток по нему, штекер мышью.
#include <algorithm>
#include <cmath>

#include "gfx/color.h"
#include "gfx/scene.h"

using namespace Gdiplus;

namespace scene {

namespace {

constexpr int kSeg = 14;  // звеньев в кабеле
constexpr float kPi = 3.14159265f;

std::wstring Age(int minutes) {
    if (minutes < 0) return L"";
    if (minutes < 60) return std::to_wstring(minutes) + L" мин";
    return std::to_wstring(minutes / 60) + L" ч";
}

bool Idle(const PortData& d) { return d.minutes >= 360 && d.conns == 0; }

}  // namespace

bool PortsScene::TakeKill(PortData& out) {
    if (kill_.empty()) return false;
    out = kill_.front();
    kill_.erase(kill_.begin());
    return true;
}

// Верёвка: точки помнят прошлое место (скорость - разница), тяжесть тянет вниз, связи держат длину.
void PortsScene::Rope(Row& w, float dt, float sx, float cx) {
    std::vector<Dot>& P = w.pts;
    for (Dot& q : P) {
        float vx = (q.x - q.px) * 0.97f, vy = (q.y - q.py) * 0.97f;
        q.px = q.x, q.py = q.y;
        q.x += vx;
        q.y += vy + 520 * k_ * dt * dt;
    }
    // запас длины под провис ~22 пикселя при любой ширине (парабола: лишнее = 8 h^2 / 3 L)
    float span = std::max(1.0f, cx - sx), sag = 22 * k_;
    float seg = (span + 8 * sag * sag / (3 * span)) / kSeg;
    bool dragged = drag_ >= 0 && &rows_[drag_] == &w;
    for (int it = 0; it < 16; ++it) {
        if (dragged) {
            P[0].x = mx_, P[0].y = my_;
        } else if (!w.cut) {
            float f = w.snap > 0 ? 0.35f : 1;
            P[0].x += (sx - P[0].x) * f;
            P[0].y += (w.y - P[0].y) * f;
        }
        P[kSeg].x = cx, P[kSeg].y = w.y;
        for (int q = 0; q < kSeg; ++q) {
            Dot &a = P[q], &b = P[q + 1];
            float dx = b.x - a.x, dy = b.y - a.y, d = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
            float f = (d - seg) / d / 2;
            if (q > 0 || w.cut) a.x += dx * f, a.y += dy * f;
            if (q < kSeg - 1) b.x -= dx * f, b.y -= dy * f;
        }
    }
}

bool PortsScene::Draw(Graphics& g, const RectF& r, float k, const Palette& p, const Input& in,
                      const std::vector<PortData>& data) {
    float dt = Step(r, k, in);
    float W = r.Width, H = r.Height, ox = r.X, oy = r.Y;
    float sx = 74 * k, cx = W - 200 * k;
    // строки по данным: свои живут, пропавшие падают, новые вплетаются снизу
    for (Row& w : rows_) w.seen = false;
    for (const PortData& d : data) {
        // выдернутый, но ещё живой (данные не обновились) тоже свой: не плодить заново
        auto it = std::find_if(rows_.begin(), rows_.end(),
                               [&](const Row& w) { return w.d.pid == d.pid && w.d.port == d.port; });
        if (it != rows_.end()) {
            it->d = d;
            it->seen = true;
            continue;
        }
        Row w;
        w.d = d;
        w.seen = true;
        w.y = w.ty = 26 * k + rows_.size() * 52 * k;
        for (int q = 0; q <= kSeg; ++q) {
            float u = (float)q / kSeg;
            Dot dt0;
            dt0.x = dt0.px = sx + (cx - sx) * u;
            dt0.y = dt0.py = w.y + std::sin(u * kPi) * 22 * k;
            w.pts.push_back(dt0);
        }
        rows_.push_back(w);
        Wake(2.5);
    }
    for (size_t i = 0; i < rows_.size(); ++i)
        if (!rows_[i].seen && !rows_[i].cut) {
            rows_[i].cut = true;
            if ((int)i == drag_) drag_ = -1;
            Wake(2);
        }
    // ход времени
    bool moving = false;
    int slot = 0;
    for (Row& w : rows_) {
        if (w.cut) {
            w.gone += dt;
            w.led = std::max(0.0f, w.led - dt * 2);
            moving = true;
        } else {
            w.ty = 26 * k + slot++ * 52 * k;
        }
        w.vy += ((w.ty - w.y) * 90 - w.vy * 13) * dt;
        if (!w.cut || w.gone < 0.6f) w.y += w.vy * dt;
        if (game_) w.y = w.ty, w.vy = 0;
        if (std::fabs(w.ty - w.y) > 0.3f || std::fabs(w.vy) > 0.5f) moving = true;
        w.flow += dt * (0.2f + std::min(4, w.d.conns) * 0.35f);
        w.snap = std::max(0.0f, w.snap - dt);
        // висящий кабель сам не замирает: тяжесть и связи спорят на доли пикселя, и сцена рисовала бы
        // 30 кадров в секунду без конца. Две секунды без дела (не тянут, не отпустили, строка на месте,
        // концы те же) - кабель замирает, пока его снова не тронут
        bool dragged = drag_ >= 0 && &rows_[drag_] == &w;
        bool busy = dragged || w.cut || w.snap > 0 || std::fabs(w.ty - w.y) > 0.3f || std::fabs(w.vy) > 0.5f ||
                    w.sx != sx || w.cx != cx;
        w.still = busy ? 0 : w.still + dt;
        w.sx = sx, w.cx = cx;
        if (w.still > 2 && !game_) continue;
        int steps = game_ ? 40 : 1;  // в игровом режиме кабель сразу провисает как надо
        for (int s = 0; s < steps; ++s) Rope(w, game_ ? 0.033f : dt, sx, cx);
        float motion = 0;
        for (const Dot& q : w.pts) motion += std::fabs(q.x - q.px) + std::fabs(q.y - q.py);
        if (motion > 0.05f * k && !game_) moving = true;
    }
    for (size_t i = 0; i < rows_.size();)
        if (rows_[i].cut && rows_[i].gone > 1.5f) {
            if (drag_ > (int)i) --drag_;
            rows_.erase(rows_.begin() + i);
        } else {
            ++i;
        }
    // мышь: штекер тянется, отпустил далеко - выдернут
    if (drag_ < 0) {
        hot_ = -1;
        if (over_)
            for (size_t i = 0; i < rows_.size(); ++i)
                if (!rows_[i].cut && std::fabs(my_ - rows_[i].y) < 22 * k) hot_ = (int)i;
    }
    auto cut = [&](Row& w, const wchar_t* how) {
        w.cut = true;
        kill_.push_back(w.d);
        toast_ = std::wstring(how) + w.d.proc + L" (порт " + std::to_wstring(w.d.port) + L")";
        toastT_ = 2.2f;
        Wake(2.5);
    };
    if (pressed_ && drag_ < 0)
        for (size_t i = 0; i < rows_.size(); ++i)
            if (!rows_[i].cut && std::hypot(mx_ - rows_[i].pts[0].x, my_ - rows_[i].pts[0].y) < 16 * k) {
                drag_ = (int)i;
                break;
            }
    if (released_ && drag_ >= 0) {
        Row& w = rows_[drag_];
        drag_ = -1;
        if (std::hypot(mx_ - sx, my_ - w.y) > 70 * k)
            cut(w, L"Завершаю ");
        else
            w.snap = 0.4f;
        Wake(1.5);
    }
    if (click_ && hot_ >= 0 && drag_ < 0 && mx_ > cx + 150 * k && mx_ < cx + 186 * k &&
        std::fabs(my_ - rows_[hot_].y) < 14 * k)
        cut(rows_[hot_], L"Завершаю ");
    if (toastT_ > 0) toastT_ -= dt, moving = true;

    // ---- рисование ----
    GraphicsState gs = g.Save();
    g.SetClip(r, CombineModeIntersect);  // выдернутый кабель свисает вниз - не дальше своей рамки
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Color pale = Pale(p), hiCard = MixColor(p.card, p.text, 0.08);
    for (size_t i = 0; i < rows_.size(); ++i) {
        Row& w = rows_[i];
        if (w.gone > 1.5f) continue;
        float a = w.cut ? std::clamp(1 - (w.gone - 0.7f) / 0.8f, 0.0f, 1.0f) : 1;
        BYTE A = (BYTE)(255 * a);
        bool on = (int)i == hot_ || (int)i == drag_;
        bool idle = Idle(w.d);
        std::vector<PointF> pts;
        for (const Dot& q : w.pts) pts.push_back(PointF(ox + q.x, oy + q.y));
        {
            Color cc = idle ? WithAlpha(p.faint, (BYTE)(180 * a))
                       : on ? WithAlpha(p.accent, A)
                            : WithAlpha(p.accent2, (BYTE)(140 * a));
            Pen cable(cc, (on ? 3 : 2.2f) * k);
            cable.SetLineJoin(LineJoinRound);
            g.DrawLines(&cable, pts.data(), (INT)pts.size());
        }
        if (!w.cut && !idle) {  // ток: штрихи бегут от гнезда к программе
            float pw = 1.4f * k;
            Pen cur(on ? pale : WithAlpha(pale, 115), pw);
            REAL dash[2] = {3 * k / pw, 12 * k / pw};
            cur.SetDashPattern(dash, 2);
            cur.SetDashOffset(-w.flow * 40 * k / pw);
            g.DrawLines(&cur, pts.data(), (INT)pts.size());
        }
        if (idle && !w.cut) {  // пыль на забытом кабеле
            SolidBrush dust(WithAlpha(p.faint, 180));
            for (int q = 2; q < kSeg - 1; q += 2)
                g.FillRectangle(&dust, ox + w.pts[q].x + std::sin((float)q) * 3 * k, oy + w.pts[q].y + 4 * k, 1.5f * k,
                                1.5f * k);
            const Dot& m = w.pts[kSeg / 2];
            Label(g, Age(w.d.minutes) + L" без дела", RectF(ox + m.x - 70 * k, oy + m.y + 9 * k, 140 * k, 15 * k),
                  p.faint, 10.5f * k, false, 1);
        }
        // штекер
        const Dot& p0 = w.pts[0];
        FillRound(g, RectF(ox + p0.x - 5 * k, oy + p0.y - 5 * k, 11 * k, 10 * k), 2 * k,
                  WithAlpha((int)i == drag_ ? p.accent : pale, A));
        // гнездо (остаётся на месте выдернутого, гаснет)
        float sy = oy + (w.cut ? w.ty : w.y);
        RectF sock(ox + sx - 60 * k, sy - 16 * k, 54 * k, 32 * k);
        FillRound(g, sock, 8 * k, MixColor(p.card, p.text, 0.04));
        StrokeRound(g, sock, 8 * k, WithAlpha(p.accent2, 76), 1);
        Color led = w.led > 0 ? MixColor(Deep(p), idle ? p.faint : p.accent, w.led) : Deep(p);
        SolidBrush lb(led);
        g.FillEllipse(&lb, ox + sx - 53.5f * k, sy - 3.5f * k, 7 * k, 7 * k);
        Label(g, std::to_wstring(w.d.port), RectF(ox + sx - 44 * k, sy - 9 * k, 40 * k, 18 * k),
              w.led > 0 ? p.text : p.faint, 12 * k, true);
        // карточка программы
        float yy = oy + w.y;
        RectF card(ox + cx, yy - 19 * k, 190 * k, 38 * k);
        FillRound(g, card, 9 * k, WithAlpha(on ? hiCard : MixColor(p.card, p.text, 0.04), A));
        StrokeRound(g, card, 9 * k, WithAlpha(p.accent2, (BYTE)(50 * a)), 1);
        Label(g, w.d.proc, RectF(card.X + 12 * k, yy - 16 * k, 136 * k, 18 * k), WithAlpha(p.text, A), 12.5f * k, true);
        std::wstring sub = (w.d.proj.empty() ? L"" : w.d.proj + L", ") + Age(w.d.minutes);
        if (w.d.conns) sub += L", соединений " + std::to_wstring(w.d.conns);
        if (sub.empty()) sub = L"PID " + std::to_wstring(w.d.pid);
        Label(g, sub, RectF(card.X + 12 * k, yy + 1 * k, 172 * k, 16 * k), WithAlpha(p.faint, A), 10.5f * k);
        if ((int)i == hot_ && !w.cut && drag_ < 0) {
            RectF x(ox + cx + 152 * k, yy - 12 * k, 30 * k, 24 * k);
            FillRound(g, x, 6 * k, p.accent);
            Label(g, L"×", x, Color(255, 255, 255, 255), 15 * k, true, 1);
        }
    }
    if (drag_ >= 0) {
        bool away = std::hypot(mx_ - sx, my_ - rows_[drag_].y) > 70 * k;
        Label(g, away ? L"отпусти - завершить " + rows_[drag_].d.proc : L"отпусти рядом - вернуть",
              RectF(ox + mx_ + 14 * k, oy + my_ - 24 * k, 260 * k, 18 * k), away ? p.accent : p.muted, 11.5f * k, true);
        moving = true;
    }
    if (toastT_ > 0) {
        BYTE A = (BYTE)(255 * std::clamp(toastT_, 0.0f, 1.0f));
        RectF tr(ox + W / 2 - 130 * k, oy + H - 30 * k, 260 * k, 26 * k);
        FillRound(g, tr, 8 * k, WithAlpha(MixColor(p.card, p.text, 0.06), A));
        Label(g, toast_, tr, WithAlpha(p.text, A), 11.5f * k, false, 1);
    } else {
        Label(g, L"потяни штекер из гнезда - выдернуть; отпустил рядом - защёлкнется",
              RectF(ox, oy + H - 18 * k, W - 8 * k, 16 * k), p.faint, 11 * k, false, 2);
    }
    g.Restore(gs);
    return (moving || Awake()) && !game_;
}

}  // namespace scene
