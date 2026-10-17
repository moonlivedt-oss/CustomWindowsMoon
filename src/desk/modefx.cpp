#include "desk/modefx.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/night.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/hex.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialModeFx";
constexpr UINT_PTR kTimer = 1;
ModeFx* g_self = nullptr;

// длительности фиксированные: в игровом режиме общая скорость «мгновенно», а эта анимация и есть знак
constexpr double kGameOn = 2900, kGameOff = 2300, kDevOn = 3000, kDevOff = 2300;

double Smooth(double t) {
    t = Clamp01(t);
    return t * t * (3 - 2 * t);
}

// псевдослучайное число 0..1 по номеру: осколки и искры одинаковы от кадра к кадру
double Rnd(int i, int salt) {
    unsigned x = (unsigned)i * 2654435761u ^ (unsigned)salt * 40503u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (x & 0xFFFF) / 65535.0;
}

Color Lerp(Color a, Color b, double t) { return MixColor(a, b, Clamp01(t)); }

// точка на кубической кривой Безье
PointF Bez(const PointF& a, const PointF& b, const PointF& c, const PointF& d, double t) {
    double u = 1 - t;
    double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
    return PointF((REAL)(a.X * w0 + b.X * w1 + c.X * w2 + d.X * w3), (REAL)(a.Y * w0 + b.Y * w1 + c.Y * w2 + d.Y * w3));
}

}  // namespace

void ModeFx::Create(HINSTANCE inst) {
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
}

void ModeFx::Destroy() {
    Stop();
    if (wnd_) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

LRESULT CALLBACK ModeFx::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_TIMER && wp == kTimer && g_self) {
        g_self->Frame();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

double ModeFx::Total() const { return kind_ == GAME ? (on_ ? kGameOn : kGameOff) : (on_ ? kDevOn : kDevOff); }

void ModeFx::Play(Kind kind, bool on, const std::wstring& title, const std::vector<std::wstring>& chips,
                  const ThemeInfo& theme) {
    QUERY_USER_NOTIFICATION_STATE st{};
    if (SUCCEEDED(SHQueryUserNotificationState(&st)) && st == QUNS_RUNNING_D3D_FULL_SCREEN) return;
    Stop();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    mon_ = mi.rcMonitor;
    int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    HDC scr = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_) {
        DeleteDC(memDC_);
        memDC_ = nullptr;
        return;
    }
    old_ = SelectObject(memDC_, dib_);
    kind_ = kind;
    on_ = on;
    title_ = title;
    chips_ = chips;
    if (chips_.size() > 6) chips_.resize(6);
    theme_ = theme;
    t0_ = GetTickCount64();
    Frame();
    SetTimer(wnd_, kTimer, 15, nullptr);
}

void ModeFx::Frame() {
    const double el = (double)(GetTickCount64() - t0_);
    if (el >= Total()) {
        Stop();
        return;
    }
    const int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(MonitorFromPoint({mon_.left, mon_.top}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dpi, &dy);
    Draw(bits_, W, H, dpi / 96.0, el);
    Present(W, H);
}

void ModeFx::Draw(void* bits, int W, int H, double k, double el) const {
    const double total = Total();
    memset(bits, 0, (size_t)W * H * 4);
    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        const float glowK = night::Glow();
        const Color white(255, 255, 255, 255);
        const Color accent = ToColor(theme_.ac);
        const Color steel = MixColor(ToColor(theme_.muted), ToColor(theme_.fg), 0.3);
        // чертёжный синий, чуть подкрашенный темой - чтобы Unreal отличался от игры с первого взгляда
        const Color blue = MixColor(Color(255, 86, 168, 255), accent, 0.18);
        Color base = kind_ == DEV ? blue : on_ ? accent : steel;
        Color rim = MixColor(base, white, 0.45);

        // ---------- плашка: где она и какая (нужна всем сценам) ----------
        Font font(Family(), (REAL)(14 * k), FontStyleRegular, UnitPixel);
        RectF tb;
        g.MeasureString(title_.c_str(), -1, &font, PointF(0, 0), &tb);
        const double ph = 38 * k, pwFull = tb.Width + 52 * k;
        const double cx = W / 2.0, cy = H - 92 * k - ph / 2;  // середина плашки
        // раскрытие плашки (0 - сота-ядро, 1 - полная ширина) и общая видимость
        double open = 0, alpha = 0, fadeOut = Clamp01((total - el) / 380);
        if (kind_ == GAME && on_) {
            open = EaseOutBack((el - 480) / 420, 1.2);
            alpha = el < 480 ? 0 : fadeOut;
        } else if (on_) {
            open = EaseOutBack((el - 300) / 420, 1.2);
            alpha = Clamp01((el - 300) / 120) * fadeOut;
        } else {
            open = el < total - 520 ? EaseOut(el / 260) : 1 - Smooth((el - (total - 520)) / 420);
            alpha = Clamp01(el / 120) * Clamp01((total - el) / 120);
        }
        open = std::max(0.0, open);
        const double pw = ph + (pwFull - ph) * std::min(open, 1.08);

        // ================= ИГРА =================
        if (kind_ == GAME) {
            // волна сот по периметру: включение - от середины низа вверх, выключение - обратно
            const double P = 2.0 * (W + H), half = P / 2;
            double front = -1, fade = 1;
            if (on_) {
                double t = Clamp01((el - 500) / 1150);
                if (el > 500 && t < 1) front = (t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t)) * half * 1.05;
                fade = t < 0.72 ? 1 : (1 - t) / 0.28;
            } else {
                double t = Clamp01(el / 1100);
                if (t < 1) front = (1 - Smooth(t)) * half * 1.05;
                fade = t < 0.15 ? t / 0.15 : 1;
            }
            if (front >= 0) {
                const double s = 14 * k, band = 96 * k, wv = 240 * k;
                auto perim = [&](double x, double y) {
                    double dl = x, dr = W - x, dt = y, db = H - y, m = std::min({dl, dr, dt, db});
                    double p;
                    if (m == db)
                        p = W / 2.0 - x;
                    else if (m == dl)
                        p = W / 2.0 + (H - y);
                    else if (m == dt)
                        p = W / 2.0 + H + x;
                    else
                        p = W / 2.0 + H + W + y;
                    if (p < 0) p += P;
                    return std::pair<double, double>(std::min(p, P - p), m);
                };
                auto cellAt = [&](const hex::Cell& c) {
                    auto [pd, depth] = perim(c.x, c.y);
                    double d = (pd - front) / wv, lit = std::exp(-d * d);
                    // включение: за фронтом остаётся тлеющий след; выключение: впереди фронта (к середине низа)
                    // ещё горит, позади уже погасло
                    if (pd < front) lit = std::max(lit, on_ ? 0.3 * std::exp((pd - front) / (650 * k)) : 0.42);
                    double reach = band * (0.35 + 0.65 * std::exp(-d * d));
                    double a = lit * std::max(0.0, 1 - depth / reach) * fade * glowK;
                    if (a < 0.02) return;
                    GraphicsPath p;
                    hex::Path(p, c.x, c.y, s, 0.84);
                    SolidBrush fill(WithAlpha(base, (BYTE)std::min(255.0, 175 * a)));
                    g.FillPath(&fill, &p);
                    Pen edge(WithAlpha(rim, (BYTE)std::min(255.0, 240 * a)), (REAL)(1.3 * k));
                    g.DrawPath(&edge, &p);
                };
                hex::ForEach(0, 0, W, band + s, s, cellAt);
                hex::ForEach(0, H - band - s, W, H, s, cellAt);
                hex::ForEach(0, band + s, band + s, H - band - s, s, cellAt);
                hex::ForEach(W - band - s, band + s, W, H - band - s, s, cellAt);
            }

            if (on_) {
                // ядро заряжается: сота растёт, вокруг три дуги набирают круг
                if (el < 560) {
                    double t = el / 480;
                    double r = 22 * k * EaseOutBack(el / 320, 1.6);
                    for (int ring = 0; ring < 3; ++ring) {
                        double rr = r + (12 + ring * 9) * k, sweep = 110 * Clamp01(t * 1.25 - ring * 0.12);
                        double rot = el * (0.45 + ring * 0.2) * (ring % 2 ? -1 : 1);
                        Pen arc(WithAlpha(rim, (BYTE)(220 * glowK * (1 - ring * 0.2))), (REAL)((2.6 - ring * 0.6) * k));
                        arc.SetStartCap(LineCapRound);
                        arc.SetEndCap(LineCapRound);
                        for (int j = 0; j < 3; ++j)
                            if (sweep > 1)
                                g.DrawArc(&arc, (REAL)(cx - rr), (REAL)(cy - rr), (REAL)(2 * rr), (REAL)(2 * rr),
                                          (REAL)(rot + j * 120), (REAL)sweep);
                    }
                    // свечение ядра
                    for (int i = 4; i >= 1; --i) {
                        GraphicsPath gp;
                        hex::Path(gp, cx, cy, r + i * 6 * k * t);
                        SolidBrush gb(WithAlpha(base, (BYTE)(26 * t * glowK)));
                        g.FillPath(&gb, &gp);
                    }
                    GraphicsPath hp;
                    hex::Path(hp, cx, cy, std::max(1.0, r));
                    SolidBrush hb(Lerp(base, white, 0.25 + 0.5 * Clamp01((el - 380) / 100)));
                    g.FillPath(&hb, &hp);
                }
                // ударная волна: два кольца от ядра через весь экран
                const double maxR = std::hypot(W / 2.0, (double)H);
                for (int i = 0; i < 2; ++i) {
                    double p = (el - 480 - i * 110) / (i ? 1000 : 820);
                    if (p <= 0 || p >= 1) continue;
                    double R = maxR * EaseOut(p), a = std::pow(1 - p, 1.6) * glowK;
                    Pen ring(WithAlpha(i ? base : rim, (BYTE)(210 * a)), (REAL)((i ? 2.0 : 5.0) * k * (1 - p) + 1));
                    g.DrawEllipse(&ring, (REAL)(cx - R), (REAL)(cy - R), (REAL)(2 * R), (REAL)(2 * R));
                    if (!i) {  // мягкий ореол за первым кольцом
                        Pen halo(WithAlpha(base, (BYTE)(60 * a)), (REAL)(18 * k * (1 - p) + 2));
                        g.DrawEllipse(&halo, (REAL)(cx - R + 8 * k), (REAL)(cy - R + 8 * k), (REAL)(2 * R - 16 * k),
                                      (REAL)(2 * R - 16 * k));
                    }
                }
                // осколки-соты разлетаются веером вверх и в стороны, тормозят и гаснут
                double ts = (el - 480) / 1000.0;
                if (ts > 0 && ts < 1.15)
                    for (int i = 0; i < 42; ++i) {
                        double life = 0.6 + 0.55 * Rnd(i, 1);
                        if (ts > life) continue;
                        double ang = kPi + kPi * Rnd(i, 2) + (Rnd(i, 3) - 0.5) * 0.5;  // верхняя полуплоскость
                        double v = (700 + 1100 * Rnd(i, 4)) * k, drag = 2.6;
                        double dist = v * (1 - std::exp(-drag * ts)) / drag;
                        double x = cx + std::cos(ang) * dist, y = cy + std::sin(ang) * dist + 120 * k * ts * ts;
                        double a = std::pow(1 - ts / life, 1.3) * glowK, sz = (4 + 6 * Rnd(i, 5)) * k * (1 - 0.4 * ts);
                        GraphicsPath sp;
                        hex::Path(sp, x, y, sz);
                        Matrix m;
                        m.RotateAt((REAL)(ts * 400 * (Rnd(i, 6) - 0.5)), PointF((REAL)x, (REAL)y));
                        sp.Transform(&m);
                        SolidBrush sb(WithAlpha(Rnd(i, 7) > 0.5 ? rim : base, (BYTE)(230 * a)));
                        g.FillPath(&sb, &sp);
                    }
            } else {
                // выключение: искры остывают и поднимаются от плашки
                double ts = el / 1500.0;
                if (ts < 1)
                    for (int i = 0; i < 30; ++i) {
                        double start = 0.45 * Rnd(i, 11), life = 0.45 + 0.4 * Rnd(i, 12), lt = (ts - start) / life;
                        if (lt <= 0 || lt >= 1) continue;
                        double x0 = cx + (Rnd(i, 13) - 0.5) * pwFull, y0 = cy - ph / 2;
                        double x = x0 + std::sin(lt * 6 + i) * 10 * k, y = y0 - lt * (90 + 120 * Rnd(i, 14)) * k;
                        Color c = Lerp(accent, steel, lt * 1.4);
                        double a = std::sin(lt * kPi) * glowK, r = (1.6 + 2.2 * Rnd(i, 15)) * k;
                        SolidBrush sb(WithAlpha(c, (BYTE)(220 * a)));
                        g.FillEllipse(&sb, (REAL)(x - r), (REAL)(y - r), (REAL)(2 * r), (REAL)(2 * r));
                    }
            }
        }

        // ================= UNREAL =================
        if (kind_ == DEV) {
            const double cell = 40 * k;
            // луч и сетка чертежа: включение - снизу вверх, выключение - сверху вниз стирает сетку
            double sweep = on_ ? Smooth(el / 1000) : Smooth(el / 1100);
            double scanY = on_ ? H * (1 - sweep) : H * sweep;
            double gridFade = on_ ? Clamp01((total - 500 - el) / 700) : 1;
            if (sweep < 1 || on_) {
                Pen line(WithAlpha(base, 255), (REAL)(1 * k));
                g.SetSmoothingMode(SmoothingModeNone);
                for (double y = std::fmod(H, cell); y < H; y += cell) {
                    // включение: сетка видна ниже луча, тает за ним; выключение: видна ниже луча (ещё не стёрта)
                    double d = y - scanY;
                    if (d < 0) continue;
                    double a = (on_ ? std::max(0.18, std::exp(-d / (300 * k))) : 0.35) * gridFade * glowK;
                    bool major = (int)std::lround(y / cell) % 4 == 0;
                    line.SetColor(WithAlpha(base, (BYTE)((major ? 70 : 34) * a)));
                    g.DrawLine(&line, 0.0f, (REAL)y, (REAL)W, (REAL)y);
                }
                for (double x = std::fmod(W / 2.0, cell); x < W; x += cell) {
                    bool major = (int)std::lround((x - W / 2.0) / cell) % 4 == 0;
                    double a = (on_ ? 0.7 : 0.35) * gridFade * glowK;
                    // вертикали - градиентом от луча вниз
                    LinearGradientBrush vb(PointF(0, (REAL)scanY), PointF(0, (REAL)(scanY + (on_ ? 520 : 900) * k)),
                                           WithAlpha(base, (BYTE)((major ? 80 : 40) * a)),
                                           WithAlpha(base, (BYTE)((major ? 26 : 10) * a * (on_ ? 0.4 : 1))));
                    Pen vp(&vb, (REAL)(1 * k));
                    g.DrawLine(&vp, (REAL)x, (REAL)scanY, (REAL)x, (REAL)H);
                }
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                // сам луч: яркая линия с мягким светом
                if (sweep < 1) {
                    double la = std::sin(sweep * kPi) * 0.6 + 0.4;
                    LinearGradientBrush glow(PointF(0, (REAL)(scanY - 60 * k * (on_ ? -1 : 1))), PointF(0, (REAL)scanY),
                                             WithAlpha(base, 0), WithAlpha(base, (BYTE)(90 * la * glowK)));
                    double gy = on_ ? scanY : scanY - 60 * k;
                    g.FillRectangle(&glow, 0.0f, (REAL)gy, (REAL)W, (REAL)(60 * k));
                    Pen beam(WithAlpha(rim, (BYTE)(230 * la * glowK)), (REAL)(2 * k));
                    g.DrawLine(&beam, 0.0f, (REAL)scanY, (REAL)W, (REAL)scanY);
                }
            }
            // рамки видоискателя по углам: въезжают из-за краёв, при выключении уезжают
            {
                double t = on_ ? EaseOutBack(el / 520, 1.1) * fadeOut : 1 - Smooth(el / 700);
                if (t > 0.01) {
                    double off = (1 - std::min(t, 1.0)) * 40 * k, m = 26 * k + off, len = 64 * k;
                    Pen br(WithAlpha(rim, (BYTE)(220 * std::min(t, 1.0) * glowK)), (REAL)(2.4 * k));
                    br.SetStartCap(LineCapRound);
                    br.SetEndCap(LineCapRound);
                    double xs[2] = {m, W - m}, ys[2] = {m, H - m};
                    for (int i = 0; i < 2; ++i)
                        for (int j = 0; j < 2; ++j) {
                            double x = xs[i], y = ys[j], sx = i ? -1 : 1, sy = j ? -1 : 1;
                            PointF pts[3] = {PointF((REAL)x, (REAL)(y + sy * len)), PointF((REAL)x, (REAL)y),
                                             PointF((REAL)(x + sx * len), (REAL)y)};
                            g.DrawLines(&br, pts, 3);
                        }
                }
            }
            // узлы над плашкой и провода к ним, как в блюпринтах
            const int n = (int)chips_.size();
            Font nf(Family(), (REAL)(12 * k), FontStyleRegular, UnitPixel);
            for (int i = 0; i < n; ++i) {
                // выключение: узлы с тем, что вернули, сначала видны, потом провода втягиваются в плашку
                double outT = Smooth((el - 1150 - i * 70) / 450);
                double appear = on_ ? EaseOutBack((el - 650 - i * 120) / 380, 1.3)
                                    : EaseOutBack((el - 250 - i * 90) / 320, 1.3) * (1 - outT);
                double wire = on_ ? Smooth((el - 520 - i * 120) / 420)
                                  : Smooth((el - 120 - i * 90) / 320) * (1 - Smooth((el - 1100 - i * 70) / 520));
                double na = on_ ? Clamp01((el - 650 - i * 120) / 160) * fadeOut
                                : Clamp01((el - 250 - i * 90) / 160) * (1 - outT);
                if (wire <= 0.001 && appear <= 0.001) continue;
                RectF nb;
                g.MeasureString(chips_[i].c_str(), -1, &nf, PointF(0, 0), &nb);
                double nw = nb.Width + 26 * k, nh = 30 * k;
                // по дуге над плашкой
                double spread = std::min(W * 0.36, (n - 1) * 190 * k), u = n > 1 ? (double)i / (n - 1) - 0.5 : 0;
                double nx = cx + u * 2 * spread, ny = cy - ph / 2 - 120 * k - std::cos(u * kPi) * 46 * k;
                PointF a((REAL)(cx + u * pw * 0.6), (REAL)(cy - ph / 2)), d((REAL)nx, (REAL)(ny + nh / 2));
                PointF b(a.X, (REAL)(a.Y - 60 * k)), c(d.X, (REAL)(d.Y + 60 * k));
                if (wire > 0.001) {
                    const int N = 28;
                    PointF pts[N + 1];
                    int m = std::max(1, (int)std::ceil(N * wire));
                    for (int j = 0; j <= m; ++j) pts[j] = Bez(a, b, c, d, std::min(1.0, (double)j / N));
                    pts[m] = Bez(a, b, c, d, wire);
                    Pen wp(WithAlpha(base, (BYTE)(200 * glowK * std::max(na, 0.3))), (REAL)(2.2 * k));
                    wp.SetStartCap(LineCapRound);
                    wp.SetEndCap(LineCapRound);
                    g.DrawLines(&wp, pts, m + 1);
                    // импульс бежит по проводу к узлу
                    if (on_ && wire >= 1) {
                        double pt = std::fmod((el - i * 150) / 800.0, 1.0);
                        PointF q = Bez(a, b, c, d, pt);
                        double r = 3.4 * k;
                        SolidBrush pb(WithAlpha(rim, (BYTE)(240 * na * glowK)));
                        g.FillEllipse(&pb, (REAL)(q.X - r), (REAL)(q.Y - r), (REAL)(2 * r), (REAL)(2 * r));
                    }
                    // гнездо на плашке
                    SolidBrush sk(WithAlpha(rim, (BYTE)(230 * glowK * std::max(na, 0.3))));
                    g.FillEllipse(&sk, (REAL)(a.X - 3 * k), (REAL)(a.Y - 3 * k), (REAL)(6 * k), (REAL)(6 * k));
                }
                if (appear > 0.001 && na > 0.01) {
                    double sc = std::max(0.0, appear);
                    RectF box((REAL)(nx - nw * sc / 2), (REAL)(ny - nh * sc / 2), (REAL)(nw * sc), (REAL)(nh * sc));
                    GraphicsPath bp;
                    RoundRectPath(bp, box, (float)(7 * k * sc));
                    SolidBrush bg(
                        Color((BYTE)(232 * na), GetRValue(theme_.bg), GetGValue(theme_.bg), GetBValue(theme_.bg)));
                    g.FillPath(&bg, &bp);
                    // шапка узла цветом провода
                    Region clip(&bp);
                    g.SetClip(&clip);
                    SolidBrush hd(WithAlpha(base, (BYTE)(235 * na)));
                    g.FillRectangle(&hd, box.X, box.Y, box.Width, (REAL)(4 * k * sc));
                    g.ResetClip();
                    Pen ed(WithAlpha(rim, (BYTE)(150 * na)), (REAL)(1.1 * k));
                    g.DrawPath(&ed, &bp);
                    if (sc > 0.6) {
                        StringFormat sf;
                        sf.SetAlignment(StringAlignmentCenter);
                        sf.SetLineAlignment(StringAlignmentCenter);
                        sf.SetFormatFlags(StringFormatFlagsNoWrap);
                        SolidBrush tx(Color((BYTE)(250 * na * Clamp01((sc - 0.6) / 0.4)), GetRValue(theme_.fg),
                                            GetGValue(theme_.fg), GetBValue(theme_.fg)));
                        RectF tr = box;
                        tr.Y += (REAL)(2 * k);
                        g.DrawString(chips_[i].c_str(), -1, &nf, tr, &sf, &tx);
                    }
                }
            }
        }

        // ---------- чипы над плашкой (для игры; у Unreal они - узлы) ----------
        if (kind_ == GAME && !chips_.empty()) {
            Font cf(Family(), (REAL)(12 * k), FontStyleRegular, UnitPixel);
            std::vector<double> ws;
            double sum = 0, gap = 8 * k;
            for (const std::wstring& c : chips_) {
                RectF b;
                g.MeasureString(c.c_str(), -1, &cf, PointF(0, 0), &b);
                ws.push_back(b.Width + 30 * k);
            }
            // в два ряда, если не влезает в 70% ширины
            size_t split = chips_.size();
            for (double w : ws) sum += w + gap;
            if (sum > W * 0.7) split = (chips_.size() + 1) / 2;
            double startT = on_ ? 820 : 260;
            for (int rowI = 0; rowI < 2; ++rowI) {
                size_t from = rowI ? split : 0, to = rowI ? chips_.size() : split;
                if (from >= to) continue;
                double rw = 0;
                for (size_t i = from; i < to; ++i) rw += ws[i] + (i > from ? gap : 0);
                double x = cx - rw / 2, ch = 28 * k;
                double y = cy - ph / 2 - 16 * k - ch - (split < chips_.size() && rowI == 0 ? ch + 8 * k : 0);
                for (size_t i = from; i < to; ++i) {
                    double t = (el - startT - i * 95) / 320;
                    double a = Clamp01(t) * fadeOut;
                    if (!on_) a = Clamp01(t) * Clamp01((total - 300 - el) / 300);
                    if (a > 0.01) {
                        double rise = (1 - EaseOut(t)) * 14 * k;
                        RectF r((REAL)x, (REAL)(y + rise), (REAL)ws[i], (REAL)ch);
                        GraphicsPath cp;
                        RoundRectPath(cp, r, (float)(ch / 2));
                        SolidBrush bg(
                            Color((BYTE)(215 * a), GetRValue(theme_.bg), GetGValue(theme_.bg), GetBValue(theme_.bg)));
                        g.FillPath(&bg, &cp);
                        Pen ed(WithAlpha(base, (BYTE)(140 * a)), (REAL)(1 * k));
                        g.DrawPath(&ed, &cp);
                        SolidBrush dot(WithAlpha(on_ ? rim : steel, (BYTE)(255 * a)));
                        g.FillEllipse(&dot, (REAL)(r.X + 11 * k), (REAL)(r.Y + ch / 2 - 3 * k), (REAL)(6 * k),
                                      (REAL)(6 * k));
                        StringFormat sf;
                        sf.SetLineAlignment(StringAlignmentCenter);
                        sf.SetFormatFlags(StringFormatFlagsNoWrap);
                        SolidBrush tx(
                            Color((BYTE)(245 * a), GetRValue(theme_.fg), GetGValue(theme_.fg), GetBValue(theme_.fg)));
                        g.DrawString(chips_[i].c_str(), -1, &cf, RectF(r.X + (REAL)(22 * k), r.Y, r.Width, r.Height),
                                     &sf, &tx);
                    }
                    x += ws[i] + gap;
                }
            }
        }

        // ---------- плашка ----------
        if (alpha > 0.01) {
            RectF pr((REAL)(cx - pw / 2), (REAL)(cy - ph / 2), (REAL)pw, (REAL)ph);
            GraphicsPath pp;
            RoundRectPath(pp, pr, (float)(ph / 2));
            // свет под плашкой
            for (int i = 3; i >= 1; --i) {
                GraphicsPath gp;
                RoundRectPath(gp, Grow(pr, (float)(i * 5 * k)), (float)(ph / 2 + i * 5 * k));
                SolidBrush gb(WithAlpha(base, (BYTE)(18 * alpha * glowK)));
                g.FillPath(&gb, &gp);
            }
            SolidBrush bg(Color((BYTE)(232 * alpha), GetRValue(theme_.bg), GetGValue(theme_.bg), GetBValue(theme_.bg)));
            g.FillPath(&bg, &pp);
            Pen edge(WithAlpha(rim, (BYTE)(210 * alpha)), (REAL)(1.3 * k));
            g.DrawPath(&edge, &pp);
            // сота-значок слева: при выключении игры остывает из цвета темы в сталь
            Color hc = kind_ == GAME && !on_ ? Lerp(accent, steel, el / 900) : base;
            double hx = open < 0.5 ? cx : pr.X + 21 * k;
            GraphicsPath hp;
            hex::Path(hp, hx, cy, 7.5 * k);
            SolidBrush hb(WithAlpha(hc, (BYTE)(255 * alpha)));
            g.FillPath(&hb, &hp);
            // текст проявляется слева направо, по краю бежит светлая полоса
            double reveal = on_ ? Smooth((el - (kind_ == GAME ? 600 : 420)) / 420) : Clamp01(el / 200);
            if (!on_ && el > total - 560) reveal = 1 - Smooth((el - (total - 560)) / 260);
            if (reveal > 0.01 && open > 0.6) {
                RectF tr(pr.X + (REAL)(36 * k), pr.Y, pr.Width - (REAL)(44 * k), pr.Height);
                RectF clipR(tr.X, tr.Y, (REAL)(tr.Width * reveal), tr.Height);
                g.SetClip(clipR);
                StringFormat sf;
                sf.SetLineAlignment(StringAlignmentCenter);
                sf.SetFormatFlags(StringFormatFlagsNoWrap);
                SolidBrush txt(
                    Color((BYTE)(250 * alpha), GetRValue(theme_.fg), GetGValue(theme_.fg), GetBValue(theme_.fg)));
                g.DrawString(title_.c_str(), -1, &font, tr, &sf, &txt);
                g.ResetClip();
                if (reveal < 0.99) {
                    REAL bx = clipR.GetRight();
                    LinearGradientBrush sb(PointF(bx - (REAL)(18 * k), 0), PointF(bx + 1, 0), WithAlpha(rim, 0),
                                           WithAlpha(rim, (BYTE)(200 * alpha)));
                    g.FillRectangle(&sb, bx - (REAL)(18 * k), pr.Y + (REAL)(8 * k), (REAL)(19 * k),
                                    pr.Height - (REAL)(16 * k));
                }
            }
        }
    }
}

void ModeFx::Present(int W, int H) {
    HDC scr = GetDC(nullptr);
    POINT pos{mon_.left, mon_.top}, src{0, 0};
    SIZE size{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &pos, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ModeFx::Stop() {
    if (wnd_) {
        KillTimer(wnd_, kTimer);
        ShowWindow(wnd_, SW_HIDE);
    }
    if (memDC_) {
        SelectObject(memDC_, old_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
        dib_ = nullptr;
        bits_ = nullptr;
    }
}

// Лист кадров без экрана: кадры поверх тёмно-серого, каждый уменьшен вдвое.
int ModeFx::Film(Kind kind, bool on, const std::wstring& title, const std::vector<std::wstring>& chips,
                 const ThemeInfo& theme, const std::wstring& out) {
    ModeFx fx;
    fx.kind_ = kind;
    fx.on_ = on;
    fx.title_ = title;
    fx.chips_ = chips;
    fx.theme_ = theme;
    const int W = 1600, H = 1000, cols = 3;
    const double k = 1.25;
    std::vector<double> at;
    for (double t = 150; t < fx.Total(); t += fx.Total() / 9) at.push_back(t);
    int rows = (int)(at.size() + cols - 1) / cols, cw = W / 2, ch = H / 2;
    Bitmap sheet(cw * cols, ch * rows, PixelFormat32bppARGB);
    Graphics sg(&sheet);
    sg.Clear(Color(255, 46, 44, 52));
    sg.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    std::vector<BYTE> bits((size_t)W * H * 4);
    for (size_t i = 0; i < at.size(); ++i) {
        fx.Draw(bits.data(), W, H, k, at[i]);
        Bitmap frame(W, H, W * 4, PixelFormat32bppPARGB, bits.data());
        int x = (int)(i % cols) * cw, y = (int)(i / cols) * ch;
        SolidBrush back(Color(255, 58, 62, 74));
        sg.FillRectangle(&back, x + 2, y + 2, cw - 4, ch - 4);
        sg.DrawImage(&frame, x + 2, y + 2, cw - 4, ch - 4);
        Font f(L"Segoe UI", 14, FontStyleRegular, UnitPixel);
        SolidBrush lb(Color(200, 255, 255, 255));
        wchar_t s[32];
        swprintf(s, 32, L"%d мс", (int)at[i]);
        sg.DrawString(s, -1, &f, PointF((REAL)x + 8, (REAL)y + 6), &lb);
    }
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png"))
            return sheet.Save(out.c_str(), &codecs[i].Clsid, nullptr) == Ok ? 0 : 1;
    return 1;
}
