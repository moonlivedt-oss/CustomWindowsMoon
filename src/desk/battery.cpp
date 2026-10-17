#include "desk/battery.h"

#include <shellscalingapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#include "core/fx.h"
#include "gfx/hex.h"
#include "core/config.h"
#include "core/night.h"
#include "desk/corners.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialBattery";
constexpr int kHeight = 70;  // высота свечения, до масштаба
constexpr ULONGLONG kWaveMs = 2000, kCheckMs = 2000, kFrameMs = 33;

}  // namespace

void BatteryGlow::Create(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        hasBattery_ = !(ps.BatteryFlag & 128) && ps.BatteryLifePercent <= 100;
        ac_ = ps.ACLineStatus == 1;
        percent_ = ps.BatteryLifePercent;
    }
}

void BatteryGlow::Test(bool charging) {
    testCharge_ = charging;
    testUntil_ = GetTickCount64() + (charging ? kWaveMs : 3500);
    if (charging) waveAt_ = GetTickCount64();
}

void BatteryGlow::Tick(const Config& cfg) {
    hex_ = cfg.hexGlass;
    if (!wnd_) return;
    ULONGLONG now = GetTickCount64();
    bool testing = now < testUntil_;
    if (!cfg.batteryGlow && !testing) {
        Hide();
        return;
    }
    if (now - lastCheck_ >= kCheckMs) {
        lastCheck_ = now;
        theme_ = CurrentTheme(cfg);
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps)) {
            hasBattery_ = !(ps.BatteryFlag & 128) && ps.BatteryLifePercent <= 100;
            bool ac = ps.ACLineStatus == 1;
            if (ac && !ac_ && hasBattery_) waveAt_ = now;  // только что подключили зарядку
            ac_ = ac;
            percent_ = ps.BatteryLifePercent;
        }
        low_ = hasBattery_ && !ac_ && percent_ <= cfg.batteryLow;
    }
    bool lowNow = (low_ && !fx::Game()) || (testing && !testCharge_);  // в игровом режиме не светим
    float wave = waveAt_ && now - waveAt_ < kWaveMs && (!fx::Game() || testing) ? (now - waveAt_) / (float)kWaveMs : -1;
    show_ = lowNow ? std::min(1.0f, show_ + fx::Step(0.03f)) : std::max(0.0f, show_ - fx::Step(0.05f));
    if (show_ <= 0 && wave < 0) {
        Hide();
        return;
    }
    // поверх полноэкранной игры не мигаем
    POINT o{0, 0};
    HMONITOR mon = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (cfg.skipFullscreen && corners::FullscreenOn(mon, mi.rcMonitor) && !testing) {
        Hide();
        return;
    }
    fx::Active();
    if (now - lastFrame_ < kFrameMs) return;
    lastFrame_ = now;
    // дыхание: медленный синус, период около 2.6 с
    float pulse = show_ * (0.45f + 0.55f * (0.5f + 0.5f * std::sin(now / 2600.0f * 6.2832f)));
    Paint(pulse, wave);
}

void BatteryGlow::Hide() {
    if (wnd_ && IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_HIDE);
}

void BatteryGlow::Paint(float pulse, float wave) {
    POINT o{0, 0};
    HMONITOR mon = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
    float k = dpi / 96.0f;
    int W = mi.rcMonitor.right - mi.rcMonitor.left, H = (int)(kHeight * k);
    float dim = night::Glow();

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
        g.Clear(Color(0, 0, 0, 0));
        Color ac = ToColor(theme_.ac), glow = ToColor(theme_.gold);
        if (pulse > 0) {
            // широкое мягкое пятно от середины нижнего края: концы гаснут сами
            BYTE a = (BYTE)std::min(255.0f, 190 * pulse * dim);
            float rw = W * 0.62f, rh = H * 2.0f;
            GraphicsPath p;
            p.AddEllipse(W / 2.0f - rw, H - rh / 2, rw * 2, rh);
            PathGradientBrush pg(&p);
            pg.SetCenterPoint(PointF(W / 2.0f, (REAL)H));
            pg.SetCenterColor(WithAlpha(ac, a));
            Color edge(0, ac.GetR(), ac.GetG(), ac.GetB());
            int n = 1;
            pg.SetSurroundColors(&edge, &n);
            g.FillPath(&pg, &p);
            LinearGradientBrush line(PointF(W * 0.1f, 0), PointF(W * 0.9f, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0));
            Color lc = WithAlpha(MixColor(ac, Color(255, 255, 255), 0.3), (BYTE)(230 * pulse * dim));
            Color cols[] = {WithAlpha(lc, 0), lc, WithAlpha(lc, 0)};
            REAL pos[] = {0, 0.5f, 1};
            line.SetInterpolationColors(cols, pos, 3);
            g.FillRectangle(&line, RectF(W * 0.1f, H - 2 * k, W * 0.8f, 2 * k));
        }
        if (wave >= 0 && hex_) {
            // волна загорающихся сот слева направо: у фронта соты поднимаются выше и горят ярче,
            // позади остаётся гаснущий след
            float ew = wave < 0.5f ? 2 * wave * wave : 1 - 2 * (1 - wave) * (1 - wave);  // плавно разгоняется и тормозит
            float x = ew * (W + 400 * k) - 200 * k;
            float fade = wave < 0.8f ? 1.0f : (1 - wave) / 0.2f;
            double s = 12 * k;
            Color rim = MixColor(glow, Color(255, 255, 255), 0.35);
            hex::ForEach(0, 0, W, H + s, s, [&](const hex::Cell& c) {
                double up = (H - c.y) / H;  // 0 - у нижнего края, 1 - верх окна
                if (up > 1 || up < -0.1) return;
                double d = (c.x - x) / (150 * k);
                double lit = std::exp(-d * d);
                if (c.x < x) lit = std::max(lit, 0.32 * std::exp((c.x - x) / (520 * k)));
                double reach = 0.45 + 0.55 * std::exp(-d * d);  // у фронта соты встают выше
                double a = lit * std::max(0.0, 1 - std::max(0.0, up) / reach) * fade * dim;
                if (a < 0.02) return;
                GraphicsPath p;
                hex::Path(p, c.x, c.y, s, 0.84);
                SolidBrush fill(WithAlpha(MixColor(glow, ToColor(theme_.ac), 0.35), (BYTE)std::min(255.0, 190 * a)));
                g.FillPath(&fill, &p);
                Pen edge(WithAlpha(rim, (BYTE)std::min(255.0, 255 * a)), 1.4f * k);
                g.DrawPath(&edge, &p);
            });
            SolidBrush trail(WithAlpha(glow, (BYTE)(140 * fade * dim)));
            g.FillRectangle(&trail, RectF(0, H - 2 * k, std::max(0.0f, x), 2 * k));
        } else if (wave >= 0) {
            // светлая волна слева направо: пятно с размытыми краями, к концу гаснет
            float x = (float)EaseOut(wave) * (W + 400 * k) - 200 * k;
            float fade = wave < 0.8f ? 1.0f : (1 - wave) / 0.2f;
            float rw = 260 * k, rh = H * 1.6f;
            GraphicsPath p;
            p.AddEllipse(x - rw, H - rh / 2, rw * 2, rh);
            PathGradientBrush pg(&p);
            pg.SetCenterPoint(PointF(x, (REAL)H));
            pg.SetCenterColor(WithAlpha(glow, (BYTE)(200 * fade * dim)));
            Color edge(0, glow.GetR(), glow.GetG(), glow.GetB());
            int n = 1;
            pg.SetSurroundColors(&edge, &n);
            g.FillPath(&pg, &p);
            SolidBrush trail(WithAlpha(glow, (BYTE)(140 * fade * dim)));
            g.FillRectangle(&trail, RectF(0, H - 2 * k, std::max(0.0f, x), 2 * k));
        }
    }
    POINT src{0, 0}, dst{mi.rcMonitor.left, mi.rcMonitor.bottom - H};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}
