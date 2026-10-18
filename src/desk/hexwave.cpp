#include "desk/hexwave.h"

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

constexpr wchar_t kClass[] = L"MoonRadialHexWave";
constexpr UINT_PTR kTimer = 1;
// длительность фиксированная: в игровом режиме общая скорость анимаций «мгновенно», а волна и есть знак
constexpr double kWaveMs = 1600, kLabelMs = 2100;
HexWave* g_self = nullptr;

}  // namespace

void HexWave::Create(HINSTANCE inst) {
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
}

void HexWave::Destroy() {
    Stop();
    if (wnd_) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

LRESULT CALLBACK HexWave::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_TIMER && wp == kTimer && g_self) {
        g_self->Frame();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void HexWave::Play(bool entering, const std::wstring& label, const ThemeInfo& theme) {
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
    entering_ = entering;
    label_ = label;
    theme_ = theme;
    t0_ = GetTickCount64();
    Frame();
    SetTimer(wnd_, kTimer, 16, nullptr);
}

void HexWave::Frame() {
    double el = (double)(GetTickCount64() - t0_);
    if (el >= std::max(kWaveMs, kLabelMs)) {
        Stop();
        return;
    }
    const int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(MonitorFromPoint({mon_.left, mon_.top}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dpi, &dy);
    const double k = dpi / 96.0;
    memset(bits_, 0, (size_t)W * H * 4);
    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        // включение - акцентом, выключение - сталью
        Color base = entering_ ? ToColor(theme_.ac) : MixColor(ToColor(theme_.muted), ToColor(theme_.fg), 0.3);
        Color rim = MixColor(base, Color(255, 255, 255), 0.4);
        float glowK = night::Glow();

        double t = Clamp01(el / kWaveMs);
        if (t < 1) {
            // фронт идёт по периметру от середины нижнего края в обе стороны и встречается наверху
            const double P = 2.0 * (W + H), half = P / 2, front = (t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t)) * half * 1.04;
            const double s = 14 * k, band = 90 * k, wv = 220 * k;
            const double fade = t < 0.75 ? 1 : (1 - t) / 0.25;
            auto perim = [&](double x, double y) {
                // позиция ближайшей точки края по периметру, отсчёт от середины низа по часовой
                double dl = x, dr = W - x, dt = y, db = H - y, m = std::min({dl, dr, dt, db});
                double p;
                if (m == db)
                    p = W / 2.0 - x;  // низ: влево от середины
                else if (m == dl)
                    p = W / 2.0 + (H - y);
                else if (m == dt)
                    p = W / 2.0 + H + x;
                else
                    p = W / 2.0 + H + W + y;
                if (p < 0) p += P;
                return std::pair<double, double>(std::min(p, P - p), m);  // путь от старта, глубина от края
            };
            auto cellAt = [&](const hex::Cell& c) {
                auto [pd, depth] = perim(c.x, c.y);
                double d = (pd - front) / wv;
                double lit = std::exp(-d * d);
                if (pd < front) lit = std::max(lit, 0.28 * std::exp((pd - front) / (600 * k)));
                double reach = band * (0.35 + 0.65 * std::exp(-d * d));  // у фронта соты встают глубже
                double a = lit * std::max(0.0, 1 - depth / reach) * fade * glowK;
                if (a < 0.02) return;
                GraphicsPath p;
                hex::Path(p, c.x, c.y, s, 0.84);
                SolidBrush fill(WithAlpha(base, (BYTE)std::min(255.0, 170 * a)));
                g.FillPath(&fill, &p);
                Pen edge(WithAlpha(rim, (BYTE)std::min(255.0, 235 * a)), (REAL)(1.3 * k));
                g.DrawPath(&edge, &p);
            };
            // только полосы вдоль краёв, середину экрана не перебираем
            hex::ForEach(0, 0, W, band + s, s, cellAt);
            hex::ForEach(0, H - band - s, W, H, s, cellAt);
            hex::ForEach(0, band + s, band + s, H - band - s, s, cellAt);
            hex::ForEach(W - band - s, band + s, W, H - band - s, s, cellAt);
        }

        // плашка внизу посередине
        double lt = el / kLabelMs;
        double la = std::min(Clamp01(lt / 0.12), Clamp01((1 - lt) / 0.25));
        if (la > 0.01 && !label_.empty()) {
            Font font(Family(), (REAL)(13 * k), FontStyleRegular, UnitPixel);
            RectF tb;
            g.MeasureString(label_.c_str(), -1, &font, PointF(0, 0), &tb);
            double ph = 34 * k, pw = tb.Width + 44 * k;
            double rise = 10 * k * (1 - EaseOut(Clamp01(lt / 0.2)));
            RectF pr((REAL)((W - pw) / 2), (REAL)(H - 84 * k - ph + rise), (REAL)pw, (REAL)ph);
            GraphicsPath pp;
            RoundRectPath(pp, pr, (float)(ph / 2));
            SolidBrush bg(Color((BYTE)(225 * la), GetRValue(theme_.bg), GetGValue(theme_.bg), GetBValue(theme_.bg)));
            g.FillPath(&bg, &pp);
            Pen edge(WithAlpha(rim, (BYTE)(200 * la)), (REAL)(1.2 * k));
            g.DrawPath(&edge, &pp);
            // маленькая сота-значок слева от текста
            GraphicsPath hp;
            hex::Path(hp, pr.X + 20 * k, pr.Y + ph / 2, 6.5 * k);
            SolidBrush hb(WithAlpha(base, (BYTE)(255 * la)));
            g.FillPath(&hb, &hp);
            StringFormat sf;
            sf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush txt(Color((BYTE)(250 * la), GetRValue(theme_.fg), GetGValue(theme_.fg), GetBValue(theme_.fg)));
            g.DrawString(label_.c_str(), -1, &font,
                         RectF(pr.X + (REAL)(34 * k), pr.Y, pr.Width - (REAL)(40 * k), pr.Height), &sf, &txt);
        }
    }
    HDC scr = GetDC(nullptr);
    POINT pos{mon_.left, mon_.top}, src{0, 0};
    SIZE size{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &pos, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void HexWave::Stop() {
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
