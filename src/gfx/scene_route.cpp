// Живой маршрут сети (см. scene.h, RouteScene): узлы на дугах, пакеты, нагрев звеньев, обрыв и сварка.
#include <algorithm>
#include <cmath>

#include "gfx/color.h"
#include "gfx/scene.h"

using namespace Gdiplus;

namespace scene {

namespace {

constexpr float kCut = 0.5f;  // где рвётся звено
constexpr float kPi = 3.14159265f;

float Last(const std::vector<float>& h) {
    for (auto it = h.rbegin(); it != h.rend(); ++it)
        if (*it >= 0) return *it;
    return -1;
}

}  // namespace

PointF RouteScene::On(int seg, float u) const {
    const Node& a = nodes_[seg];
    const Node& c = nodes_[std::min(seg + 1, (int)nodes_.size() - 1)];
    float qx = (a.x + c.x) / 2, qy = (a.y + c.y) / 2 - 38 * k_, v = 1 - u;
    return PointF(v * v * a.x + 2 * u * v * qx + u * u * c.x, v * v * a.y + 2 * u * v * qy + u * u * c.y);
}

bool RouteScene::TakeFix() {
    bool f = fix_;
    fix_ = false;
    return f;
}

bool RouteScene::Draw(Graphics& g, const RectF& r, float k, const Palette& p, const Input& in, const RouteData& d) {
    float dt = Step(r, k, in);
    int n = (int)d.nodes.size();
    if (n < 2) return false;
    float W = r.Width, H = r.Height, ox = r.X, oy = r.Y;
    if ((int)nodes_.size() != n) {
        nodes_.assign(n, Node{});
        for (int i = 0; i < n; ++i) nodes_[i].tag = d.nodes[i].tag;
        packets_.clear();
        broken_ = -1;
        weld_ = pulse_ = -1;
        Wake(4);
    }
    float base = 104 * k;  // сверху место под столбики замеров, снизу - под подписи и кнопку
    for (int i = 0; i < n; ++i) {
        nodes_[i].x = 46 * k + i * (W - 92 * k) / (n - 1);
        nodes_[i].y = base + (i % 2 ? -20 * k : 14 * k);
    }
    auto ms = [&](int link) { return link < (int)d.hist.size() ? Last(d.hist[link]) : -1.0f; };
    if (d.gen != gen_) {
        gen_ = d.gen;
        Wake(4);
    }
    // выход туннеля сменился: узел переворачивается, на середине поворота - новый код
    for (int i = 0; i < n; ++i) {
        Node& nd = nodes_[i];
        if (nd.flip < 0 && d.nodes[i].tag != nd.tag) {
            if (game_) {
                nd.tag = d.nodes[i].tag;
            } else {
                nd.flip = 0;
                Wake(3);
            }
        }
        if (nd.flip >= 0) {
            nd.flip += dt / 0.6f;
            if (nd.flip >= 0.5f && !nd.swapped) nd.swapped = true, nd.tag = d.nodes[i].tag;
            if (nd.flip >= 1) nd.flip = -1, nd.swapped = false, nd.flash = 1;
        }
        nd.flash = std::max(0.0f, nd.flash - dt * 1.5f);
    }
    // обрыв: первый неисправный узел; починили - концы свариваются, потом очередь уходит
    int b = -1;
    for (int i = 1; i < n; ++i)
        if (!d.nodes[i].ok) {
            b = i;
            break;
        }
    auto release = [&] {
        broken_ = -1;
        weld_ = -1;
        pulse_ = game_ ? -1 : 0;
        float lag = 0;
        for (Packet& q : packets_)
            if (q.wait) q.wait = false, q.rush = true, q.u -= lag, lag += 0.012f;
    };
    if (b > 0 && b != broken_) {
        broken_ = b;
        weld_ = -1;
        Wake(6);
    } else if (b < 0 && broken_ > 0 && weld_ < 0) {
        weldLink_ = broken_ - 1;
        if (game_)
            release();
        else
            weld_ = 0, Wake(4);
    }
    if (weld_ >= 0) {
        weld_ += dt / 0.9f;
        PointF w = On(weldLink_, kCut - 0.06f + 0.12f * std::min(1.0f, weld_));
        for (int q = 0; q < 3; ++q) {
            Dot s;
            s.x = w.X, s.y = w.Y, s.vx = Rnd(-140, 140) * k, s.vy = Rnd(-220, -40) * k, s.a = Rnd(0.3f, 0.6f);
            sparks_.push_back(s);
        }
        if (weld_ >= 1) release();
    }
    if (d.fixing && broken_ > 0) Wake(1);
    // пакеты: сыплются, пока сцена играет; доезжают до конца или до разрыва и ждут там
    bool feeding = (Awake() || d.checking) && !game_;
    spawn_ += dt;
    if (feeding && spawn_ > 0.08f) {
        spawn_ = 0;
        Packet q;
        q.dns = Rnd(0, 1) < 0.25f;
        packets_.push_back(q);
    }
    int queue = 0;
    for (const Packet& q : packets_) queue += q.wait;
    bool flowing = false;
    for (Packet& q : packets_) {
        if (q.wait) {
            if (Awake()) {
                q.jx += (Rnd(-1, 1) * 9 * k - q.jx) * std::min(1.0f, dt * 3);
                q.jy += (Rnd(-1, 1) * 9 * k - q.jy) * std::min(1.0f, dt * 3);
            }
            continue;
        }
        flowing = true;
        float m = ms(q.seg);
        q.u += dt * (1.7f / (1 + std::max(0.0f, m < 0 ? 10 : m) / 30)) * (q.rush ? 3 : 1);
        if (broken_ == q.seg + 1 && q.u >= kCut - 0.03f) {
            q.wait = true;
            q.u = kCut - 0.05f - std::min(0.12f, queue * 0.004f);
            q.jx = q.jy = 0;
            ++queue;
            continue;
        }
        if (q.u >= 1) q.seg++, q.u = 0;
    }
    {
        int waiting = 0;
        for (int i = (int)packets_.size() - 1; i >= 0; --i)
            if (packets_[i].wait && ++waiting > 45) packets_[i].seg = n;  // очередь не бесконечна
        packets_.erase(std::remove_if(packets_.begin(), packets_.end(), [&](const Packet& q) { return q.seg > n - 2; }),
                       packets_.end());
    }
    if (game_)
        packets_.erase(std::remove_if(packets_.begin(), packets_.end(), [](const Packet& q) { return !q.wait; }),
                       packets_.end());
    // искры у разрыва
    if (broken_ > 0 && weld_ < 0 && Awake() && Rnd(0, 1) < dt * (d.fixing ? 30 : 8)) {
        PointF e = On(broken_ - 1, kCut + Rnd(-0.06f, 0.06f));
        Dot s;
        s.x = e.X, s.y = e.Y, s.vx = Rnd(-60, 60) * k, s.vy = Rnd(-90, -10) * k, s.a = Rnd(0.2f, 0.4f);
        sparks_.push_back(s);
    }
    for (Dot& s : sparks_) s.vy += 600 * k * dt, s.x += s.vx * dt, s.y += s.vy * dt, s.a -= dt;
    sparks_.erase(std::remove_if(sparks_.begin(), sparks_.end(), [](const Dot& s) { return s.a <= 0; }), sparks_.end());
    if (pulse_ >= 0) {
        pulse_ += dt * 1.6f;
        for (int i = 0; i < n; ++i)
            if (std::fabs(pulse_ * (n - 1) - i) < 0.15f) nodes_[i].flash = 1;
        if (pulse_ > 1.2f) pulse_ = -1;
    }
    // наведение
    hotNode_ = hotLink_ = -1;
    if (over_) {
        for (int i = 0; i < n; ++i)
            if (std::hypot(mx_ - nodes_[i].x, my_ - nodes_[i].y) < 24 * k) hotNode_ = i;
        if (hotNode_ < 0)
            for (int i = 0; i + 1 < n; ++i)
                for (float u = 0.1f; u < 0.95f; u += 0.1f) {
                    PointF q = On(i, u);
                    if (std::hypot(mx_ - q.X, my_ - q.Y) < 12 * k) hotLink_ = i;
                }
    }
    bool hotBtn = broken_ > 0 && In(btn_);
    if (click_ && hotBtn && !d.fixing && weld_ < 0) fix_ = true;

    // ---- рисование ----
    GraphicsState gs = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Color pale = Pale(p);
    for (int i = 0; i + 1 < n; ++i) {
        const Node &a = nodes_[i], &c = nodes_[i + 1];
        float m = ms(i);
        bool bad = broken_ == i + 1;
        float heat = std::clamp((m - 25) / 90, 0.0f, 1.0f);
        float qx = (a.x + c.x) / 2, qy = (a.y + c.y) / 2 - 38 * k;
        if (bad) {
            // разрыв: две половины, концы разошлись и разлохмачены, между ними проскакивает дуга
            float gap = weld_ >= 0 && weldLink_ == i ? 0.06f * (1 - std::min(1.0f, weld_)) : 0.06f;
            auto half = [&](float u0, float u1, Color col) {
                PointF pts[13];
                for (int q = 0; q <= 12; ++q) {
                    PointF v = On(i, u0 + (u1 - u0) * q / 12);
                    pts[q] = PointF(ox + v.X, oy + v.Y);
                }
                Pen pen(col, 2 * k);
                g.DrawLines(&pen, pts, 13);
            };
            half(0, kCut - gap, WithAlpha(p.accent2, 128));
            half(kCut + gap, 1, WithAlpha(p.faint, 150));
            PointF e1 = On(i, kCut - gap), e2 = On(i, kCut + gap);
            Pen fray(WithAlpha(pale, 128), 1 * k);
            for (int side = 0; side < 2; ++side) {
                PointF e = side ? e2 : e1;
                float s = side ? -1.0f : 1.0f;
                for (int q = -1; q <= 1; ++q)
                    g.DrawLine(&fray, ox + e.X, oy + e.Y, ox + e.X + s * 5 * k,
                               oy + e.Y + q * 4 * k + std::sin((float)t_ * 20 + q) * 1.5f * k);
            }
            if (gap > 0.005f && (!Awake() || Rnd(0, 1) < 0.55f)) {
                PointF pts[7];
                pts[0] = PointF(ox + e1.X, oy + e1.Y);
                for (int q = 1; q < 6; ++q) {
                    float u = q / 6.0f;
                    pts[q] = PointF(ox + e1.X + (e2.X - e1.X) * u + Rnd(-3, 3) * k,
                                    oy + e1.Y + (e2.Y - e1.Y) * u + Rnd(-7, 7) * k);
                }
                pts[6] = PointF(ox + e2.X, oy + e2.Y);
                Pen arc(WithAlpha(pale, (BYTE)Rnd(100, 230)), 1.3f * k);
                g.DrawLines(&arc, pts, 7);
            }
        } else {
            BYTE al = (BYTE)(255 * std::min(1.0f, 0.45f + 0.4f * heat + (hotLink_ == i ? 0.3f : 0)));
            Pen pen(MixColor(p.accent2, p.accent, heat, al), (2 + heat * 3 + (hotLink_ == i ? 1 : 0)) * k);
            // квадратичная дуга как кубическая
            g.DrawBezier(
                &pen, PointF(ox + a.x, oy + a.y), PointF(ox + a.x + (qx - a.x) * 2 / 3, oy + a.y + (qy - a.y) * 2 / 3),
                PointF(ox + c.x + (qx - c.x) * 2 / 3, oy + c.y + (qy - c.y) * 2 / 3), PointF(ox + c.x, oy + c.y));
        }
        float lx = (a.x + c.x) / 2, ly = (a.y + c.y) / 2 - 36 * k;
        std::wstring lab;
        if (bad)
            lab = L"нет ответа" + (queue ? L", ждут " + std::to_wstring(queue) : std::wstring());
        else if (m >= 0)
            lab = std::to_wstring((int)std::lround(m)) + L" мс";
        Color lc = bad ? p.accent : heat > 0.4f ? MixColor(pale, p.accent, heat) : p.muted;
        Label(g, lab, RectF(ox + lx - 70 * k, oy + ly - (bad ? 10 : 0) * k - 8 * k, 140 * k, 16 * k), lc, 11 * k,
              bad || heat > 0.4f, 1);
    }
    // пакеты
    {
        SolidBrush req(pale), dns(p.accent2);
        for (const Packet& q : packets_) {
            PointF v = On(std::min(q.seg, n - 2), std::clamp(q.u, 0.0f, 1.0f));
            float x = ox + v.X + q.jx, y = oy + v.Y + q.jy, s = 2.5f * k;
            if (q.wait) {
                SolidBrush wb(MixColor(p.accent, pale, 0.5f + 0.5f * std::sin((float)t_ * 5 + q.jx)));
                g.FillRectangle(&wb, x - s, y - s, 2 * s, 2 * s);
            } else if (q.dns) {
                PointF dm[4] = {PointF(x, y - s * 1.4f), PointF(x + s * 1.4f, y), PointF(x, y + s * 1.4f),
                                PointF(x - s * 1.4f, y)};
                g.FillPolygon(&dns, dm, 4);
            } else {
                g.FillRectangle(&req, x - s, y - s, 2 * s, 2 * s);
            }
        }
    }
    // огонёк проверки после починки
    if (pulse_ >= 0 && pulse_ < 1) {
        float pp = pulse_ * (n - 1);
        int seg = std::min(n - 2, (int)pp);
        PointF v = On(seg, std::clamp(pp - seg, 0.0f, 1.0f));
        for (int q = 3; q >= 1; --q) {
            float rr = q * 5.5f * k;
            SolidBrush gb(WithAlpha(pale, (BYTE)(70 * (4 - q))));
            g.FillEllipse(&gb, ox + v.X - rr, oy + v.Y - rr, rr * 2, rr * 2);
        }
    }
    // узлы
    for (int i = 0; i < n; ++i) {
        const Node& nd = nodes_[i];
        bool bad = !d.nodes[i].ok && i == broken_;
        float pul = bad ? 0.5f + 0.5f * std::sin((float)t_ * 6) : 0;
        float sx = nd.flip >= 0 ? std::fabs(std::cos(nd.flip * kPi)) : 1;
        float x = ox + nd.x, y = oy + nd.y, R = 20 * k;
        if (nd.flash > 0) {
            float rr = R + 1 * k + (1 - nd.flash) * 22 * k;
            Pen fl(WithAlpha(p.accent2, (BYTE)(255 * nd.flash)), 2 * k);
            g.DrawEllipse(&fl, x - rr, y - rr, rr * 2, rr * 2);
        }
        float rw = R * std::max(0.02f, sx);
        SolidBrush fill(bad             ? MixColor(p.card, p.accent, 0.35f + 0.3f * pul)
                        : hotNode_ == i ? MixColor(p.card, p.text, 0.08)
                                        : p.card);
        g.FillEllipse(&fill, x - rw, y - R, rw * 2, R * 2);
        Pen edge(bad ? p.accent : WithAlpha(p.accent2, 150), 1.5f * k);
        g.DrawEllipse(&edge, x - rw, y - R, rw * 2, R * 2);
        if (sx > 0.2f)
            Label(g, nd.tag, RectF(x - R, y - 9 * k, R * 2, 18 * k), bad ? p.text : pale,
                  (nd.tag.size() > 2 ? 10 : 11) * k, true, 1);
        Label(g, d.nodes[i].name, RectF(x - 60 * k, y + 26 * k, 120 * k, 17 * k), p.text, 12 * k, true, 1);
        Label(g, d.nodes[i].sub, RectF(x - 64 * k, y + 43 * k, 128 * k, 15 * k),
              i == d.tunnel && nd.flip >= 0 ? p.accent2 : p.faint, 10.5f * k, false, 1);
    }
    // столбики последних замеров у звена под курсором
    if (hotLink_ >= 0 && hotLink_ < (int)d.hist.size() && !d.hist[hotLink_].empty()) {
        const std::vector<float>& h = d.hist[hotLink_];
        const Node &a = nodes_[hotLink_], &c = nodes_[hotLink_ + 1];
        float lx = ox + (a.x + c.x) / 2, ly = oy + (a.y + c.y) / 2 - 36 * k;
        size_t cnt = std::min<size_t>(h.size(), 32);
        float bw = 3 * k, mxv = 60;
        for (size_t q = h.size() - cnt; q < h.size(); ++q) mxv = std::max(mxv, h[q]);
        RectF box(lx - 58 * k, ly - 64 * k, 116 * k, 52 * k);
        if (box.Y < oy) box.Y = oy;
        FillRound(g, box, 6 * k, MixColor(p.card, Color(255, 0, 0, 0), 0.25));
        StrokeRound(g, box, 6 * k, WithAlpha(p.accent2, 60), 1);
        float x0 = box.X + 4 * k + (32 - cnt) * bw, by = box.Y + 36 * k;
        SolidBrush hi(p.accent), lo(WithAlpha(p.accent2, 200)), none(WithAlpha(p.faint, 160));
        for (size_t q = h.size() - cnt; q < h.size(); ++q, x0 += bw) {
            float v = h[q];
            if (v < 0) {
                g.FillRectangle(&none, x0 + 0.5f, by - 2 * k, bw - 1, 2 * k);
                continue;
            }
            float hh = std::max(1.0f, v / mxv * 30 * k);
            g.FillRectangle(v > 80 ? &hi : &lo, x0 + 0.5f, by - hh, bw - 1, hh);
        }
        int secs = (int)std::lround(cnt * d.every);
        Label(g, L"последние " + std::to_wstring(secs) + L" с", RectF(box.X, by + 1 * k, box.Width, 14 * k), p.faint,
              10 * k, false, 1);
    }
    // искры
    {
        Batch sb;
        for (const Dot& s : sparks_) sb.Add(0, ox + s.x, oy + s.y, 2.5f * k);
        sb.Flush(g, &pale, 1);
    }
    // кнопка починки или подсказка
    if (broken_ > 0) {
        btn_ = RectF(W / 2 - 84 * k, H - 32 * k, 168 * k, 28 * k);
        RectF br(ox + btn_.X, oy + btn_.Y, btn_.Width, btn_.Height);
        bool busy = d.fixing || weld_ >= 0;
        FillRound(g, br, 8 * k, hotBtn && !busy ? MixColor(p.accent, p.text, 0.15) : p.accent);
        Label(g, busy ? L"варю звено..." : L"Починить: " + d.nodes[broken_].name, br, Color(255, 255, 255, 255), 12 * k,
              true, 1);
    } else {
        btn_ = RectF();
        Label(g, L"наведи на звено - замеры за последние секунды", RectF(ox, oy + H - 18 * k, W - 8 * k, 16 * k),
              p.faint, 11 * k, false, 2);
    }
    Label(g, L"квадрат - запрос, ромб - DNS", RectF(ox + 8 * k, oy + 4 * k, W / 2, 16 * k), p.faint, 10.5f * k);
    g.Restore(gs);

    bool moving = flowing || !sparks_.empty() || weld_ >= 0 || pulse_ >= 0 || d.checking || (Awake() && !game_);
    for (const Node& nd : nodes_) moving = moving || nd.flip >= 0 || nd.flash > 0;
    return moving && !game_;
}

}  // namespace scene
