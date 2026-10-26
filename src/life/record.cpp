// Запись области экрана: выбор области поверх экрана, процесс ffmpeg, рамка и плашка «Стоп», итог.
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>

#include <algorithm>
#include <ctime>

#include "app/autos.h"
#include "core/config.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "gfx/color.h"
#include "life/convert.h"
#include "life/record.h"
#include "tools/tools.h"

using namespace Gdiplus;

namespace record {

namespace {

constexpr int kMaxSec = 600;

HINSTANCE g_inst = nullptr;
const Config* g_cfg = nullptr;
BoostCard g_card;
HWND g_pick = nullptr, g_bar = nullptr, g_frame[4] = {};
RECT g_virt{}, g_rect{};
POINT g_a{}, g_b{};
bool g_drag = false;
PROCESS_INFORMATION g_proc{};
HANDLE g_in = nullptr;  // stdin ffmpeg: «q» - закончить
std::wstring g_out;
ULONGLONG g_start = 0, g_barAt = 0;

COLORREF Accent() { return g_cfg ? CurrentTheme(*g_cfg).ac : RGB(200, 50, 63); }

// затемнённый экран с «окном» выбранной области (попиксельная прозрачность)
void PaintPick() {
    int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.Clear(Color(110, 0, 0, 0));
        if (g_drag) {
            RECT r = Normalize(g_a, g_b);
            g.SetCompositingMode(CompositingModeSourceCopy);
            SolidBrush hole(Color(1, 0, 0, 0));  // почти прозрачно, но щелчки ловятся
            RectF rr((REAL)(r.left - g_virt.left), (REAL)(r.top - g_virt.top), (REAL)(r.right - r.left),
                     (REAL)(r.bottom - r.top));
            g.FillRectangle(&hole, rr);
            g.SetCompositingMode(CompositingModeSourceOver);
            Pen pen(ToColor(Accent()), 2);
            g.DrawRectangle(&pen, rr);
            FontFamily fam(L"Segoe UI");
            Font f(&fam, 13, FontStyleRegular, UnitPixel);
            SolidBrush tb(Color(255, 255, 255, 255));
            std::wstring t = std::to_wstring(r.right - r.left) + L" x " + std::to_wstring(r.bottom - r.top);
            g.DrawString(t.c_str(), -1, &f, PointF(rr.X, std::max(0.0f, rr.Y - 20)), &tb);
        } else {
            FontFamily fam(L"Segoe UI");
            Font f(&fam, 16, FontStyleRegular, UnitPixel);
            SolidBrush tb(Color(255, 255, 255, 255));
            POINT c;
            GetCursorPos(&c);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
            g.DrawString(
                L"Запись экрана: выделите область рамкой. Esc - отмена", -1, &f,
                PointF((REAL)(mi.rcMonitor.left - g_virt.left) + 24, (REAL)(mi.rcMonitor.top - g_virt.top) + 24), &tb);
        }
    }
    POINT src{0, 0}, dst{g_virt.left, g_virt.top};
    SIZE sz{w, h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_pick, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void ClosePick() {
    if (g_pick) DestroyWindow(g_pick);
    g_pick = nullptr;
    g_drag = false;
}

void Finish();

void Start(RECT r) {
    std::wstring ff = convert::FfmpegPath(L"ffmpeg.exe");
    if (ff.empty()) {
        g_card.kind = L"-";
        g_card.Show(BoostCard::DONE, L"Записи нет: нужен ffmpeg", L"winget install Gyan.FFmpeg", CurrentTheme(*g_cfg),
                    8000, Toast::G_FAIL);
        return;
    }
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    dir += L"\\Screen Recordings";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t name[64];
    swprintf(name, 64, L"\\Запись %04d-%02d-%02d %02d-%02d-%02d.mp4", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
    g_out = dir + name;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = si.hStdError = nul;
    std::wstring cmd = L"\"" + ff + L"\" " + GrabArgs(r, g_out, kMaxSec);
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    BOOL ok =
        CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &g_proc);
    CloseHandle(rd);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(wr);
        return;
    }
    g_in = wr;
    g_rect = r;
    g_start = GetTickCount64();
    // рамка снаружи области: в запись не попадёт
    RECT f[4] = {{r.left - 3, r.top - 3, r.right + 3, r.top - 1},
                 {r.left - 3, r.bottom + 1, r.right + 3, r.bottom + 3},
                 {r.left - 3, r.top - 1, r.left - 1, r.bottom + 1},
                 {r.right + 1, r.top - 1, r.right + 3, r.bottom + 1}};
    for (int i = 0; i < 4; ++i) {
        g_frame[i] = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                     L"MoonRadialRecFrame", L"", WS_POPUP, f[i].left, f[i].top, f[i].right - f[i].left,
                                     f[i].bottom - f[i].top, nullptr, nullptr, g_inst, nullptr);
        ShowWindow(g_frame[i], SW_SHOWNOACTIVATE);
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    int bw = 210, bh = 34, bx = std::min<LONG>(r.right - bw, mi.rcWork.right - bw);
    int by = r.bottom + 8 + bh <= mi.rcWork.bottom ? r.bottom + 8 : r.top - 8 - bh;
    if (by < mi.rcWork.top) by = mi.rcWork.bottom - bh - 8;  // область на весь экран - плашка поверх, у края
    g_bar =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"MoonRadialRecBar", L"Запись", WS_POPUP,
                        std::max<LONG>(bx, mi.rcWork.left), by, bw, bh, nullptr, nullptr, g_inst, nullptr);
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    autos::Note(L"Запись экрана: начата, " + std::to_wstring(r.right - r.left) + L"x" +
                std::to_wstring(r.bottom - r.top));
}

void Finish() {
    if (!g_proc.hProcess) return;
    if (g_in) {
        DWORD w = 0;
        WriteFile(g_in, "q", 1, &w, nullptr);
        CloseHandle(g_in);
        g_in = nullptr;
    }
    if (WaitForSingleObject(g_proc.hProcess, 8000) != WAIT_OBJECT_0) TerminateProcess(g_proc.hProcess, 1);
    CloseHandle(g_proc.hProcess);
    CloseHandle(g_proc.hThread);
    g_proc = {};
    for (HWND& f : g_frame) {
        if (f) DestroyWindow(f);
        f = nullptr;
    }
    if (g_bar) DestroyWindow(g_bar);
    g_bar = nullptr;
    int sec = (int)((GetTickCount64() - g_start) / 1000);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    bool ok = GetFileAttributesExW(g_out.c_str(), GetFileExInfoStandard, &a) && (a.nFileSizeLow || a.nFileSizeHigh);
    ULONGLONG size = ok ? ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow : 0;
    autos::Note(L"Запись экрана: " + (ok ? g_out + L", " + Clock(sec) : std::wstring(L"не записалась")));
    g_card.kind = L"-";
    if (!ok) {
        g_card.Show(BoostCard::DONE, L"Запись не получилась", L"ffmpeg ничего не записал", CurrentTheme(*g_cfg), 8000,
                    Toast::G_FAIL);
        return;
    }
    std::wstring out = g_out;
    g_card.yesLabel = L"Показать";
    g_card.noLabel = L"Превратить";
    g_card.onYes = [out] {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + out + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    };
    g_card.onNo = [out] { convert::Drop({out}); };
    g_card.Show(BoostCard::OFFER, L"Записано " + Clock(sec) + L", " + tools::Bytes(size),
                L"«Превратить» - до 8 или 25 МБ под Discord, в GIF", CurrentTheme(*g_cfg), 20000, Toast::G_OK);
}

LRESULT CALLBACK PickProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_LBUTTONDOWN:
            GetCursorPos(&g_a);
            g_b = g_a;
            g_drag = true;
            SetCapture(h);
            PaintPick();
            return 0;
        case WM_MOUSEMOVE:
            if (g_drag) {
                GetCursorPos(&g_b);
                PaintPick();
            }
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (!g_drag) return 0;
            GetCursorPos(&g_b);
            RECT r = Normalize(g_a, g_b);
            ClosePick();
            if (r.right - r.left >= 16 && r.bottom - r.top >= 16) Start(r);
            return 0;
        }
        case WM_RBUTTONUP:
            ClosePick();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) ClosePick();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK BarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(28, 24, 32));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            HBRUSH dot = CreateSolidBrush(RGB(220, 50, 60));
            HGDIOBJ old = SelectObject(dc, dot);
            Ellipse(dc, 12, 11, 24, 23);
            SelectObject(dc, old);
            DeleteObject(dot);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 236, 244));
            HFONT f =
                CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ of = SelectObject(dc, f);
            std::wstring t = L"Запись " + Clock((int)((GetTickCount64() - g_start) / 1000)) + L"    Стоп";
            RECT tr{32, 0, rc.right, rc.bottom};
            DrawTextW(dc, t.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, of);
            DeleteObject(f);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_LBUTTONUP:
            PostMessageW(h, WM_APP + 1, 0, 0);
            return 0;
        case WM_APP + 1:
            Finish();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK FrameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(Accent());
        FillRect((HDC)wp, &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg) {
    g_inst = inst;
    g_cfg = cfg;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.lpfnWndProc = PickProc;
    wc.lpszClassName = L"MoonRadialRecPick";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = BarProc;
    wc.lpszClassName = L"MoonRadialRecBar";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = FrameProc;
    wc.lpszClassName = L"MoonRadialRecFrame";
    RegisterClassExW(&wc);
    g_card.Create(inst);
}

void Toggle() {
    if (g_proc.hProcess) return Finish();
    if (g_pick) return;
    g_virt = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    g_virt.right = g_virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_virt.bottom = g_virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_pick = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"MoonRadialRecPick", L"Запись экрана",
                             WS_POPUP, g_virt.left, g_virt.top, g_virt.right - g_virt.left, g_virt.bottom - g_virt.top,
                             nullptr, nullptr, g_inst, nullptr);
    if (!g_pick) return;
    PaintPick();
    ShowWindow(g_pick, SW_SHOW);
    SetForegroundWindow(g_pick);
}

void Tick() {
    g_card.Tick();
    if (!g_proc.hProcess) return;
    if (WaitForSingleObject(g_proc.hProcess, 0) == WAIT_OBJECT_0) return Finish();  // 10 минут вышли
    ULONGLONG now = GetTickCount64();
    if (g_bar && now - g_barAt >= 1000) {
        g_barAt = now;
        InvalidateRect(g_bar, nullptr, FALSE);
    }
}

void Shutdown() {
    Finish();
    ClosePick();
    g_card.Destroy();
}

}  // namespace record

namespace record {

std::wstring TestRun(RECT r, int ms, const std::wstring& snap) {  // --record-live: запись без выбора мышью
    Start(r);
    if (!g_proc.hProcess) return L"не запустилась";
    for (ULONGLONG until = GetTickCount64() + (ULONGLONG)ms; GetTickCount64() < until;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Tick();
        Sleep(30);
    }
    if (!snap.empty()) {  // рамка и плашка: снимок экрана вокруг области
        int x = r.left - 20, y = r.top - 20, w = r.right - r.left + 40, h = r.bottom - r.top + 80;
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
        SelectObject(mem, old);
        {
            Bitmap shot(bmp, nullptr);
            CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            shot.Save(snap.c_str(), &png, nullptr);
        }
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    std::wstring out = g_out;
    Finish();
    return out;
}

}  // namespace record
