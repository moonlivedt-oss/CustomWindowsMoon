// Портал после входа: узор стекла (соты или осколки) и кадр раскрытия от курсора.
#include "desk/lockscreen.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "core/perf.h"
#include "desk/lock_internal.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace lockscreen {

using namespace detail;

// Узор стекла: соты или осколки. Для каждого пикселя - номер ячейки и насколько
// он далеко от её середины (0 - середина, 255 - граница), для каждой ячейки -
// середина и случайный сдвиг. Считается один раз при блокировке (около 50 мс),
// потом кадры только читают таблицы.
void Reveal::BuildCells() {
    PERF_SCOPE("lock.BuildCells");
    const int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    const double k = H / 1080.0;
    cell_.assign((size_t)W * H, 0);
    edge_.assign((size_t)W * H, 0);
    tex_.assign((size_t)W * H, 0);
    cellX_.clear();
    cellY_.clear();
    cellJit_.clear();
    auto hash = [](int a, int b) {
        uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return (h & 0xFFFFFF) / 16777216.0;
    };
    std::unordered_map<int64_t, uint16_t> ids;
    auto id = [&](int a, int b, double x, double y) {
        int64_t key = (int64_t)(a + 100000) << 32 | (uint32_t)(b + 100000);
        auto it = ids.find(key);
        if (it != ids.end()) return it->second;
        uint16_t n = (uint16_t)cellX_.size();
        ids.emplace(key, n);
        cellX_.push_back((float)x);
        cellY_.push_back((float)y);
        cellJit_.push_back((float)hash(a + 7, b - 3));
        return n;
    };

    if (pattern_ == 1) {
        // соты с плоским верхом: s - радиус до угла
        const double s = 34 * k, a = s * std::sqrt(3.0) / 2;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                double px = x + 0.5, py = y + 0.5;
                double q = (2.0 / 3 * px) / s, r = (-1.0 / 3 * px + std::sqrt(3.0) / 3 * py) / s;
                // округление кубических координат
                double cxq = q, czq = r, cyq = -q - r;
                double rx = std::round(cxq), ry = std::round(cyq), rz = std::round(czq);
                double dx = std::fabs(rx - cxq), dy = std::fabs(ry - cyq), dz = std::fabs(rz - czq);
                if (dx > dy && dx > dz)
                    rx = -ry - rz;
                else if (dy > dz)
                    ry = -rx - rz;
                else
                    rz = -rx - ry;
                int iq = (int)rx, ir = (int)rz;
                double hx = s * 1.5 * iq, hy = s * std::sqrt(3.0) * (ir + iq / 2.0);
                double ox = std::fabs(px - hx), oy = std::fabs(py - hy);
                double m = std::max(oy, ox * std::sqrt(3.0) / 2 + oy / 2) / a;  // 0 середина, 1 грань
                size_t i = (size_t)y * W + x;
                cell_[i] = id(iq, ir, hx, hy);
                edge_[i] = (uint8_t)std::min(255.0, m * 255);
            }
        }
    } else {
        // осколки: ячейки Вороного на сетке со случайными центрами - вытянутые
        // грани, как трещины
        const double g = 88 * k;
        int gw = (int)(W / g) + 2, gh = (int)(H / g) + 2;
        std::vector<float> sx((size_t)gw * gh), sy((size_t)gw * gh);
        for (int j = 0; j < gh; ++j)
            for (int i = 0; i < gw; ++i) {
                sx[(size_t)j * gw + i] = (float)((i + 0.1 + 0.8 * hash(i, j)) * g);
                sy[(size_t)j * gw + i] = (float)((j + 0.1 + 0.8 * hash(j + 31, i - 17)) * g);
            }
        for (int y = 0; y < H; ++y) {
            int gj = (int)(y / g);
            for (int x = 0; x < W; ++x) {
                int gi = (int)(x / g);
                double d1 = 1e18, d2 = 1e18;
                int bi = 0, bj = 0;
                for (int j = std::max(0, gj - 1); j <= std::min(gh - 1, gj + 1); ++j)
                    for (int i = std::max(0, gi - 1); i <= std::min(gw - 1, gi + 1); ++i) {
                        double ddx = x + 0.5 - sx[(size_t)j * gw + i], ddy = y + 0.5 - sy[(size_t)j * gw + i];
                        double d = ddx * ddx + ddy * ddy;
                        if (d < d1) {
                            d2 = d1;
                            d1 = d;
                            bi = i;
                            bj = j;
                        } else if (d < d2) {
                            d2 = d;
                        }
                    }
                d1 = std::sqrt(d1);
                d2 = std::sqrt(d2);
                double m = 2 * d1 / (d1 + d2);  // 0 у центра, 1 на трещине
                size_t idx = (size_t)y * W + x;
                cell_[idx] = id(bi, bj, sx[(size_t)bj * gw + bi], sy[(size_t)bj * gw + bi]);
                edge_[idx] = (uint8_t)std::min(255.0, m * 255);
            }
        }
    }

    // фактура стекла: каждая грань чуть своей яркости, по границам тёмная линия с
    // тонким бликом рядом
    for (size_t i = 0; i < tex_.size(); ++i) {
        double m = edge_[i] / 255.0;
        double facet = (cellJit_[cell_[i]] - 0.5) * (pattern_ == 1 ? 0.10 : 0.16);
        double line = m > 0.90 ? -0.55 * std::pow((m - 0.90) / 0.10, 1.5) : 0;
        double glint = m > 0.76 && m <= 0.90 ? 0.12 * std::sin((m - 0.76) / 0.14 * kPi) : 0;
        tex_[i] = (int8_t)std::clamp((facet + line + glint) * 127, -127.0, 127.0);
    }
}

// Кадр портала; t от 0 до 1 - вся сцена, в обратную сторону она же схлопывает
// портал.
//   0.00-0.30  зажигание: на стекле проступает узор, секторы по одному встают
//   вокруг курсора,
//              последним вспыхивает багровый
//   0.16-1.00  раскрытие: фронт идёт от курсора к краям экрана; с узором стекло
//   рассыпается
//              по ячейкам (каждая сжимается к своей середине и гаснет, ячейки
//              на фронте вспыхивают кромкой), без узора - круглый вырез с тенью
//              и светлой кромкой
void Reveal::Portal(double t) {
    PERF_SCOPE("lock.Portal");
    const int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    const double k = H / 1080.0;
    const double cx = origin_.x,
                 cy = origin_.y;  // откуда раскрывается: где был курсор
    const double maxR = std::hypot(std::max(cx, W - cx), std::max(cy, H - cy));

    const double ig = Clamp01(t / 0.30);           // зажигание
    const double op = Clamp01((t - 0.16) / 0.84);  // раскрытие
    const double e = InOut(op);
    const double vis = std::pow(1 - Clamp01((op - 0.30) / 0.70),
                                1.3);  // кольцо и кромка тают к концу
    const Color ac = ToColor(theme_.ac), hi = MixColor(ac, Color(255, 255, 255, 255), 0.45);
    const uint32_t hr = hi.GetR(), hg = hi.GetG(), hb = hi.GetB();
    double hole;  // где сейчас фронт

    if (pattern_ && !cell_.empty()) {
        const double span = (pattern_ == 1 ? 120 : 150) * k;  // за сколько пикселей позади фронта ячейка исчезает
        hole = e * (maxR + 2 * span);
        // на каждую ячейку: сколько от неё осталось (по радиусу), прозрачность,
        // накал кромки
        const size_t n = cellX_.size();
        keep_.resize(n);
        fade_.resize(n);
        heat_.resize(n);
        lift_.resize(n);
        lightX_.resize(n);
        lightY_.resize(n);
        // приподнятые соты: вокруг курсора поднимаются, пока зажигается кольцо,
        // и волна приподнятых бежит перед фронтом
        const double liftR = 230 * k, igE = EaseOut(ig);
        for (size_t c = 0; c < n; ++c) {
            double d = std::hypot(cellX_[c] - cx, cellY_[c] - cy) + (cellJit_[c] - 0.5) * span * 0.9;
            double p = Clamp01((hole - d) / span);                              // 0 - цела, 1 - исчезла
            double front = std::exp(-std::pow((d - hole) / (span * 0.55), 2));  // ячейки у фронта горят
            keep_[c] = (uint16_t)(256 * (1 - p * p * (3 - 2 * p)));
            fade_[c] = (uint16_t)(256 * (1 - p * p * p));
            heat_[c] = (uint16_t)(256 * std::min(1.0, front * vis * 1.1));
            double dc = std::hypot(cellX_[c] - cx, cellY_[c] - cy);
            double close = std::exp(-std::pow(dc / liftR, 2)) * igE;
            double ahead = op > 0 ? 0.8 * std::exp(-std::pow((d - hole - span * 0.9) / (span * 0.7), 2)) : 0;
            lift_[c] = (uint16_t)(256 * std::max(close, ahead) * (1 - p));
            // свет идёт от курсора: грань, обращённая к нему, светлее
            double lx = cx - cellX_[c], ly = cy - cellY_[c], ll = std::max(1.0, std::hypot(lx, ly));
            lightX_[c] = (float)(lx / ll);
            lightY_[c] = (float)(ly / ll);
        }
        const int pat = (int)(256 * EaseOut(ig));  // узор проступает при зажигании
        for (size_t i = 0, total = (size_t)W * H; i < total; ++i) {
            uint16_t c = cell_[i];
            uint32_t ed = edge_[i], kp = keep_[c];
            if (ed >= (kp * 255 >> 8) && kp < 256) {
                bits_[i] = 0;
                continue;
            }
            uint32_t p = src_[i];
            int m = 256 + (pat * tex_[i] >> 7);
            int lit = 0;  // доля света с курсора на грани приподнятой соты
            if (uint32_t lf = lift_[c]) {
                m += (int)(lf * 56 >> 8);  // приподнятая сота светлее
                if (ed > 150) {
                    // направление от середины соты к пикселю
                    int y = (int)(i / W), x = (int)(i - (size_t)y * W);
                    double dx = x + 0.5 - cellX_[c], dy = y + 0.5 - cellY_[c];
                    double dl = std::max(1.0, std::sqrt(dx * dx + dy * dy));
                    double sh = (dx * lightX_[c] + dy * lightY_[c]) / dl;  // +1 к свету, -1 от него
                    double rw = (ed - 150) / 105.0;
                    m += (int)(lf * rw * sh * 150 / 256);
                    if (sh > 0) lit = (int)(lf * rw * sh * 0.7);
                    if (ed > 230) m -= (int)(lf * 110 >> 8);  // шов под приподнятой сотой темнее - тень
                }
            }
            m = std::max(m, 0);  // тень на гранях может увести множитель в минус - цвет переполнится
            int r = std::min(255, (int)(p >> 16 & 255) * m >> 8), g = std::min(255, (int)(p >> 8 & 255) * m >> 8),
                b = std::min(255, (int)(p & 255) * m >> 8);
            if (lit) {
                r += ((int)hr - r) * lit >> 8;
                g += ((int)hg - g) * lit >> 8;
                b += ((int)hb - b) * lit >> 8;
            }
            // кромка ячейки на фронте: светится у оставшейся границы
            uint32_t ht = heat_[c];
            if (ht) {
                int lim = (int)(kp * 255 >> 8);
                int dist = lim - (int)ed;  // насколько пиксель внутри от края того, что осталось
                if (dist < 40) {
                    int gl = (int)ht * (40 - dist) / 40;
                    r += ((int)hr - r) * gl >> 8;
                    g += ((int)hg - g) * gl >> 8;
                    b += ((int)hb - b) * gl >> 8;
                }
            }
            uint32_t a = fade_[c];
            bits_[i] = (std::min(255u, 255 * a >> 8)) << 24 | (uint32_t)(r * a >> 8) << 16 |
                       (uint32_t)(g * a >> 8) << 8 | (uint32_t)(b * a >> 8);
        }
    } else {
        const double f = 30 * k;   // мягкость края картинки
        const double sh = 46 * k;  // тень внутри выреза: портал «глубже» стола
        hole = e * (maxR + 2 * f);
        const double inner = hole - f, outer = hole + f, deep = inner - sh;
        const double rim = 0.32 * vis, shadow = 0.38 * vis;
        for (int y = 0; y < H; ++y) {
            uint32_t* row = bits_ + (size_t)y * W;
            const uint32_t* src = src_.data() + (size_t)y * W;
            double dy = y + 0.5 - cy, dy2 = dy * dy;
            if (outer <= 0 || dy2 >= outer * outer) {
                memcpy(row, src, (size_t)W * 4);
                continue;
            }
            int xo0 = std::clamp((int)std::floor(cx - std::sqrt(outer * outer - dy2)), 0, W);
            int xo1 = std::clamp((int)std::ceil(cx + std::sqrt(outer * outer - dy2)), xo0, W);
            int xi0 = xo1, xi1 = xo1;
            if (deep > 0 && dy2 < deep * deep) {
                double half = std::sqrt(deep * deep - dy2);
                xi0 = std::clamp((int)std::ceil(cx - half), xo0, xo1);
                xi1 = std::clamp((int)std::floor(cx + half), xi0, xo1);
            }
            memcpy(row, src, (size_t)xo0 * 4);
            memcpy(row + xo1, src + xo1, (size_t)(W - xo1) * 4);
            if (xi1 > xi0) memset(row + xi0, 0, (size_t)(xi1 - xi0) * 4);
            auto band = [&](int a, int b) {
                for (int x = a; x < b; ++x) {
                    double dx = x + 0.5 - cx, d = std::sqrt(dx * dx + dy2);
                    double s = Clamp01((d - deep) / sh);
                    if (d < inner) {
                        row[x] = (uint32_t)(shadow * s * s * 255) << 24;
                        continue;
                    }
                    double m = Clamp01((d - inner) / (2 * f));
                    m = m * m * (3 - 2 * m);
                    double glow = rim * std::exp(-std::fabs(d - hole) / (0.8 * f));
                    uint32_t p = src[x];
                    double r = (p >> 16 & 255) * m, g = (p >> 8 & 255) * m, b = (p & 255) * m, a = 255 * m;
                    r += (hr - r) * glow;
                    g += (hg - g) * glow;
                    b += (hb - b) * glow;
                    a += (255 - a) * glow;
                    a = a + (255 - a) * shadow * s * s * (1 - m);
                    row[x] = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
                }
            };
            band(xo0, xi1 > xi0 ? xi0 : xo1);
            if (xi1 > xi0) band(xi1, xo1);
        }
    }

    Bitmap surf(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
    Graphics g(&surf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    const Color steel = MixColor(ToColor(theme_.muted), ToColor(theme_.fg), 0.25);

    // вспышка у курсора в момент, когда портал начинает раскрываться
    double bloom = std::exp(-std::pow((op - 0.06) / 0.09, 2)) * (t > 0.12 ? 1 : 0);
    if (bloom > 0.01) {
        double br = 70 * k + std::min(hole, 400 * k) * 1.2 + 60 * k * op;
        GraphicsPath bp;
        bp.AddEllipse((REAL)(cx - br), (REAL)(cy - br), (REAL)(2 * br), (REAL)(2 * br));
        PathGradientBrush pg(&bp);
        pg.SetCenterPoint(PointF((REAL)cx, (REAL)cy));
        pg.SetCenterColor(WithAlpha(hi, (BYTE)(150 * bloom)));
        Color edge = WithAlpha(ac, 0);
        int n = 1;
        pg.SetSurroundColors(&edge, &n);
        g.FillPath(&pg, &bp);
    }

    // без узора - кромка выреза и бегущая по ней искра
    if (!pattern_ && hole > 2 && vis > 0.003) {
        for (int i = 3; i >= 1; --i) {
            Pen glow(WithAlpha(ac, (BYTE)(36 * vis * (4 - i))), (REAL)(i * 9 * k));
            g.DrawEllipse(&glow, (REAL)(cx - hole), (REAL)(cy - hole), (REAL)(2 * hole), (REAL)(2 * hole));
        }
        Pen core(WithAlpha(hi, (BYTE)(235 * vis)), (REAL)(2.2 * k));
        g.DrawEllipse(&core, (REAL)(cx - hole), (REAL)(cy - hole), (REAL)(2 * hole), (REAL)(2 * hole));
        if (hole > 30 * k) {
            double ca = -90 + 520 * e;
            for (int i = 0; i < 6; ++i) {
                Pen tail(WithAlpha(i ? hi : Color(255, 255, 255, 255), (BYTE)(230 * vis * (1 - i / 6.0))),
                         (REAL)((3.4 - i * 0.4) * k));
                tail.SetStartCap(LineCapRound);
                tail.SetEndCap(LineCapRound);
                g.DrawArc(&tail, (REAL)(cx - hole), (REAL)(cy - hole), (REAL)(2 * hole), (REAL)(2 * hole),
                          (REAL)(ca - i * 6), 6.5f);
            }
        }
    }

    // кольцо секторов, как на аватаре: встают по одному, потом идут впереди
    // фронта, поворачиваются и тают
    const int kSectors = 7, kHot = 1;
    const double ringR = 96 * k + hole;
    const double rot = -90 - 180.0 / kSectors + 150 * e;
    const double gap = 3.2, width = 22 * k * (1 + 0.6 * e);
    for (int j = 0; j < kSectors; ++j) {
        int i = (kHot + 1 + j) % kSectors;  // по часовой, «наведённый» последним
        double sp = Clamp01((ig - j * 0.09) / 0.36);
        if (sp <= 0) continue;
        double pop = EaseOutBack(sp, 1.8);
        double a0 = rot + i * 360.0 / kSectors + gap / 2, sweep = (360.0 / kSectors - gap) * std::min(1.0, pop);
        bool hot = i == kHot;
        double rr = ringR * (0.82 + 0.18 * pop) + width / 2 + (hot ? 6 * k : 0);
        double a = std::min(1.0, sp * 2) * vis;
        Pen seg(WithAlpha(hot ? ac : steel, (BYTE)((hot ? 235 : 150) * a)), (REAL)width);
        g.DrawArc(&seg, (REAL)(cx - rr), (REAL)(cy - rr), (REAL)(2 * rr), (REAL)(2 * rr), (REAL)a0, (REAL)sweep);
        if (hot) {
            double flash = std::exp(-std::pow((sp - 0.55) / 0.2, 2));
            double r2 = rr + width / 2 + 7 * k;
            Pen rimPen(WithAlpha(hi, (BYTE)(std::min(255.0, 220 * a + 35 * flash))), (REAL)((2.6 + 2.4 * flash) * k));
            rimPen.SetStartCap(LineCapRound);
            rimPen.SetEndCap(LineCapRound);
            g.DrawArc(&rimPen, (REAL)(cx - r2), (REAL)(cy - r2), (REAL)(2 * r2), (REAL)(2 * r2), (REAL)(a0 + 1.5),
                      (REAL)std::max(0.0, sweep - 3));
        }
    }

    // искры: срываются с фронта и улетают наружу, как лепестки на картинке
    if (hole > 10 * k && vis > 0.01) {
        uint32_t seed = 0xA341316Cu;
        auto rnd = [&] {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) / 16777216.0;
        };
        SolidBrush dot(Color(0, 0, 0, 0));
        for (int i = 0; i < 34; ++i) {
            double ang = Rad(rnd() * 360), start = 0.05 + rnd() * 0.5, speed = 120 + rnd() * 300,
                   size = (2.4 + rnd() * 3.4) * k;
            double life = Clamp01((op - start) / 0.45);
            if (life <= 0 || life >= 1) continue;
            double born = InOut(start) * maxR;
            double r = born + 96 * k * rnd() + speed * k * EaseOut(life);
            double fade = (1 - life) * (1 - life);
            PointF p((REAL)(cx + std::cos(ang) * r), (REAL)(cy + std::sin(ang) * r));
            PointF q((REAL)(cx + std::cos(ang) * (r - 26 * k * (1 - life))),
                     (REAL)(cy + std::sin(ang) * (r - 26 * k * (1 - life))));
            Pen trail(WithAlpha(ac, (BYTE)(140 * fade)), (REAL)(size * 0.7));
            trail.SetStartCap(LineCapRound);
            trail.SetEndCap(LineCapRound);
            g.DrawLine(&trail, q, p);
            dot.SetColor(WithAlpha(i % 3 ? hi : ac, (BYTE)(230 * fade)));
            g.FillEllipse(&dot, (REAL)(p.X - size / 2), (REAL)(p.Y - size / 2), (REAL)size, (REAL)size);
        }
    }
}

}  // namespace lockscreen
