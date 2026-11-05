// Пипетка и линейка: окно на весь виртуальный экран поверх замороженного снимка, лупа и замер.
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#include "system/auto/autos.h"
#include "core/config.h"
#include "core/themes.h"
#include "cards/boostcard.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "helpers/picker.h"

using namespace Gdiplus;

namespace picker {

namespace {

constexpr wchar_t kClass[] = L"MoonRadialPicker";
constexpr int kZoom = 10, kCells = 13;  // лупа: 13 x 13 пикселей по 10

HINSTANCE g_inst = nullptr;
const Config* g_cfg = nullptr;
HWND g_wnd = nullptr;
BoostCard g_card;
RECT g_virt{};
HBITMAP g_shot = nullptr;
DWORD* g_bits = nullptr;
HDC g_shotDC = nullptr;
HGDIOBJ g_shotOld = nullptr;
POINT g_cur{}, g_from{};
bool g_down = false, g_measure = false;

DWORD PixelAt(POINT p) {  // 0x00RRGGBB
    int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
    int x = std::clamp((int)(p.x - g_virt.left), 0, w - 1), y = std::clamp((int)(p.y - g_virt.top), 0, h - 1);
    return g_bits[(size_t)y * w + x] & 0xFFFFFF;
}

void PutText(const std::wstring& t) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (m) {
        memcpy(GlobalLock(m), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(m);
        if (!SetClipboardData(CF_UNICODETEXT, m)) GlobalFree(m);
    }
    CloseClipboard();
}

void Close() {
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
    if (g_shotDC) {
        SelectObject(g_shotDC, g_shotOld);
        DeleteDC(g_shotDC);
        g_shotDC = nullptr;
    }
    if (g_shot) DeleteObject(g_shot);
    g_shot = nullptr;
    g_bits = nullptr;
    g_down = g_measure = false;
}

void Done(const std::wstring& copied, const std::wstring& what) {
    Close();
    PutText(copied);
    g_card.kind = L"-";
    g_card.Show(BoostCard::DONE, what + L" в буфере", copied, CurrentTheme(*g_cfg), 3500, Toast::G_OK);
}

POINT Local(POINT p) { return {p.x - g_virt.left, p.y - g_virt.top}; }

void Label(Graphics& g, const std::wstring& text, PointF at, Color back, Color fore, float k) {
    FontFamily fam(L"Segoe UI");
    Font f(&fam, 13 * k, FontStyleRegular, UnitPixel);
    RectF m;
    g.MeasureString(text.c_str(), -1, &f, PointF(0, 0), &m);
    RectF r(at.X, at.Y, m.Width + 16 * k, m.Height + 8 * k);
    GraphicsPath p;
    RoundRectPath(p, r, 8 * k);
    SolidBrush b(back), t(fore);
    g.FillPath(&b, &p);
    g.DrawString(text.c_str(), -1, &f, PointF(r.X + 8 * k, r.Y + 4 * k), &t);
}

void Paint(HDC dc) {
    int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, g_shotDC, 0, 0, SRCCOPY);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        UINT dpi = GetDpiForWindow(g_wnd);
        float k = (dpi ? dpi : 96) / 96.0f;
        ThemeInfo th = CurrentTheme(*g_cfg);
        Color ac = ToColor(th.ac), bg = ToColor(th.bg, 235), fg = ToColor(th.fg);
        POINT c = Local(g_cur);
        if (g_measure) {
            POINT a = Local(g_from);
            Pen dash(WithAlpha(ac, 160), 1);
            dash.SetDashStyle(DashStyleDash);
            g.DrawRectangle(&dash, (REAL)std::min(a.x, c.x), (REAL)std::min(a.y, c.y), (REAL)std::abs(c.x - a.x),
                            (REAL)std::abs(c.y - a.y));
            Pen line(ac, 2 * k);
            g.DrawLine(&line, (REAL)a.x + 0.5f, (REAL)a.y + 0.5f, (REAL)c.x + 0.5f, (REAL)c.y + 0.5f);
            SolidBrush dot(ac);
            g.FillEllipse(&dot, a.x - 3 * k, a.y - 3 * k, 6 * k, 6 * k);
            Label(g, Measure(c.x - a.x, c.y - a.y), PointF((REAL)c.x + 14 * k, (REAL)c.y - 34 * k), bg, fg, k);
        }
        // лупа
        float cell = kZoom * k, side = cell * kCells;
        float lx = c.x + 24 * k, ly = c.y + 24 * k;
        if (lx + side > w) lx = c.x - 24 * k - side;
        if (ly + side + 34 * k > h) ly = c.y - 24 * k - side - 34 * k;
        RectF box(lx, ly, side, side);
        GraphicsPath clip;
        RoundRectPath(clip, box, 12 * k);
        SolidBrush back(bg);
        g.FillPath(&back, &clip);
        g.SetClip(&clip);
        g.SetSmoothingMode(SmoothingModeNone);
        for (int dy = 0; dy < kCells; ++dy)
            for (int dx = 0; dx < kCells; ++dx) {
                DWORD px = PixelAt({g_cur.x + dx - kCells / 2, g_cur.y + dy - kCells / 2});
                SolidBrush b(Color(255, (BYTE)(px >> 16), (BYTE)(px >> 8), (BYTE)px));
                g.FillRectangle(&b, lx + dx * cell, ly + dy * cell, cell, cell);
            }
        Pen grid(Color(40, 0, 0, 0), 1);
        for (int i = 1; i < kCells; ++i) {
            g.DrawLine(&grid, lx + i * cell, ly, lx + i * cell, ly + side);
            g.DrawLine(&grid, lx, ly + i * cell, lx + side, ly + i * cell);
        }
        float mid = (kCells / 2) * cell;
        Pen sel(Color(255, 255, 255, 255), 2), selIn(Color(255, 0, 0, 0), 1);
        g.DrawRectangle(&sel, lx + mid, ly + mid, cell, cell);
        g.DrawRectangle(&selIn, lx + mid - 1, ly + mid - 1, cell + 2, cell + 2);
        g.ResetClip();
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        Pen rim(ac, 2 * k);
        g.DrawPath(&rim, &clip);
        DWORD px = PixelAt(g_cur);
        BYTE r = (BYTE)(px >> 16), gg = (BYTE)(px >> 8), b = (BYTE)px;
        Label(g, Hex(r, gg, b) + L"   " + RgbText(r, gg, b), PointF(lx, ly + side + 6 * k), bg, fg, k);
        // подсказка сверху монитора курсора
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(g_cur, MONITOR_DEFAULTTONEAREST), &mi);
        Label(g,
              L"Щелчок - цвет (HEX), Ctrl+щелчок - RGB, протянуть - замер (Shift - по оси), стрелки - на пиксель, "
              L"Esc - выйти",
              PointF((REAL)(mi.rcMonitor.left - g_virt.left) + 16 * k, (REAL)(mi.rcMonitor.top - g_virt.top) + 16 * k),
              bg, fg, k);
    }
    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void Move(POINT p) {
    g_cur = p;
    if (g_down && !g_measure && (std::abs(p.x - g_from.x) > 3 || std::abs(p.y - g_from.y) > 3)) g_measure = true;
    if (g_measure && (GetKeyState(VK_SHIFT) & 0x8000)) g_cur = Constrain(g_from, p);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void Pick(bool rgb) {
    DWORD px = PixelAt(g_cur);
    BYTE r = (BYTE)(px >> 16), g = (BYTE)(px >> 8), b = (BYTE)px;
    std::wstring t = rgb ? RgbText(r, g, b) : Hex(r, g, b);
    autos::Note(L"Пипетка: " + t);
    Done(t, L"Цвет");
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (g_bits) Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_MOUSEMOVE: {
            POINT p;
            GetCursorPos(&p);
            Move(p);
            return 0;
        }
        case WM_LBUTTONDOWN:
            GetCursorPos(&g_from);
            g_cur = g_from;
            g_down = true;
            SetCapture(h);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (!g_down) return 0;
            g_down = false;
            if (g_measure) {
                int dx = std::abs(g_cur.x - g_from.x), dy = std::abs(g_cur.y - g_from.y);
                autos::Note(L"Линейка: " + Measure(dx, dy));
                Done(std::to_wstring(dx) + L"x" + std::to_wstring(dy), L"Замер");
            } else {
                Pick((wp & MK_CONTROL) != 0);
            }
            return 0;
        }
        case WM_RBUTTONUP:
            Close();
            return 0;
        case WM_KEYDOWN: {
            POINT p;
            GetCursorPos(&p);
            if (wp == VK_ESCAPE)
                Close();
            else if (wp == VK_RETURN)
                Pick((GetKeyState(VK_CONTROL) & 0x8000) != 0);
            else if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN) {
                p.x += wp == VK_LEFT ? -1 : wp == VK_RIGHT ? 1 : 0;
                p.y += wp == VK_UP ? -1 : wp == VK_DOWN ? 1 : 0;
                SetCursorPos(p.x, p.y);
                Move(p);
            }
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && g_wnd == h) PostMessageW(h, WM_CLOSE, 0, 0);  // ушли в другое окно
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg) {
    g_inst = inst;
    g_cfg = cfg;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassExW(&wc);
    g_card.Create(inst);
}

bool Capture() {
    g_virt = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    g_virt.right = g_virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_virt.bottom = g_virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    g_shot = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_shot) {
        ReleaseDC(nullptr, screen);
        return false;
    }
    g_shotDC = CreateCompatibleDC(screen);
    g_shotOld = SelectObject(g_shotDC, g_shot);
    BitBlt(g_shotDC, 0, 0, w, h, screen, g_virt.left, g_virt.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    g_bits = (DWORD*)bits;
    return true;
}

void Show() {
    if (g_wnd || !Capture()) return;
    int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
    GetCursorPos(&g_cur);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"Пипетка", WS_POPUP, g_virt.left, g_virt.top, w,
                            h, nullptr, nullptr, g_inst, nullptr);
    if (!g_wnd) return Close();
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SetFocus(g_wnd);
}

void Tick() { g_card.Tick(); }

bool Snap(POINT cur, POINT from, bool measure, const std::wstring& out) {
    if (!Capture()) return false;
    g_cur = cur, g_from = from, g_measure = measure;
    int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    Paint(mem);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    bool ok;
    {
        Bitmap shot(bmp, nullptr);
        CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        ok = shot.Save(out.c_str(), &png, nullptr) == Ok;
    }
    DeleteObject(bmp);
    Close();
    return ok;
}

void Shutdown() {
    Close();
    g_card.Destroy();
}

}  // namespace picker
