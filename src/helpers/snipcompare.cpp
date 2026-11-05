#include "helpers/snipcompare.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/themes.h"
#include "helpers/markup_logic.h"
#include "ui/ui.h"

using namespace Gdiplus;

namespace snipcompare {
namespace {

constexpr wchar_t kClass[] = L"MoonRadialSnipCompare";
enum { ID_MODE = 120 };

class Window;
Window* g_window = nullptr;

class Window {
public:
    Window(const UiColors& colors, std::unique_ptr<Bitmap> a, std::unique_ptr<Bitmap> b)
        : colors_(colors), a_(std::move(a)), b_(std::move(b)) {}

    bool Create() {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Static;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);  // повторная регистрация безвредна
        POINT cur;
        GetCursorPos(&cur);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
        const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
        int w = workW * 4 / 5, h = workH * 4 / 5;
        wnd_ = CreateWindowExW(0, kClass, L"Сравнение снимков", WS_OVERLAPPEDWINDOW, mi.rcWork.left + (workW - w) / 2,
                               mi.rcWork.top + (workH - h) / 2, w, h, nullptr, nullptr, inst, this);
        if (!wnd_) return false;
        k_ = GetDpiForWindow(wnd_) / 96.0;
        ui_.Attach(wnd_);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(wnd_, 20, &dark, sizeof(dark));
        ShowWindow(wnd_, SW_SHOWNORMAL);
        SetForegroundWindow(wnd_);
        return true;
    }

    void Destroy() {
        if (wnd_) DestroyWindow(wnd_);
    }

private:
    static LRESULT CALLBACK Static(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        auto* self = (Window*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        if (!self->wnd_) self->wnd_ = h;
        return self->Proc(msg, wp, lp);
    }

    void Draw(Graphics& g, Bitmap* b, const RectF& area) {
        markup::Fit f =
            markup::FitImage((int)b->GetWidth(), (int)b->GetHeight(), area.X, area.Y, area.Width, area.Height);
        g.SetInterpolationMode(f.scale >= 1 ? InterpolationModeNearestNeighbor : InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(b, RectF(f.x, f.y, f.w, f.h));
        g.SetPixelOffsetMode(PixelOffsetModeDefault);
    }

    // Снимок в заданном масштабе по центру области
    void DrawAt(Graphics& g, Bitmap* b, const RectF& area, float scale) {
        const float w = b->GetWidth() * scale, h = b->GetHeight() * scale;
        g.SetInterpolationMode(scale >= 1 ? InterpolationModeNearestNeighbor : InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(b, RectF(std::floor(area.X + (area.Width - w) / 2), std::floor(area.Y + (area.Height - h) / 2), w, h));
        g.SetPixelOffsetMode(PixelOffsetModeDefault);
    }

    // Подпись поверх снимка: какой из двух где. Плашка - чтобы читалась на любом снимке
    void Caption(const wchar_t* text, float x, float y, bool alignRight) {
        const float tw = ui_.Measure(text, 12, true) + ui_.S(18), h = ui_.S(24);
        RectF r(alignRight ? x - tw : x, y, tw, h);
        ui_.Fill(r, Color(205, 15, 15, 20), ui_.S(6));
        ui_.Text(text, r, Color(255, 255, 255, 255), 12, true, ui::Center);
    }

    void Frame(float W, float H) {
        const UiColors& c = ui_.C();
        Graphics& g = ui_.G();
        ui_.Fill(RectF(0, 0, W, H), c.win);
        const float pad = ui_.S(12), rowH = ui_.S(36);
        int mode = mode_;
        if (ui_.Segmented(ID_MODE, RectF(pad, pad, ui_.S(220), rowH), {L"Шторка", L"Рядом"}, mode)) mode_ = mode;
        const wchar_t* hint = mode_ == 0 ? L"Тяните по снимку, чтобы двигать границу" : L"Два снимка рядом";
        ui_.Text(hint, RectF(pad + ui_.S(232), pad, std::max(0.0f, W - pad * 2 - ui_.S(232)), rowH), c.muted, 12);
        const float top = pad + rowH + pad;
        area_ = RectF(pad, top, W - 2 * pad, H - top - pad);
        ui_.Fill(area_, c.field, ui_.S(8));
        const RectF inner(area_.X + ui_.S(8), area_.Y + ui_.S(8), area_.Width - ui_.S(16), area_.Height - ui_.S(16));
        if (mode_ == 1) {
            const float half = (inner.Width - ui_.S(8)) / 2;
            Draw(g, a_.get(), RectF(inner.X, inner.Y, half, inner.Height));
            Draw(g, b_.get(), RectF(inner.X + half + ui_.S(8), inner.Y, half, inner.Height));
            Caption(L"Раньше", inner.X + ui_.S(8), inner.Y + ui_.S(8), false);
            Caption(L"Позже", inner.GetRight() - ui_.S(8), inner.Y + ui_.S(8), true);
            return;
        }
        // оба снимка в одной рамке и одном масштабе: одинаковое на них совпадает, даже если размеры разные
        const float scale = markup::FitImage(std::max(a_->GetWidth(), b_->GetWidth()), std::max(a_->GetHeight(), b_->GetHeight()),
                                             inner.X, inner.Y, inner.Width, inner.Height)
                                .scale;
        const float split = inner.X + inner.Width * split_;
        GraphicsState st = g.Save();
        g.SetClip(RectF(inner.X, inner.Y, split - inner.X, inner.Height));
        DrawAt(g, a_.get(), inner, scale);
        g.Restore(st);
        st = g.Save();
        g.SetClip(RectF(split, inner.Y, inner.GetRight() - split, inner.Height));
        DrawAt(g, b_.get(), inner, scale);
        g.Restore(st);
        Pen line(c.accent, ui_.S(2));
        g.DrawLine(&line, split, inner.Y, split, inner.GetBottom());
        // подпись не висит над пустотой, когда шторку увели к краю
        if (split_ > 0.12f) Caption(L"Раньше", inner.X + ui_.S(8), inner.Y + ui_.S(8), false);
        if (split_ < 0.88f) Caption(L"Позже", inner.GetRight() - ui_.S(8), inner.Y + ui_.S(8), true);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        const float knob = ui_.S(26), cy = inner.Y + inner.Height / 2;
        SolidBrush fill(c.accent);
        g.FillEllipse(&fill, split - knob / 2, cy - knob / 2, knob, knob);
        Pen arrows(c.onAccent, ui_.S(2));
        const float a = ui_.S(5);
        g.DrawLine(&arrows, split - a, cy - a * .7f, split - a * 1.7f, cy);
        g.DrawLine(&arrows, split - a * 1.7f, cy, split - a, cy + a * .7f);
        g.DrawLine(&arrows, split + a, cy - a * .7f, split + a * 1.7f, cy);
        g.DrawLine(&arrows, split + a * 1.7f, cy, split + a, cy + a * .7f);
    }

    void SetSplit(int x) {
        if (area_.Width <= 0) return;
        split_ = std::clamp((x - area_.X) / area_.Width, 0.0f, 1.0f);
        InvalidateRect(wnd_, nullptr, FALSE);
    }

    void Paint() {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd_, &ps);
        RECT rc;
        GetClientRect(wnd_, &rc);
        int w = std::max(1L, rc.right), h = std::max(1L, rc.bottom);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Graphics g(mem);
            ui_.Begin(g, colors_, k_);
            Frame((float)w, (float)h);
            ui_.End();
        }
        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd_, &ps);
        if (ui_.TakeRepaint()) InvalidateRect(wnd_, nullptr, FALSE);
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        auto after = [&] {
            if (ui_.TakeRepaint()) InvalidateRect(wnd_, nullptr, FALSE);
        };
        switch (msg) {
            case WM_PAINT:
                Paint();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                InvalidateRect(wnd_, nullptr, FALSE);
                return 0;
            case WM_MOUSEMOVE: {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd_, 0};
                TrackMouseEvent(&tme);
                ui_.OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (dragging_) SetSplit(GET_X_LPARAM(lp));
                after();
                return 0;
            }
            case WM_MOUSELEAVE:
                ui_.OnLeave();
                after();
                return 0;
            case WM_LBUTTONDOWN: {
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                ui_.OnDown(x, y);
                if (mode_ == 0 && !ui_.HotId() && area_.Contains((REAL)x, (REAL)y)) {
                    dragging_ = true;
                    SetSplit(x);
                }
                after();
                return 0;
            }
            case WM_LBUTTONUP:
                dragging_ = false;
                ui_.OnUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                after();
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT && ui_.OnSetCursor()) return TRUE;
                if (LOWORD(lp) == HTCLIENT && mode_ == 0) {
                    POINT p;
                    GetCursorPos(&p);
                    ScreenToClient(wnd_, &p);
                    if (area_.Contains((REAL)p.x, (REAL)p.y)) {
                        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                        return TRUE;
                    }
                }
                break;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) {
                    DestroyWindow(wnd_);
                    return 0;
                }
                break;
            case WM_NCDESTROY:
                if (g_window == this) g_window = nullptr;
                SetWindowLongPtrW(wnd_, GWLP_USERDATA, 0);
                delete this;
                return 0;
        }
        return DefWindowProcW(wnd_, msg, wp, lp);
    }

    UiColors colors_;
    std::unique_ptr<Bitmap> a_, b_;
    HWND wnd_ = nullptr;
    ui::Ui ui_;
    double k_ = 1;
    int mode_ = 0;
    float split_ = 0.5f;
    bool dragging_ = false;
    RectF area_;
};

}  // namespace

void Show(const UiColors& colors, std::unique_ptr<Bitmap> left, std::unique_ptr<Bitmap> right) {
    Close();  // одно окно: новое сравнение заменяет прежнее
    if (!left || !right) return;
    auto* w = new Window(colors, std::move(left), std::move(right));
    g_window = w;
    if (!w->Create()) {
        g_window = nullptr;
        delete w;
    }
}

void Close() {
    if (g_window) g_window->Destroy();
}

}  // namespace snipcompare
