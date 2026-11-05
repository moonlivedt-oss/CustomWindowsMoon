#include "helpers/pin.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace pin {
namespace {

constexpr wchar_t kClass[] = L"MoonRadialPin", kTabClass[] = L"MoonRadialPinTab";
constexpr size_t kMaxPins = 8;
constexpr int kMinSide = 80;
constexpr float kMinOpacity = 0.2f;

struct Pin {
    HWND wnd = nullptr, tab = nullptr;  // tab - ярлычок, пока окно пропускает щелчки
    std::unique_ptr<Bitmap> image;
    bool hover = false, tracking = false;
    int hot = 0;  // кнопка панели под курсором: 1 закрыть, 2 обрезать, 3 сквозь окно, 4 прозрачность
    float opacity = 1;
    bool draggingOpacity = false;
    bool cropping = false, cropDown = false;
    POINT cropFrom{}, cropTo{};
};
std::vector<Pin*> g_pins;

int Dip(HWND w, int v) { return MulDiv(v, (int)GetDpiForWindow(w), 96); }

// Панель появляется под курсором: крестик справа, «обрезать» и «сквозь окно» слева, прозрачность снизу
RECT CloseRect(HWND w) {
    RECT rc;
    GetClientRect(w, &rc);
    int s = Dip(w, 24), m = Dip(w, 6);
    return RECT{rc.right - s - m, m, rc.right - m, m + s};
}
RECT CropRect(HWND w) {
    int s = Dip(w, 24), m = Dip(w, 6);
    return RECT{m, m, m + s, m + s};
}
RECT ThroughRect(HWND w) {
    int s = Dip(w, 24), m = Dip(w, 6);
    return RECT{m * 2 + s, m, m * 2 + s * 2, m + s};
}
RECT OpacityRect(HWND w) {
    RECT rc;
    GetClientRect(w, &rc);
    int h = Dip(w, 18), m = Dip(w, 8), width = std::min<LONG>(Dip(w, 160), rc.right - 2 * m);
    return RECT{(rc.right - width) / 2, rc.bottom - h - m, (rc.right + width) / 2, rc.bottom - m};
}

void Track(Pin* p, bool client) {
    if (p->tracking) return;
    TRACKMOUSEEVENT t{sizeof(t), (DWORD)(TME_LEAVE | (client ? 0 : TME_NONCLIENT)), p->wnd, 0};
    TrackMouseEvent(&t);
    p->tracking = true;
}

void SetHover(Pin* p, bool on) {
    if (p->hover == on) return;
    p->hover = on;
    InvalidateRect(p->wnd, nullptr, FALSE);
}

int HotControl(HWND w, POINT pt) {
    const RECT rects[] = {CloseRect(w), CropRect(w), ThroughRect(w), OpacityRect(w)};
    for (int i = 0; i < 4; ++i)
        if (PtInRect(&rects[i], pt)) return i + 1;
    return 0;
}

// Подпись к кнопке: значки без слов на снимке не прочитать
void Chip(Graphics& g, HWND w, const wchar_t* text, float x, float y, bool alignRight) {
    FontFamily fam(L"Segoe UI");
    Font font(&fam, (REAL)Dip(w, 12), FontStyleRegular, UnitPixel);
    RectF size;
    g.MeasureString(text, -1, &font, PointF(0, 0), &size);
    const float padX = (float)Dip(w, 8), height = (float)Dip(w, 22);
    RectF r(alignRight ? x - size.Width - 2 * padX : x, y, size.Width + 2 * padX, height);
    SolidBrush back(Color(225, 15, 15, 20));
    g.FillRectangle(&back, r);
    SolidBrush ink(Color(255, 255, 255, 255));
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &font, r, &center, &ink);
}

void ApplyOpacity(Pin* p) { SetLayeredWindowAttributes(p->wnd, 0, (BYTE)(255 * p->opacity), LWA_ALPHA); }

void IconBack(Graphics& g, const RECT& c, RectF& r) {
    r = RectF((REAL)c.left, (REAL)c.top, (REAL)(c.right - c.left), (REAL)(c.bottom - c.top));
    SolidBrush back(Color(200, 15, 15, 20));
    g.FillEllipse(&back, r);
}

void Paint(Pin* p) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(p->wnd, &ps);
    RECT rc;
    GetClientRect(p->wnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(p->image.get(), Rect(0, 0, rc.right, rc.bottom), 0, 0, (INT)p->image->GetWidth(),
                    (INT)p->image->GetHeight(), UnitPixel);
        g.SetPixelOffsetMode(PixelOffsetModeDefault);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        Pen white(Color(255, 255, 255, 255), 1.6f);
        white.SetStartCap(LineCapRound);
        white.SetEndCap(LineCapRound);
        if (p->cropping) {
            if (!p->cropDown) Chip(g, p->wnd, L"Обведите нужное. Правый щелчок - отмена", (float)Dip(p->wnd, 8), (float)Dip(p->wnd, 8), false);
            // обрезка: всё затемнено, выделенное светлое
            SolidBrush veil(Color(120, 0, 0, 0));
            g.FillRectangle(&veil, 0, 0, rc.right, rc.bottom);
            if (p->cropDown) {
                int x0 = std::min(p->cropFrom.x, p->cropTo.x), x1 = std::max(p->cropFrom.x, p->cropTo.x);
                int y0 = std::min(p->cropFrom.y, p->cropTo.y), y1 = std::max(p->cropFrom.y, p->cropTo.y);
                g.SetInterpolationMode(InterpolationModeBilinear);
                float sx = (float)p->image->GetWidth() / rc.right, sy = (float)p->image->GetHeight() / rc.bottom;
                g.DrawImage(p->image.get(), RectF((REAL)x0, (REAL)y0, (REAL)(x1 - x0), (REAL)(y1 - y0)), x0 * sx,
                            y0 * sy, (x1 - x0) * sx, (y1 - y0) * sy, UnitPixel);
                Pen dash(Color(255, 255, 255, 255), 1);
                dash.SetDashStyle(DashStyleDash);
                g.DrawRectangle(&dash, x0, y0, x1 - x0, y1 - y0);
            }
        } else if (p->hover) {
            RectF r;
            IconBack(g, CloseRect(p->wnd), r);
            float a = r.Width * .33f;
            g.DrawLine(&white, r.X + a, r.Y + a, r.GetRight() - a, r.GetBottom() - a);
            g.DrawLine(&white, r.GetRight() - a, r.Y + a, r.X + a, r.GetBottom() - a);
            // обрезка: два уголка
            IconBack(g, CropRect(p->wnd), r);
            a = r.Width * .3f;
            g.DrawLine(&white, r.X + a, r.Y + a * .7f, r.X + a, r.GetBottom() - a);
            g.DrawLine(&white, r.X + a, r.GetBottom() - a, r.GetRight() - a * .7f, r.GetBottom() - a);
            g.DrawLine(&white, r.GetRight() - a, r.Y + a * .7f, r.GetRight() - a, r.GetBottom() - a * 1.5f);
            g.DrawLine(&white, r.X + a * 1.5f, r.Y + a, r.GetRight() - a, r.Y + a);
            // сквозь окно: стрелка-курсор
            IconBack(g, ThroughRect(p->wnd), r);
            float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, q = r.Width * .22f;
            PointF arrow[] = {PointF(cx - q, cy - q * 1.3f), PointF(cx + q, cy), PointF(cx, cy + q * .3f),
                              PointF(cx - q * .3f, cy + q * 1.3f)};
            g.DrawPolygon(&white, arrow, 4);
            // прозрачность
            RECT o = OpacityRect(p->wnd);
            RectF bar((REAL)o.left, (REAL)o.top, (REAL)(o.right - o.left), (REAL)(o.bottom - o.top));
            SolidBrush back(Color(200, 15, 15, 20));
            g.FillRectangle(&back, bar);
            float x = bar.X + Dip(p->wnd, 6), w = bar.Width - 2 * Dip(p->wnd, 6), y = bar.Y + bar.Height / 2;
            Pen track(Color(120, 255, 255, 255), 2);
            g.DrawLine(&track, x, y, x + w, y);
            float at = x + w * (p->opacity - kMinOpacity) / (1 - kMinOpacity);
            Pen done(Color(255, 255, 255, 255), 2);
            g.DrawLine(&done, x, y, at, y);
            SolidBrush knob(Color(255, 255, 255, 255));
            const float kr = (float)Dip(p->wnd, 5);
            g.FillEllipse(&knob, at - kr, y - kr, 2 * kr, 2 * kr);
            // подписи: к кнопке под курсором, у шкалы - текущее значение
            const float top = (float)(CropRect(p->wnd).bottom + Dip(p->wnd, 4));
            if (p->hot == 1) Chip(g, p->wnd, L"Закрыть", (float)CloseRect(p->wnd).right, top, true);
            if (p->hot == 2) Chip(g, p->wnd, L"Обрезать: обведите нужное", (float)CropRect(p->wnd).left, top, false);
            if (p->hot == 3) Chip(g, p->wnd, L"Щелчки пойдут сквозь снимок", (float)ThroughRect(p->wnd).left, top, false);
            if (p->hot == 4 || p->draggingOpacity) {
                std::wstring pct = L"Прозрачность: " + std::to_wstring(100 - (int)std::lround(p->opacity * 100)) + L"%";
                Chip(g, p->wnd, pct.c_str(), bar.X + bar.Width / 2 + (float)Dip(p->wnd, 60), bar.Y - (float)Dip(p->wnd, 26), true);
            }
        }
        Pen edge(Color(120, 255, 255, 255), 1);
        g.DrawRectangle(&edge, 0, 0, rc.right - 1, rc.bottom - 1);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(p->wnd, &ps);
}

// Колесо: размер вокруг середины окна, пропорции снимка сохраняются
void Resize(Pin* p, int wheel) {
    RECT r;
    GetWindowRect(p->wnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    double f = wheel > 0 ? 1.1 : 1 / 1.1;
    const int iw = (int)p->image->GetWidth(), ih = (int)p->image->GetHeight();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(p->wnd, MONITOR_DEFAULTTONEAREST), &mi);
    double lo = std::min(1.0, (double)kMinSide / std::min(iw, ih));
    double hi = std::max(1.0, std::min((double)(mi.rcMonitor.right - mi.rcMonitor.left) / iw,
                                       (double)(mi.rcMonitor.bottom - mi.rcMonitor.top) / ih));
    double s = std::clamp((double)w / iw * f, lo, hi);
    int nw = std::max(1, (int)(iw * s)), nh = std::max(1, (int)(ih * s));
    SetWindowPos(p->wnd, nullptr, r.left + (w - nw) / 2, r.top + (h - nh) / 2, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
}

void SetOpacityAt(Pin* p, int x) {
    RECT o = OpacityRect(p->wnd);
    int m = Dip(p->wnd, 6);
    float u = std::clamp((float)(x - o.left - m) / std::max(1, (int)(o.right - o.left - 2 * m)), 0.0f, 1.0f);
    p->opacity = kMinOpacity + (1 - kMinOpacity) * u;
    ApplyOpacity(p);
    InvalidateRect(p->wnd, nullptr, FALSE);
}

// Оставить выделенное: окно становится меньше на том же месте, масштаб снимка прежний
void FinishCrop(Pin* p) {
    RECT rc;
    GetClientRect(p->wnd, &rc);
    int x0 = std::clamp<int>(std::min(p->cropFrom.x, p->cropTo.x), 0, rc.right);
    int x1 = std::clamp<int>(std::max(p->cropFrom.x, p->cropTo.x), 0, rc.right);
    int y0 = std::clamp<int>(std::min(p->cropFrom.y, p->cropTo.y), 0, rc.bottom);
    int y1 = std::clamp<int>(std::max(p->cropFrom.y, p->cropTo.y), 0, rc.bottom);
    p->cropDown = false;
    p->cropping = false;
    if (x1 - x0 >= 8 && y1 - y0 >= 8) {
        double sx = (double)p->image->GetWidth() / rc.right, sy = (double)p->image->GetHeight() / rc.bottom;
        int ix = (int)(x0 * sx), iy = (int)(y0 * sy);
        int iw = std::max(1, (int)((x1 - x0) * sx)), ih = std::max(1, (int)((y1 - y0) * sy));
        auto part = std::make_unique<Bitmap>(iw, ih, PixelFormat32bppRGB);
        {
            Graphics g(part.get());
            g.DrawImage(p->image.get(), Rect(0, 0, iw, ih), ix, iy, iw, ih, UnitPixel);
        }
        p->image = std::move(part);
        RECT w;
        GetWindowRect(p->wnd, &w);
        SetWindowPos(p->wnd, nullptr, w.left + x0, w.top + y0, x1 - x0, y1 - y0, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(p->wnd, nullptr, FALSE);
}

void LeaveThrough(Pin* p) {
    if (!p->tab) return;
    DestroyWindow(p->tab);
    p->tab = nullptr;
    LONG_PTR ex = GetWindowLongPtrW(p->wnd, GWL_EXSTYLE);
    SetWindowLongPtrW(p->wnd, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_TRANSPARENT);
}

LRESULT CALLBACK TabProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* p = (Pin*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            {
                Graphics g(dc);
                g.SetSmoothingMode(SmoothingModeHighQuality);
                SolidBrush back(Color(255, 229, 72, 77));
                g.FillRectangle(&back, 0, 0, rc.right, rc.bottom);
                FontFamily fam(L"Segoe UI");
                Font font(&fam, (REAL)Dip(h, 12), FontStyleBold, UnitPixel);
                SolidBrush ink(Color(255, 255, 255, 255));
                StringFormat center;
                center.SetAlignment(StringAlignmentCenter);
                center.SetLineAlignment(StringAlignmentCenter);
                g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
                g.DrawString(L"Вернуть щелчки", -1, &font, RectF(0, 0, (REAL)rc.right, (REAL)rc.bottom), &center, &ink);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONUP:
            if (p) LeaveThrough(p);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Щелчки идут сквозь снимок к окну под ним. Выключить можно ярлычком: по самому снимку уже не щёлкнуть
void EnterThrough(Pin* p) {
    if (p->tab) return;
    SetHover(p, false);
    LONG_PTR ex = GetWindowLongPtrW(p->wnd, GWL_EXSTYLE);
    SetWindowLongPtrW(p->wnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = TabProc;
    wc.hInstance = inst;
    wc.lpszClassName = kTabClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassW(&wc);  // повторная регистрация безвредна
    RECT r;
    GetWindowRect(p->wnd, &r);
    const int tabW = Dip(p->wnd, 124), tabH = Dip(p->wnd, 26);
    p->tab = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTabClass, L"Вернуть щелчки", WS_POPUP,
                             r.right - tabW, r.top, tabW, tabH, nullptr, nullptr, inst, p);
    if (p->tab) ShowWindow(p->tab, SW_SHOWNOACTIVATE);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* p = (Pin*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!p) return DefWindowProcW(h, msg, wp, lp);
    auto inRect = [&](const RECT& r, POINT pt) { return PtInRect(&r, pt) != FALSE; };
    switch (msg) {
        case WM_NCHITTEST: {
            if (p->cropping) return HTCLIENT;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &pt);
            // кнопки панели ловят щелчок, всё остальное тянет окно
            if (p->hover && (inRect(CloseRect(h), pt) || inRect(CropRect(h), pt) || inRect(ThroughRect(h), pt) ||
                             inRect(OpacityRect(h), pt)))
                return HTCLIENT;
            if (p->draggingOpacity) return HTCLIENT;
            return HTCAPTION;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint(p);
            return 0;
        case WM_SIZE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (p->cropping) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            Track(p, true);
            SetHover(p, true);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            {
                int hot = HotControl(h, pt);
                if (hot != p->hot) {
                    p->hot = hot;
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            if (p->draggingOpacity) SetOpacityAt(p, pt.x);
            if (p->cropDown) {
                p->cropTo = pt;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_NCMOUSEMOVE:
            Track(p, false);
            SetHover(p, true);
            if (p->hot) {  // ушли с кнопки на сам снимок
                p->hot = 0;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
        case WM_NCMOUSELEAVE:
            p->tracking = false;
            {
                POINT cur;
                GetCursorPos(&cur);
                RECT r;
                GetWindowRect(h, &r);
                SetHover(p, PtInRect(&r, cur) != FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (p->cropping) {
                p->cropDown = true;
                p->cropFrom = p->cropTo = pt;
                SetCapture(h);
            } else if (inRect(OpacityRect(h), pt)) {
                p->draggingOpacity = true;
                SetCapture(h);
                SetOpacityAt(p, pt.x);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (p->cropDown) {
                p->cropTo = pt;
                ReleaseCapture();
                FinishCrop(p);
                return 0;
            }
            if (p->draggingOpacity) {
                p->draggingOpacity = false;
                ReleaseCapture();
                return 0;
            }
            if (inRect(CloseRect(h), pt))
                DestroyWindow(h);
            else if (inRect(CropRect(h), pt)) {
                p->cropping = true;
                InvalidateRect(h, nullptr, FALSE);
            } else if (inRect(ThroughRect(h), pt))
                EnterThrough(p);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (!p->cropping) Resize(p, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_RBUTTONUP:
            if (p->cropping) {  // передумали обрезать
                p->cropping = p->cropDown = false;
                if (GetCapture() == h) ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            DestroyWindow(h);
            return 0;
        case WM_NCLBUTTONDBLCLK:
        case WM_NCRBUTTONUP:
            DestroyWindow(h);
            return 0;
        case WM_NCRBUTTONDOWN:
        case WM_CONTEXTMENU:
            return 0;  // без системного меню: правый щелчок закрывает
        case WM_NCDESTROY:
            if (p->tab) DestroyWindow(p->tab);
            g_pins.erase(std::remove(g_pins.begin(), g_pins.end(), p), g_pins.end());
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            delete p;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Show(Bitmap& image) {
    if (g_pins.size() >= kMaxPins) DestroyWindow(g_pins.front()->wnd);  // самый старый уступает место
    int iw = (int)image.GetWidth(), ih = (int)image.GetHeight();
    if (iw <= 0 || ih <= 0) return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);  // повторная регистрация безвредна

    auto* p = new Pin();
    p->image = std::make_unique<Bitmap>(iw, ih, PixelFormat32bppRGB);
    {
        Graphics g(p->image.get());
        g.DrawImage(&image, 0, 0, iw, ih);
    }
    POINT cur;
    GetCursorPos(&cur);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
    const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
    // большой снимок поначалу не закрывает полэкрана; следующие ложатся лесенкой, а не друг на друга
    double s = std::min({1.0, workW * .4 / iw, workH * .5 / ih});
    int w = std::max(1, (int)(iw * s)), h = std::max(1, (int)(ih * s));
    int shift = (int)g_pins.size() * 28;
    int x = mi.rcWork.left + (workW - w) / 2 + shift, y = mi.rcWork.top + (workH - h) / 2 + shift;
    g_pins.push_back(p);
    p->wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClass,
                             L"Закреплённый снимок", WS_POPUP, x, y, w, h, nullptr, nullptr, inst, p);
    if (!p->wnd) {
        g_pins.pop_back();
        delete p;
        return;
    }
    ApplyOpacity(p);
    int round = 2;  // DWMWCP_ROUND: скругление на Windows 11, на прежних просто игнорируется
    DwmSetWindowAttribute(p->wnd, 33, &round, sizeof(round));
    ShowWindow(p->wnd, SW_SHOWNOACTIVATE);
}

void CloseAll() {
    while (!g_pins.empty()) DestroyWindow(g_pins.back()->wnd);
}

int Count() { return (int)g_pins.size(); }

}  // namespace pin
