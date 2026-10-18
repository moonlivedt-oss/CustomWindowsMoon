#include "desk/winfx.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#include "core/fx.h"
#include "core/llmouse.h"
#include "core/config.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialWinFx";
constexpr UINT WM_FX_WHEEL = WM_APP + 1, WM_FX_PIN = WM_APP + 2;
constexpr int kStrip = 44;      // высота «верхней полосы» окна, до масштаба
constexpr BYTE kMinAlpha = 51;  // 20%: совсем невидимое окно потом не найти
constexpr ULONGLONG kShow = 1300;
constexpr float kW = 300, kH = 64, kM = 22;   // подсказка и поля под тень
constexpr DWORD kBorderDefault = 0xFFFFFFFF;  // DWMWA_COLOR_DEFAULT
constexpr DWORD kBorderAttr = 34;             // DWMWA_BORDER_COLOR (Windows 11)

WindowFx* g_fx = nullptr;
bool g_swallowMiddleUp = false;  // только поток перехвата

void RoundRect(GraphicsPath& p, const RectF& r, float rad) {
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

RECT Frame(HWND w) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(w, &r);
    return r;
}

std::wstring Title(HWND w) {
    wchar_t b[256] = {};
    GetWindowTextW(w, b, 256);
    return b;
}

// Отпущенный Alt без других клавиш открыл бы меню окна: гасим это пустым нажатием.
void MaskAlt() {
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

}  // namespace

void PinForegroundWindow() {
    if (!g_fx) return;
    HWND w = GetForegroundWindow();
    if (w) g_fx->TogglePin(GetAncestor(w, GA_ROOT));
}

void WindowFx::Create(HINSTANCE inst) {
    g_fx = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    llmouse::Set(llmouse::WINDOW_FX, OnMouse);
}

void WindowFx::Apply(const Config& cfg) {
    enabled_ = cfg.winFx;
    llmouse::Need(llmouse::WINDOW_FX, enabled_ && wnd_);
    theme_ = CurrentTheme(cfg);
    // рамки закреплённых окон перекрасить в новый цвет темы
    for (auto& [w, s] : states_) {
        if (!s.pinned) continue;
        DWORD c = theme_.ac;
        DwmSetWindowAttribute(w, kBorderAttr, &c, sizeof(c));
    }
}

void WindowFx::Destroy() {
    llmouse::Need(llmouse::WINDOW_FX, false);
    ResetAll();
}

void WindowFx::ResetAll() {
    for (auto& [w, s] : states_) {
        if (!IsWindow(w)) continue;
        if (s.alpha != 255) SetAlpha(w, 255);
        if (s.pinned) Unpin(w, s);
    }
    states_.clear();
}

HWND WindowFx::TargetAt(POINT pt) {
    HWND w = WindowFromPoint(pt);
    if (!w) return nullptr;
    w = GetAncestor(w, GA_ROOT);
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    // рабочий стол, панель задач и наши собственные окна не трогаем
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcsncmp(cls, L"MoonRadial", 10))
        return nullptr;
    if (IsZoomed(w) == FALSE && IsIconic(w)) return nullptr;
    RECT r = Frame(w);
    UINT dpi = GetDpiForWindow(w);
    int strip = kStrip * (int)(dpi ? dpi : 96) / 96;
    if (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.top + strip) return nullptr;
    return w;
}

bool WindowFx::OnMouse(WPARAM wp, const MSLLHOOKSTRUCT& m) {
    // поток перехвата (core/llmouse): только быстрые проверки, сама работа уходит сообщением
    if (!g_fx) return false;
    if (wp == WM_MBUTTONUP && g_swallowMiddleUp) {
        g_swallowMiddleUp = false;
        return true;
    }
    if (wp != WM_MOUSEWHEEL && wp != WM_MBUTTONDOWN) return false;
    bool alt = GetAsyncKeyState(VK_MENU) & 0x8000;
    bool other = (GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT)) & 0x8000;
    if (!alt || other) return false;
    HWND w = TargetAt(m.pt);
    if (!w) return false;
    if (wp == WM_MOUSEWHEEL) {
        PostMessageW(g_fx->wnd_, WM_FX_WHEEL, (WPARAM)w, (LPARAM)(short)HIWORD(m.mouseData));
    } else {
        g_swallowMiddleUp = true;
        PostMessageW(g_fx->wnd_, WM_FX_PIN, (WPARAM)w, 0);
    }
    MaskAlt();  // сразу, пока Alt ещё зажат: иначе его отпускание откроет меню окна
    return true;
}

LRESULT CALLBACK WindowFx::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (g_fx && msg == WM_FX_WHEEL) {
        g_fx->Wheel((HWND)wp, (int)lp);
        return 0;
    }
    if (g_fx && msg == WM_FX_PIN) {
        g_fx->TogglePin((HWND)wp);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void WindowFx::SetAlpha(HWND w, BYTE a) {
    State& s = states_[w];
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (a == 255) {
        if (s.addedLayered) {
            SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
            SetWindowLongPtrW(w, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
            s.addedLayered = false;
        } else if (ex & WS_EX_LAYERED) {
            SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
        }
    } else {
        if (!(ex & WS_EX_LAYERED)) {
            SetWindowLongPtrW(w, GWL_EXSTYLE, ex | WS_EX_LAYERED);
            s.addedLayered = true;
        }
        SetLayeredWindowAttributes(w, 0, a, LWA_ALPHA);
    }
    s.alpha = a;
}

void WindowFx::Wheel(HWND w, int delta) {
    if (!IsWindow(w)) return;
    State& s = states_[w];
    BYTE cur = s.alpha;
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (!s.addedLayered && (ex & WS_EX_LAYERED)) {
        // окно уже прозрачное само: берём его уровень; рисующие себя попиксельно уровня не отдают
        BYTE a = 255;
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(w, nullptr, &a, &flags)) {
            Show(w, K_DENIED, 0, L"Не получится", L"Это окно само рисует свою прозрачность");
            return;
        }
        cur = (flags & LWA_ALPHA) ? a : 255;
    }
    int step = (int)std::lround(delta / (float)WHEEL_DELTA * 13);  // 5% за щелчок
    int next = std::clamp((int)cur + step, (int)kMinAlpha, 255);
    SetLastError(0);
    SetAlpha(w, (BYTE)next);
    // окно от администратора не даёт себя менять: проверяем, что прозрачность встала
    BYTE got = 255;
    DWORD flags = 0;
    bool ok = next == 255 || (GetLayeredWindowAttributes(w, nullptr, &got, &flags) && got == next);
    if (!ok) {
        states_.erase(w);
        Show(w, K_DENIED, 0, L"Нет доступа", L"Окно запущено от администратора");
        return;
    }
    if (next == 255 && !states_[w].pinned) states_.erase(w);
    Show(w, K_ALPHA, (int)std::lround(next * 100 / 255.0), L"Непрозрачность", Title(w));
}

void WindowFx::Unpin(HWND w, State& s) {
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DWORD c = kBorderDefault;
    DwmSetWindowAttribute(w, kBorderAttr, &c, sizeof(c));
    s.pinned = false;
}

void WindowFx::TogglePin(HWND w) {
    if (!w || !IsWindow(w) || w == GetShellWindow()) return;
    State& s = states_[w];
    bool topmost = GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST;
    if (s.pinned || topmost) {
        Unpin(w, s);
        if (s.alpha == 255) states_.erase(w);
        Show(w, K_UNPIN, 0, L"Больше не поверх", Title(w));
        return;
    }
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!(GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        if (s.alpha == 255) states_.erase(w);
        Show(w, K_DENIED, 0, L"Нет доступа", L"Окно запущено от администратора");
        return;
    }
    s.pinned = true;
    DWORD c = theme_.ac;
    DwmSetWindowAttribute(w, kBorderAttr, &c, sizeof(c));
    Show(w, K_PIN, 0, L"Поверх всех окон", Title(w));
}

void WindowFx::Show(HWND w, int kind, int value, const std::wstring& title, const std::wstring& sub) {
    kind_ = kind;
    value_ = value;
    title_ = title;
    sub_ = sub.empty() ? L"Окно без названия" : sub;
    anchor_ = Frame(w);
    until_ = GetTickCount64() + kShow;
    if (show_ <= 0) show_ = 0.01f;
    Paint();
}

void WindowFx::Tick() {
    ULONGLONG now = GetTickCount64();
    if (now - lastPrune_ > 1000) {
        lastPrune_ = now;
        for (auto it = states_.begin(); it != states_.end();)
            it = IsWindow(it->first) ? std::next(it) : states_.erase(it);
    }
    if (show_ <= 0) return;
    fx::Active();
    float target = now < until_ ? 1.0f : 0.0f;
    float before = show_;
    show_ = target > show_ ? std::min(1.0f, show_ + fx::Step(0.16f)) : std::max(0.0f, show_ - fx::Step(0.07f));
    if (show_ <= 0) {
        ShowWindow(wnd_, SW_HIDE);
        return;
    }
    if (show_ != before) Paint();
}

void WindowFx::Paint() {
    POINT c{(anchor_.left + anchor_.right) / 2, anchor_.top + 40};
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
    float k = dpi / 96.0f;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int W = (int)std::ceil((kW + kM * 2) * k), H = (int)std::ceil((kH + kM * 2) * k);
    // под верхней полосой окна, по центру; не вылезая за экран
    int x = std::clamp((int)c.x - W / 2, (int)mi.rcWork.left, (int)mi.rcWork.right - W);
    int y = std::clamp((int)(anchor_.top + 50 * k - kM * k), (int)mi.rcWork.top, (int)mi.rcWork.bottom - H);
    float e = (float)EaseOut(show_);
    y += (int)((1 - e) * 8 * k);

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
        Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), bg = ToColor(theme_.bg),
              ac = ToColor(kind_ == K_DENIED ? RGB(240, 110, 90) : theme_.ac);
        float m = kM * k;
        RectF body(m, m, kW * k, kH * k);
        float rad = 18 * k;

        // мягкая тень: несколько расходящихся полупрозрачных слоёв
        for (int i = 6; i >= 1; --i) {
            GraphicsPath sp;
            RoundRect(sp,
                      RectF(body.X - i * 2 * k, body.Y - i * 1.5f * k + 5 * k, body.Width + i * 4 * k,
                            body.Height + i * 3 * k),
                      rad + i * 2 * k);
            SolidBrush sb(Color((BYTE)(9 + (6 - i) * 3), 0, 0, 0));
            g.FillPath(&sb, &sp);
        }
        GraphicsPath path;
        RoundRect(path, body, rad);
        SolidBrush fill(WithAlpha(bg, 242));
        g.FillPath(&fill, &path);
        LinearGradientBrush tint(PointF(body.X, 0), PointF(body.X + body.Width * 0.6f, 0), WithAlpha(ac, 34),
                                 WithAlpha(ac, 0));
        g.FillPath(&tint, &path);
        LinearGradientBrush shine(PointF(0, body.Y), PointF(0, body.Y + body.Height), Color(24, 255, 255, 255),
                                  Color(0, 255, 255, 255));
        g.FillPath(&shine, &path);
        Pen rim(Color(40, 255, 255, 255), 1 * k);
        g.DrawPath(&rim, &path);

        // значок в кружке
        RectF ic(body.X + 14 * k, body.Y + 14 * k, 36 * k, 36 * k);
        SolidBrush icBg(WithAlpha(ac, 46));
        g.FillEllipse(&icBg, ic);
        Pen icRim(WithAlpha(ac, 120), 1 * k);
        g.DrawEllipse(&icRim, ic);
        float cx = ic.X + ic.Width / 2, cy = ic.Y + ic.Height / 2;
        Color icCol = MixColor(ac, Color(255, 255, 255), 0.35);
        SolidBrush icB(icCol);
        Pen icP(icCol, 2 * k);
        if (kind_ == K_ALPHA) {
            // круг, залитый на долю непрозрачности
            float r = 9 * k;
            g.DrawEllipse(&icP, cx - r, cy - r, r * 2, r * 2);
            GraphicsPath clip;
            clip.AddEllipse(cx - r, cy - r, r * 2, r * 2);
            GraphicsState st = g.Save();
            g.SetClip(&clip);
            float h = r * 2 * value_ / 100.0f;
            g.FillRectangle(&icB, RectF(cx - r, cy + r - h, r * 2, h));
            g.Restore(st);
        } else if (kind_ == K_PIN || kind_ == K_UNPIN) {
            // канцелярская кнопка, наклонённая на 45 градусов
            GraphicsState st = g.Save();
            g.TranslateTransform(cx, cy);
            g.RotateTransform(45);
            GraphicsPath head;
            RoundRect(head, RectF(-5 * k, -10 * k, 10 * k, 4 * k), 1.5f * k);
            g.FillPath(&icB, &head);
            g.FillRectangle(&icB, RectF(-3 * k, -7 * k, 6 * k, 7 * k));
            GraphicsPath base;
            RoundRect(base, RectF(-7 * k, -0.5f * k, 14 * k, 3.5f * k), 1.5f * k);
            g.FillPath(&icB, &base);
            Pen needle(icCol, 1.8f * k);
            needle.SetEndCap(LineCapRound);
            g.DrawLine(&needle, 0.0f, 3 * k, 0.0f, 10 * k);
            g.Restore(st);
            if (kind_ == K_UNPIN) {
                Pen cross(icCol, 2 * k);
                cross.SetStartCap(LineCapRound);
                cross.SetEndCap(LineCapRound);
                g.DrawLine(&cross, cx - 9 * k, cy + 9 * k, cx + 9 * k, cy - 9 * k);
            }
        } else {
            Font f(Family(), 18 * k, FontStyleBold, UnitPixel);
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(L"!", -1, &f, ic, &sf, &icB);
        }

        // текст
        FontFamily fam(L"Segoe UI");
        Font fT(Family(), 13 * k, FontStyleRegular, UnitPixel);
        Font fS(&fam, 11 * k, FontStyleRegular, UnitPixel);
        StringFormat sf;
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        float tx = body.X + 62 * k, tw = body.Width - 62 * k - 16 * k;
        SolidBrush tB(fg), sB(muted), aB(icCol);
        bool bar = kind_ == K_ALPHA;
        float ty = bar ? body.Y + 10 * k : body.Y + 13 * k;
        g.DrawString(title_.c_str(), -1, &fT, RectF(tx, ty, tw - 50 * k, 20 * k), &sf, &tB);
        if (bar) {
            StringFormat rf;
            rf.SetAlignment(StringAlignmentFar);
            rf.SetLineAlignment(StringAlignmentCenter);
            std::wstring v = std::to_wstring(value_) + L"%";
            g.DrawString(v.c_str(), -1, &fT, RectF(tx, ty, tw, 20 * k), &rf, &aB);
        }
        g.DrawString(sub_.c_str(), -1, &fS, RectF(tx, ty + 19 * k, tw, 17 * k), &sf, &sB);
        if (bar) {
            RectF track(tx, body.Y + body.Height - 13 * k, tw, 4 * k);
            GraphicsPath tp;
            RoundRect(tp, track, 2 * k);
            SolidBrush trB(Color(40, 255, 255, 255));
            g.FillPath(&trB, &tp);
            // заполнение начинается с 20%: ниже прозрачность не опускается
            float t = std::clamp((value_ - 20) / 80.0f, 0.0f, 1.0f);
            if (t > 0) {
                GraphicsPath fp;
                RoundRect(fp, RectF(track.X, track.Y, std::max(4 * k, track.Width * t), track.Height), 2 * k);
                LinearGradientBrush fb(PointF(track.X - 1, 0), PointF(track.X + track.Width + 1, 0),
                                       MixColor(ac, Color(255, 255, 255), 0.35), ac);
                g.FillPath(&fb, &fp);
            }
        }
    }
    POINT src{0, 0}, dst{x, y};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(255 * e), AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}
