#include "desk/boostcard.h"

#include <shellscalingapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#include "core/mem.h"
#include "core/night.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/hex.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialBoost";
constexpr float kW = 390, kH = 138, kM = 26;  // карточка и поля под тень, до масштаба
BoostCard* g_self = nullptr;

}  // namespace

void BoostCard::Create(HINSTANCE inst) {
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClass, L"Boost",
                           WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
}

void BoostCard::Destroy() {
    if (wnd_) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

void BoostCard::Show(Mode mode, const std::wstring& title, const std::wstring& sub, const ThemeInfo& theme) {
    MONITORINFO mi{sizeof(mi)};
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    work_ = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    k_ = dx / 96.0;
    mode_ = mode;
    title_ = title;
    sub_ = sub;
    theme_ = theme;
    open_ = true;
    // вопрос висит дольше, итог - коротко
    until_ = GetTickCount64() + (mode == DONE ? 4500 : mode == WORKING ? 60000 : 16000);
    dirty_ = true;
}

void BoostCard::Hide() { open_ = false; }

RectF BoostCard::Rect(Hit h) const {
    float k = (float)k_, m = kM * k;
    float right = m + (kW - 16) * k;
    switch (h) {
        case H_CLOSE:
            return RectF(right - 26 * k, m + 12 * k, 26 * k, 26 * k);
        case H_YES:
            return RectF(right - 254 * k, m + 92 * k, 134 * k, 34 * k);
        case H_NO:
            return RectF(right - 112 * k, m + 92 * k, 112 * k, 34 * k);
        case H_BODY:
            return RectF(m, m, kW * k, kH * k);
        default:
            return RectF();
    }
}

BoostCard::Hit BoostCard::HitAt(POINT p) const {
    PointF pt((REAL)p.x, (REAL)p.y);
    bool buttons = mode_ == OFFER || mode_ == RESTORE;
    for (Hit h : {H_CLOSE, H_YES, H_NO})
        if ((h == H_CLOSE || buttons) && Rect(h).Contains(pt)) return h;
    return Rect(H_BODY).Contains(pt) ? H_BODY : H_NONE;
}

void BoostCard::Tick() {
    if (!wnd_ || (slide_ <= 0 && !open_)) return;
    ULONGLONG now = GetTickCount64();
    if (hot_ != H_NONE) until_ = std::max(until_, now + 2500);  // пока на карточку смотрят, она не уходит
    if (open_ && now > until_) open_ = false;
    bool anim = false;
    // карточка - знак и в игровом режиме, поэтому шаг свой, а не fx::Step (там всё мгновенно)
    auto step = [&](float& v, float t, float up, float down) {
        if (v == t) return;
        v = v < t ? std::min(t, v + up) : std::max(t, v - down);
        anim = true;
    };
    step(slide_, open_ ? 1.0f : 0.0f, 0.09f, 0.1f);
    for (int i = 0; i < kHits; ++i) step(hover_[i], i == hot_ ? 1.0f : 0.0f, 0.2f, 0.14f);
    if (slide_ <= 0) {
        if (IsWindowVisible(wnd_)) {
            ShowWindow(wnd_, SW_HIDE);
            mem::TrimSoon();
        }
        return;
    }
    if (anim || dirty_ || mode_ == WORKING) Render();
}

LRESULT CALLBACK BoostCard::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_self && g_self->wnd_ == h) return g_self->Handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT BoostCard::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // не отнимать фокус у игры
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, hot_ == H_BODY || hot_ == H_NONE ? IDC_ARROW : IDC_HAND));
            return TRUE;
        case WM_MOUSEMOVE:
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, wnd_, 0};
                TrackMouseEvent(&t);
                tracking_ = true;
            }
            hot_ = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            hot_ = H_NONE;
            return 0;
        case WM_LBUTTONDOWN:
            down_ = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            return 0;
        case WM_LBUTTONUP: {
            Hit h = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            if (h != down_) return 0;
            if (h == H_CLOSE || h == H_NO) open_ = false;
            if (h == H_YES && onYes) onYes();
            dirty_ = true;
            return 0;
        }
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}

void BoostCard::Render() {
    dirty_ = false;
    float k = (float)k_;
    int W = (int)std::ceil((kW + kM * 2) * k), H = (int)std::ceil((kH + kM * 2) * k);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), bg = ToColor(theme_.bg), ac = ToColor(theme_.ac);
        RectF body = Rect(H_BODY);
        float rad = 18 * k;
        for (int i = 6; i >= 1; --i) {  // мягкая тень
            GraphicsPath sp;
            RoundRectPath(sp,
                          RectF(body.X - i * 2 * k, body.Y - i * 1.5f * k + 6 * k, body.Width + i * 4 * k,
                                body.Height + i * 3 * k),
                          rad + i * 2 * k);
            SolidBrush sb(Color((BYTE)(9 + (6 - i) * 3), 0, 0, 0));
            g.FillPath(&sb, &sp);
        }
        GraphicsPath path;
        RoundRectPath(path, body, rad);
        SolidBrush fill(WithAlpha(night::On() ? MixColor(bg, Color(0, 0, 0), 0.35) : bg, 246));
        g.FillPath(&fill, &path);
        LinearGradientBrush tint(PointF(body.X, 0), PointF(body.X + body.Width, 0), WithAlpha(ac, 30),
                                 WithAlpha(ac, 0));
        g.FillPath(&tint, &path);
        // грани сот на стекле карточки, как у кольца
        {
            GraphicsState st = g.Save();
            g.SetClip(&path, CombineModeIntersect);
            Pen line(Color(16, 255, 255, 255), 1.0f);
            double s = 13 * k;
            hex::ForEach(body.X, body.Y, body.GetRight(), body.GetBottom(), s, [&](const hex::Cell& c) {
                GraphicsPath p;
                hex::Path(p, c.x, c.y, s, 0.94);
                g.DrawPath(&line, &p);
            });
            g.Restore(st);
        }
        Pen rim(Color(40, 255, 255, 255), 1 * k);
        g.DrawPath(&rim, &path);

        // значок: сота цвета акцента; во время работы по ней бежит искра
        float ix = body.X + 30 * k, iy = body.Y + 32 * k;
        GraphicsPath hp;
        hex::Path(hp, ix, iy, 12 * k);
        SolidBrush hb(mode_ == RESTORE ? MixColor(muted, fg, 0.3) : ac);
        g.FillPath(&hb, &hp);
        if (mode_ == WORKING) {
            double a = (GetTickCount64() % 900) / 900.0 * 360;
            Pen spin(Color(230, 255, 255, 255), 2.2f * k);
            spin.SetStartCap(LineCapRound);
            spin.SetEndCap(LineCapRound);
            g.DrawArc(&spin, ix - 17 * k, iy - 17 * k, 34 * k, 34 * k, (REAL)a, 70);
        }

        FontFamily fam(L"Segoe UI");
        Font fT(Family(), 14.5f * k, FontStyleRegular, UnitPixel);
        Font fS(&fam, 11.5f * k, FontStyleRegular, UnitPixel);
        Font fB(Family(), 12 * k, FontStyleRegular, UnitPixel);
        StringFormat sf;
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        float x0 = body.X + 56 * k, tw = body.GetRight() - 46 * k - x0;
        SolidBrush tB(fg), mB(muted);
        g.DrawString(title_.c_str(), -1, &fT, RectF(x0, body.Y + 18 * k, tw, 24 * k), &sf, &tB);
        StringFormat wf;
        wf.SetTrimming(StringTrimmingEllipsisWord);
        g.DrawString(sub_.c_str(), -1, &fS, RectF(x0, body.Y + 44 * k, body.GetRight() - 18 * k - x0, 40 * k), &wf,
                     &mB);

        if (mode_ == OFFER || mode_ == RESTORE) {
            for (Hit h : {H_YES, H_NO}) {
                RectF r = Rect(h);
                float hv = hover_[h];
                GraphicsPath bp;
                RoundRectPath(bp, r, r.Height / 2);
                bool primary = h == H_YES;
                SolidBrush bb(primary ? MixColor(ac, Color(255, 255, 255), hv * 0.15)
                                      : Color((BYTE)(26 + 30 * hv), 255, 255, 255));
                g.FillPath(&bb, &bp);
                if (!primary) {
                    Pen bpn(Color(50, 255, 255, 255), 1 * k);
                    g.DrawPath(&bpn, &bp);
                }
                std::wstring label = h == H_YES ? (mode_ == OFFER ? L"Освободить" : L"Вернуть")
                                                : (mode_ == OFFER ? L"Не сейчас" : L"Не нужно");
                double lum = (0.299 * ac.GetR() + 0.587 * ac.GetG() + 0.114 * ac.GetB()) / 255;
                SolidBrush lb(primary ? (lum > 0.72 ? Color(255, 24, 20, 28) : Color(255, 255, 255, 255)) : fg);
                StringFormat cf;
                cf.SetAlignment(StringAlignmentCenter);
                cf.SetLineAlignment(StringAlignmentCenter);
                g.DrawString(label.c_str(), -1, &fB, r, &cf, &lb);
            }
        }
        RectF cr = Rect(H_CLOSE);
        if (hover_[H_CLOSE] > 0) {
            SolidBrush hb2(Color((BYTE)(36 * hover_[H_CLOSE]), 255, 255, 255));
            g.FillEllipse(&hb2, cr);
        }
        Pen xp(MixColor(muted, fg, hover_[H_CLOSE]), 1.6f * k);
        xp.SetStartCap(LineCapRound);
        xp.SetEndCap(LineCapRound);
        float c = 5 * k, cx = cr.X + cr.Width / 2, cy = cr.Y + cr.Height / 2;
        g.DrawLine(&xp, cx - c, cy - c, cx + c, cy + c);
        g.DrawLine(&xp, cx - c, cy + c, cx + c, cy - c);
    }
    // справа снизу; спрятанная стоит верхом на нижнем краю рабочей области
    float e = (float)EaseOut(slide_);
    LONG x = work_.right - W + (LONG)(kM * k) - (LONG)(14 * k);
    LONG yShown = work_.bottom - H + (LONG)(kM * k) - (LONG)(14 * k), yHidden = work_.bottom - (LONG)(kM * k);
    POINT at{x, (LONG)(yHidden + (yShown - yHidden) * e)};
    POINT src{0, 0};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(255 * std::min(1.0f, e * 1.4f)), AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &at, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}
