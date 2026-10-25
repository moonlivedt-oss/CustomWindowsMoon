#include "desk/toast.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/fx.h"
#include "core/mem.h"
#include "desk/share.h"
#include "core/night.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

std::vector<Toast*>& All() {
    static std::vector<Toast*> all;
    return all;
}
ULONGLONG g_order = 0;

// плавное приближение к цели за время tau (мс), не зависит от частоты кадров
inline float Approach(float v, float target, double dt, double tau) {
    return target + (v - target) * (float)std::exp(-dt / tau);
}

double Lum(const Color& c) { return (0.299 * c.GetR() + 0.587 * c.GetG() + 0.114 * c.GetB()) / 255; }

}  // namespace

// ---------------- окно ----------------

void Toast::CreateWnd(HINSTANCE inst, const wchar_t* cls, const wchar_t* title) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, cls, title, WS_POPUP, 0,
                           0, 1, 1, nullptr, nullptr, inst, this);
    All().push_back(this);
}

void Toast::Destroy() {
    auto& all = All();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (wnd_) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

void Toast::Open(int holdMs, bool onCursor) {
    POINT pt{0, 0};
    if (onCursor) GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, onCursor ? MONITOR_DEFAULTTONEAREST : MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    bool moved = !EqualRect(&work_, &mi.rcWork) || k_ != dx / 96.0;
    work_ = mi.rcWork;
    k_ = dx / 96.0;
    if (appear_ <= 0.001f || moved) {
        // новая карточка встаёт над уже открытыми
        order_ = ++g_order;
        yInit_ = false;
        appear_ = appearV_ = 0;
        dragX_ = 0;
    } else {
        pulse_ = 1;  // уже на экране: короткий толчок вместо повторного въезда
    }
    if (!open_) last_ = GetTickCount64();
    open_ = true;
    hold_ = left_ = holdMs > 0 ? holdMs : 0;
    dirty_ = true;
    fx::Wake();  // въезд идёт от тактов опроса
}

void Toast::Dismiss() {
    if (!open_) return;
    open_ = false;
    Closed();
}

// ---------------- стопка ----------------

float Toast::Presence() const { return std::clamp(open_ ? std::max(appear_, 0.35f) : appear_, 0.0f, 1.0f); }

int Toast::TargetY() const {
    float y = 14 * k();
    for (const Toast* o : All())
        if (o != this && o->order_ < order_ && (o->open_ || o->appear_ > 0) && EqualRect(&o->work_, &work_))
            y += (o->h_ + 10) * o->k() * o->Presence();
    return (int)std::lround(y);
}

// ---------------- кадр ----------------

void Toast::Tick() {
    ULONGLONG now = GetTickCount64();
    double dt = std::min<double>(100, (double)(now - last_));
    last_ = now;
    if (!wnd_ || (!open_ && appear_ <= 0)) return;
    fx::Active();  // карточка на экране: таймер ухода и пружина идут от тактов опроса

    bool anim = false;
    // таймер стоит, пока на карточку смотрят или тянут её
    if (open_ && hold_ > 0 && hot_ == H_NONE && !dragging_) {
        left_ -= dt;
        if (left_ <= 0) {
            open_ = false;
            Closed();
        }
    }
    // въезд - пружина с лёгким перелётом, уход - быстрое растворение
    float before = appear_;
    if (open_) {
        const double w = 2 * 3.14159265 / 0.46, z = 0.62;
        for (double t = dt / 1000; t > 0; t -= 0.008) {
            double h = std::min(0.008, t);
            appearV_ += (float)((w * w * (1 - appear_) - 2 * z * w * appearV_) * h);
            appear_ += (float)(appearV_ * h);
        }
        if (std::fabs(1 - appear_) < 0.0015f && std::fabs(appearV_) < 0.01f) appear_ = 1, appearV_ = 0;
    } else {
        appearV_ = 0;
        appear_ = std::max(0.0f, appear_ - (float)(dt / 190));
    }
    anim |= appear_ != before;
    for (int i = 0; i < kMaxHits; ++i) {
        float t = i == hot_ ? 1.0f : 0.0f;
        if (hover_[i] == t) continue;
        hover_[i] = std::fabs(hover_[i] - t) < 0.01f ? t : Approach(hover_[i], t, dt, i == hot_ ? 45 : 90);
        anim = true;
    }
    if (pulse_ > 0) pulse_ = std::max(0.0f, pulse_ - (float)(dt / 420)), anim = true;
    if (!dragging_ && open_ && dragX_ > 0) {  // не добросили - пружиной назад
        dragX_ = dragX_ < 0.5f ? 0 : Approach(dragX_, 0, dt, 60);
        anim = true;
    }
    if (!open_ && dragX_ > 0) dragX_ += (float)(dt * 2.2 * k()), anim = true;  // брошенная улетает дальше
    float ty = (float)TargetY();
    if (!yInit_) y_ = ty, yInit_ = true;
    if (std::fabs(y_ - ty) > 0.4f)
        y_ = Approach(y_, ty, dt, 85), anim = true;
    else
        y_ = ty;

    if (appear_ <= 0 && !open_) {
        if (IsWindowVisible(wnd_)) {
            ShowWindow(wnd_, SW_HIDE);
            mem::TrimSoon();
        }
        dragX_ = 0;
        return;
    }
    // полоска таймера двигается медленно: хватает кадра в 80 мс
    bool timer = open_ && hold_ > 0 && now - lastTimerFrame_ >= 80;
    if (anim || dirty_ || Busy() || timer) Render();
}

RectF Toast::Body() const {
    float m = kM * k();
    return RectF(m, m, kW * k(), h_ * k());
}

Color Toast::Tone() const { return ToColor(theme_.ac); }

void Toast::Render() {
    lastTimerFrame_ = GetTickCount64();
    float K = k();
    if (dirty_) {
        static Bitmap* probe = new Bitmap(1, 1, PixelFormat32bppPARGB);
        Graphics mg(probe);
        mg.SetTextRenderingHint(TextRenderingHintAntiAlias);
        h_ = std::max(64.0f, BodyHeight(mg));
    }
    dirty_ = false;
    int W = (int)std::ceil((kW + kM * 2) * K), H = (int)std::ceil((h_ + kM * 2) * K);
    HDC scr = GetDC(nullptr);
    if (!memDC_ || W != dibW_ || H != dibH_) {
        if (memDC_) {
            SelectObject(memDC_, oldBmp_);
            DeleteObject(dib_);
            DeleteDC(memDC_);
        }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
        memDC_ = CreateCompatibleDC(scr);
        dib_ = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        oldBmp_ = SelectObject(memDC_, dib_);
        dibW_ = W, dibH_ = H;
    }
    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), bg = ToColor(theme_.bg), tone = Tone();
        RectF body = Body();
        float rad = 16 * K;

        // тень: мягкие слои, гуще книзу
        for (int i = 7; i >= 1; --i) {
            GraphicsPath sp;
            RoundRectPath(sp,
                          RectF(body.X - i * 1.6f * K, body.Y - i * 1.0f * K + 7 * K, body.Width + i * 3.2f * K,
                                body.Height + i * 2.4f * K),
                          rad + i * 1.6f * K);
            SolidBrush sb(Color((BYTE)(7 + (7 - i) * 3), 0, 0, 0));
            g.FillPath(&sb, &sp);
        }
        GraphicsPath path;
        RoundRectPath(path, body, rad);
        Color base = night::On() ? MixColor(bg, Color(0, 0, 0), 0.35) : bg;
        SolidBrush fill(WithAlpha(MixColor(base, Color(0, 0, 0), 0.12), 250));
        g.FillPath(&fill, &path);
        {
            GraphicsState st = g.Save();
            g.SetClip(&path, CombineModeIntersect);
            // свет знака слева и блик сверху
            LinearGradientBrush glow(PointF(body.X - 1, 0), PointF(body.X + body.Width * 0.55f + 1, 0), WithAlpha(tone, 34),
                                     WithAlpha(tone, 0));
            g.FillRectangle(&glow, RectF(body.X, body.Y, body.Width * 0.55f, body.Height));
            LinearGradientBrush shine(PointF(0, body.Y - 1), PointF(0, body.Y + 36 * K + 1), Color(18, 255, 255, 255),
                                      Color(0, 255, 255, 255));
            g.FillRectangle(&shine, RectF(body.X, body.Y, body.Width, 36 * K));
            // полоска тона слева
            SolidBrush strip(tone);
            g.FillRectangle(&strip, RectF(body.X, body.Y, 3.5f * K, body.Height));
            // время до ухода или бегущий блик работы
            float bh = 2.5f * K, by = body.GetBottom() - bh;
            if (Busy()) {
                double t = (GetTickCount64() % 1300) / 1300.0;
                float sw = body.Width * 0.32f, sx = body.X - sw + (float)(t * (body.Width + sw));
                LinearGradientBrush run(PointF(sx - 1, 0), PointF(sx + sw + 1, 0), WithAlpha(tone, 0),
                                        WithAlpha(tone, 0));
                Color cs[3] = {WithAlpha(tone, 0), WithAlpha(tone, 230), WithAlpha(tone, 0)};
                REAL ps[3] = {0, 0.5f, 1};
                run.SetInterpolationColors(cs, ps, 3);
                g.FillRectangle(&run, RectF(sx, by, sw, bh));
            } else if (open_ && hold_ > 0) {
                float f = (float)std::clamp(left_ / hold_, 0.0, 1.0);
                SolidBrush track(Color(16, 255, 255, 255));
                g.FillRectangle(&track, RectF(body.X, by, body.Width, bh));
                SolidBrush bar(WithAlpha(tone, hot_ != H_NONE ? 110 : 190));
                g.FillRectangle(&bar, RectF(body.X, by, body.Width * f, bh));
            }
            g.Restore(st);
        }
        Pen rim(Color(36, 255, 255, 255), 1 * K);
        g.DrawPath(&rim, &path);
        if (pulse_ > 0) {  // обновили открытую карточку - кромка вспыхивает тоном
            Pen pp(WithAlpha(tone, (BYTE)(200 * pulse_)), 1.6f * K);
            g.DrawPath(&pp, &path);
        }

        Paint(g, body);

        // закрыть: виден ярче, когда курсор на карточке
        RectF cr(body.GetRight() - 34 * K, body.Y + 10 * K, 24 * K, 24 * K);
        float onCard = hot_ != H_NONE ? 1.0f : 0.0f;
        if (hover_[H_CLOSE] > 0) {
            SolidBrush hb(Color((BYTE)(34 * hover_[H_CLOSE]), 255, 255, 255));
            g.FillEllipse(&hb, cr);
        }
        Pen xp(MixColor(muted, fg, std::max(hover_[H_CLOSE], onCard * 0.3f), (BYTE)(150 + 105 * onCard)), 1.5f * K);
        xp.SetStartCap(LineCapRound);
        xp.SetEndCap(LineCapRound);
        float c = 4.5f * K, cx = cr.X + cr.Width / 2, cy = cr.Y + cr.Height / 2;
        g.DrawLine(&xp, cx - c, cy - c, cx + c, cy + c);
        g.DrawLine(&xp, cx - c, cy + c, cx + c, cy - c);
    }
    float a = std::clamp(appear_, 0.0f, 1.0f);
    float slide = (1 - appear_) * 64 * K + dragX_;  // перелёт пружины даёт лёгкий заход влево
    LONG x = work_.right - W + (LONG)(kM * K) - (LONG)(14 * K) + (LONG)std::lround(slide);
    LONG y = work_.bottom - H + (LONG)(kM * K) - (LONG)std::lround(y_);
    float fade = dragX_ > 0 ? std::max(0.0f, 1 - dragX_ / (220 * K)) : 1.0f;
    POINT at{x, y}, src{0, 0};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(255 * std::min(1.0f, a * 1.5f) * fade), AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &at, &sz, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) {
        share::Cover(wnd_);  // в показе экрана карточку видите только вы
        ShowWindow(wnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
}

// ---------------- помощники рисования ----------------

void Toast::DrawGlyph(Graphics& g, float cx, float cy, Glyph glyph, Color c) const {
    if (glyph == G_NONE) return;
    float K = k(), r = 17 * K;
    SolidBrush disc(WithAlpha(c, 46));
    g.FillEllipse(&disc, cx - r, cy - r, 2 * r, 2 * r);
    Pen ring(WithAlpha(c, 110), 1 * K);
    g.DrawEllipse(&ring, cx - r, cy - r, 2 * r, 2 * r);
    Pen p(MixColor(c, Color(255, 255, 255), 0.35), 2.1f * K);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    SolidBrush dot(MixColor(c, Color(255, 255, 255), 0.35));
    float s = 6 * K;
    switch (glyph) {
        case G_OK: {
            PointF pts[3] = {{cx - s, cy + 0.2f * K}, {cx - s * 0.3f, cy + s * 0.7f}, {cx + s, cy - s * 0.7f}};
            g.DrawLines(&p, pts, 3);
            break;
        }
        case G_FAIL:
            g.DrawLine(&p, cx - s * 0.8f, cy - s * 0.8f, cx + s * 0.8f, cy + s * 0.8f);
            g.DrawLine(&p, cx - s * 0.8f, cy + s * 0.8f, cx + s * 0.8f, cy - s * 0.8f);
            break;
        case G_WARN:
        case G_INFO: {
            bool warn = glyph == G_WARN;
            float top = warn ? cy - s : cy - s * 0.15f, bot = warn ? cy + s * 0.25f : cy + s;
            g.DrawLine(&p, cx, top, cx, bot);
            float dy = warn ? cy + s * 0.95f : cy - s * 0.95f, dr = 1.35f * K;
            g.FillEllipse(&dot, cx - dr, dy - dr, 2 * dr, 2 * dr);
            break;
        }
        case G_ASK: {
            g.DrawArc(&p, cx - s * 0.75f, cy - s * 1.15f, s * 1.5f, s * 1.3f, 180, 230);
            g.DrawLine(&p, cx + s * 0.18f, cy - s * 0.05f, cx, cy + s * 0.35f);
            float dr = 1.35f * K;
            g.FillEllipse(&dot, cx - dr, cy + s * 0.95f - dr, 2 * dr, 2 * dr);
            break;
        }
        case G_BUSY: {
            double a = (GetTickCount64() % 1000) / 1000.0 * 360;
            Pen faint(WithAlpha(c, 70), 2.1f * K);
            g.DrawEllipse(&faint, cx - s, cy - s, 2 * s, 2 * s);
            g.DrawArc(&p, cx - s, cy - s, 2 * s, 2 * s, (REAL)a, 100);
            break;
        }
        case G_NET: {
            Pen thin(MixColor(c, Color(255, 255, 255), 0.35), 1.6f * K);
            g.DrawEllipse(&thin, cx - s, cy - s, 2 * s, 2 * s);
            g.DrawEllipse(&thin, cx - s * 0.42f, cy - s, s * 0.84f, 2 * s);
            g.DrawLine(&thin, cx - s, cy, cx + s, cy);
            break;
        }
        case G_RESTORE: {
            g.DrawArc(&p, cx - s, cy - s, 2 * s, 2 * s, 200, 290);
            float ax = cx + s * (float)std::cos(Rad(200)), ay = cy + s * (float)std::sin(Rad(200));
            g.DrawLine(&p, ax, ay, ax - 0.5f * K, ay - 4.2f * K);
            g.DrawLine(&p, ax, ay, ax + 4.0f * K, ay - 0.8f * K);
            break;
        }
        default:
            break;
    }
}

float Toast::ButtonWidth(Graphics& g, const std::wstring& label) const {
    Font f(Family(), 12.5f * k(), FontStyleRegular, UnitPixel);
    RectF b;
    g.MeasureString(label.c_str(), -1, &f, PointF(0, 0), StringFormat::GenericTypographic(), &b);
    return std::max(84 * k(), b.Width + 34 * k());
}

void Toast::DrawButton(Graphics& g, const RectF& rr, const std::wstring& label, bool primary, int id) const {
    float K = k(), hv = HoverOf(id);
    RectF r = rr;
    if (Pressed(id)) r = Inset(r, 1.2f * K, 1.0f * K);
    GraphicsPath bp;
    RoundRectPath(bp, r, r.Height / 2);
    Color ac = ToColor(theme_.ac), fg = ToColor(theme_.fg);
    if (primary) {
        LinearGradientBrush bb(PointF(0, r.Y), PointF(0, r.GetBottom()),
                               MixColor(ac, Color(255, 255, 255), 0.10 + hv * 0.14),
                               MixColor(ac, Color(0, 0, 0), 0.08 - hv * 0.04));
        g.FillPath(&bb, &bp);
        Pen hi(Color((BYTE)(50 + 40 * hv), 255, 255, 255), 1 * K);
        g.DrawPath(&hi, &bp);
    } else {
        SolidBrush bb(Color((BYTE)(20 + 26 * hv), 255, 255, 255));
        g.FillPath(&bb, &bp);
        Pen bpn(Color((BYTE)(44 + 30 * hv), 255, 255, 255), 1 * K);
        g.DrawPath(&bpn, &bp);
    }
    Font f(Family(), 12.5f * K, FontStyleRegular, UnitPixel);
    SolidBrush lb(primary ? (Lum(ac) > 0.72 ? Color(255, 24, 20, 28) : Color(255, 255, 255, 255)) : fg);
    StringFormat cf;
    cf.SetAlignment(StringAlignmentCenter);
    cf.SetLineAlignment(StringAlignmentCenter);
    cf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(label.c_str(), -1, &f, r, &cf, &lb);
}

float Toast::TextHeight(Graphics& g, const std::wstring& s, const Font& f, float width) const {
    if (s.empty()) return 0;
    RectF b;
    g.MeasureString(s.c_str(), -1, &f, RectF(0, 0, width, 1000), nullptr, &b);
    return b.Height;
}

// ---------------- мышь ----------------

int Toast::HitAt(POINT p) const {
    PointF pt((REAL)p.x, (REAL)p.y);
    RectF body = Body();
    RectF cr(body.GetRight() - 36 * k(), body.Y + 8 * k(), 28 * k(), 28 * k());
    if (cr.Contains(pt)) return H_CLOSE;
    int h = HitContent(pt);
    if (h != H_NONE) return h;
    return body.Contains(pt) ? H_BODY : H_NONE;
}

LRESULT CALLBACK Toast::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* self = (Toast*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self && self->wnd_ == h) return self->Handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT Toast::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    auto close = [this] { Dismiss(); };
    if (Message(msg, wp, lp)) return 0;
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // не отнимать фокус у игры
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, dragging_                          ? IDC_SIZEWE
                                           : hot_ == H_BODY || hot_ == H_NONE ? IDC_ARROW
                                                                              : IDC_HAND));
            return TRUE;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, wnd_, 0};
                TrackMouseEvent(&t);
                tracking_ = true;
            }
            if (down_ != H_NONE && (wp & MK_LBUTTON) && open_) {
                POINT s;
                GetCursorPos(&s);
                float dx = s.x - dragStart_;
                if (!dragging_ && dx > 8 * k()) dragging_ = true;
                if (dragging_) {
                    dragX_ = std::max(0.0f, dx);
                    return 0;
                }
            }
            int h = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            hot_ = h;
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!dragging_) hot_ = H_NONE;
            return 0;
        case WM_LBUTTONDOWN: {
            down_ = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            POINT s;
            GetCursorPos(&s);
            dragStart_ = (float)s.x;
            SetCapture(wnd_);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int d = down_;
            down_ = H_NONE;
            if (dragging_) {
                dragging_ = false;
                if (dragX_ > 90 * k()) close();  // бросили вправо
                return 0;
            }
            int h = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            if (h != d || h == H_NONE) return 0;
            if (h == H_CLOSE)
                close();
            else
                Click(h);
            return 0;
        }
        case WM_RBUTTONUP:
            close();
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging_) dragging_ = false;
            return 0;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}
