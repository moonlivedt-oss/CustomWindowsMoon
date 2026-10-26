// Предпросмотр: выделенное в Проводнике, окно с картинкой, текстом или миниатюрой Windows.
#include <dwmapi.h>
#include <exdisp.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <memory>

#include "core/config.h"
#include "core/text.h"
#include "core/themes.h"
#include "gfx/color.h"
#include "life/preview.h"

using namespace Gdiplus;

namespace preview {

namespace {

HINSTANCE g_inst = nullptr;
const Config* g_cfg = nullptr;
HWND g_wnd = nullptr;
ULONGLONG g_shownAt = 0;
std::vector<std::wstring> g_files;
size_t g_at = 0;
std::unique_ptr<Bitmap> g_img;
std::wstring g_text, g_caption;
int g_iw = 0, g_ih = 0;

// выделенное в верхнем окне Проводника; focus - на каком стоит курсор
std::vector<std::wstring> Selection(size_t& focus) {
    std::vector<std::wstring> out;
    focus = 0;
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw)))) return out;
    HWND top = nullptr;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            wchar_t cls[64];
            if (GetClassNameW(w, cls, 64) && !wcscmp(cls, L"CabinetWClass") && IsWindowVisible(w)) {
                *(HWND*)lp = w;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&top);
    long n = 0;
    sw->get_Count(&n);
    for (long i = 0; i < n && top && out.empty(); ++i) {
        VARIANT v;
        v.vt = VT_I4;
        v.lVal = i;
        IDispatch* d = nullptr;
        if (FAILED(sw->Item(v, &d)) || !d) continue;
        IWebBrowserApp* app = nullptr;
        IServiceProvider* sp = nullptr;
        IShellBrowser* sb = nullptr;
        IShellView* view = nullptr;
        IFolderView2* fv = nullptr;
        IShellItemArray* sel = nullptr;
        SHANDLE_PTR h = 0;
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&app))) && SUCCEEDED(app->get_HWND(&h)) && (HWND)h == top &&
            SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&sp))) &&
            SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) &&
            SUCCEEDED(sb->QueryActiveShellView(&view)) && SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&fv))) &&
            SUCCEEDED(fv->GetSelection(FALSE, &sel)) && sel) {
            DWORD count = 0;
            sel->GetCount(&count);
            for (DWORD k = 0; k < count && k < 500; ++k) {
                IShellItem* it = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(sel->GetItemAt(k, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    DWORD a = GetFileAttributesW(p);
                    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(p);
                }
                CoTaskMemFree(p);
                if (it) it->Release();
            }
            int focused = -1;
            if (SUCCEEDED(fv->GetFocusedItem(&focused)) && focused >= 0) {
                IShellItem* it = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(fv->GetItem(focused, IID_PPV_ARGS(&it))) &&
                    SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    auto f = std::find(out.begin(), out.end(), std::wstring(p));
                    if (f != out.end()) focus = (size_t)(f - out.begin());
                }
                CoTaskMemFree(p);
                if (it) it->Release();
            }
        }
        for (IUnknown* u : std::initializer_list<IUnknown*>{sel, fv, view, sb, sp, app, d})
            if (u) u->Release();
    }
    sw->Release();
    return out;
}

// один выделенный - листаем все файлы его папки
std::vector<std::wstring> Siblings(const std::wstring& file, size_t& at) {
    std::vector<std::wstring> out;
    std::wstring dir = file.substr(0, file.find_last_of(L'\\'));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)))
                out.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd) && out.size() < 5000);
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    auto f = std::find(out.begin(), out.end(), file);
    at = f == out.end() ? 0 : (size_t)(f - out.begin());
    if (f == out.end()) out.insert(out.begin(), file);
    return out;
}

Bitmap* ShellImage(const std::wstring& path, int size) {  // миниатюра Проводника: кадр видео, страница PDF, значок
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP hb = nullptr;
    HRESULT hr = f->GetImage({size, size}, SIIGBF_BIGGERSIZEOK, &hb);
    if (FAILED(hr)) hr = f->GetImage({256, 256}, SIIGBF_ICONONLY, &hb);
    f->Release();
    if (FAILED(hr) || !hb) return nullptr;
    DIBSECTION ds{};
    Bitmap* out = nullptr;
    if (GetObjectW(hb, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBitsPixel == 32 && ds.dsBm.bmBits) {
        int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
        out = new Bitmap(w, h, PixelFormat32bppPARGB);
        BitmapData bd{};
        Rect r(0, 0, w, h);
        if (out->LockBits(&r, ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Ok) {
            bool bottomUp = ds.dsBmih.biHeight > 0;
            for (int y = 0; y < h; ++y)
                memcpy((BYTE*)bd.Scan0 + (size_t)y * bd.Stride,
                       (BYTE*)ds.dsBm.bmBits + (size_t)(bottomUp ? h - 1 - y : y) * ds.dsBm.bmWidthBytes,
                       (size_t)w * 4);
            out->UnlockBits(&bd);
        }
    }
    DeleteObject(hb);
    return out;
}

std::wstring ReadText(const std::wstring& path) {
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"(не открыть)";
    std::string u(64 << 10, '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    if (u.size() >= 2 && (unsigned char)u[0] == 0xFF && (unsigned char)u[1] == 0xFE)
        return std::wstring((const wchar_t*)(u.data() + 2), (u.size() - 2) / 2);
    if (u.size() >= 3 && u.compare(0, 3, "\xEF\xBB\xBF") == 0) u.erase(0, 3);
    return Widen(u);
}

void Load() {
    g_img.reset();
    g_text.clear();
    g_iw = g_ih = 0;
    if (g_files.empty()) return;
    const std::wstring& p = g_files[g_at];
    Kind k = KindOf(p);
    if (k == IMAGE) {
        auto* b = new Bitmap(p.c_str());
        if (b->GetLastStatus() == Ok && b->GetWidth() > 0) {
            g_img.reset(b);
            g_iw = (int)b->GetWidth(), g_ih = (int)b->GetHeight();
        } else {
            delete b;
        }
    }
    if (k == TEXT) g_text = ReadText(p);
    if (!g_img && k != TEXT) g_img.reset(ShellImage(p, 1600));  // webp, heic, видео, PDF и прочее
    WIN32_FILE_ATTRIBUTE_DATA a{};
    long long size = GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)
                         ? ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow
                         : -1;
    g_caption = Caption(p.substr(p.find_last_of(L'\\') + 1), size, g_iw, g_ih, g_at, g_files.size());
    if (g_wnd) {
        SetWindowTextW(g_wnd, g_caption.c_str());
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

void Paint(HDC dc) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int w = rc.right, h = rc.bottom;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        ThemeInfo th = CurrentTheme(*g_cfg);
        g.Clear(ToColor(th.bg));
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        float k = GetDpiForWindow(g_wnd) / 96.0f;
        int pad = (int)(16 * k), capH = (int)(36 * k);
        RECT box{pad, pad, w - pad, h - pad - capH};
        if (g_img) {
            RECT r = Fit((int)g_img->GetWidth(), (int)g_img->GetHeight(), box);
            g.DrawImage(g_img.get(), Rect(r.left, r.top, r.right - r.left, r.bottom - r.top));
        } else if (!g_text.empty()) {
            FontFamily mono(L"Consolas");
            Font f(&mono, 13 * k, FontStyleRegular, UnitPixel);
            SolidBrush b(ToColor(th.fg));
            StringFormat sf;
            sf.SetTrimming(StringTrimmingNone);
            g.DrawString(
                g_text.c_str(), (INT)std::min<size_t>(g_text.size(), 20000), &f,
                RectF((REAL)box.left, (REAL)box.top, (REAL)(box.right - box.left), (REAL)(box.bottom - box.top)), &sf,
                &b);
        } else {
            FontFamily fam(L"Segoe UI");
            Font f(&fam, 15 * k, FontStyleRegular, UnitPixel);
            SolidBrush b(ToColor(th.muted));
            StringFormat c;
            c.SetAlignment(StringAlignmentCenter);
            c.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(L"Предпросмотра нет - Enter откроет файл", -1, &f, RectF(0, 0, (REAL)w, (REAL)(h - capH)), &c,
                         &b);
        }
        FontFamily fam(L"Segoe UI");
        Font f(&fam, 13 * k, FontStyleRegular, UnitPixel);
        SolidBrush b(MixColor(ToColor(th.muted), ToColor(th.fg), 0.3));
        StringFormat sf;
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetTrimming(StringTrimmingEllipsisPath);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        std::wstring cap = g_caption + L"      стрелки - соседние, Enter - открыть, Esc - закрыть";
        g.DrawString(cap.c_str(), -1, &f, RectF((REAL)pad, (REAL)(h - capH), (REAL)(w - 2 * pad), (REAL)capH), &sf, &b);
    }
    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void Close() {
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
    g_img.reset();
    g_text.clear();
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE || wp == VK_SPACE) {
                PostMessageW(h, WM_CLOSE, 0, 0);
            } else if ((wp == VK_RIGHT || wp == VK_DOWN) && g_at + 1 < g_files.size()) {
                ++g_at;
                Load();
            } else if ((wp == VK_LEFT || wp == VK_UP) && g_at > 0) {
                --g_at;
                Load();
            } else if (wp == VK_RETURN && !g_files.empty()) {
                ShellExecuteW(nullptr, L"open", g_files[g_at].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && GetTickCount64() - g_shownAt > 500)  // щелчок мимо; не сразу после показа
                PostMessageW(h, WM_CLOSE, 0, 0);
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
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MoonRadialPreview";
    RegisterClassExW(&wc);
}

void ShowFiles(const std::vector<std::wstring>& files, size_t at) {
    if (files.empty()) return;
    Close();
    g_files = files;
    g_at = std::min(at, files.size() - 1);
    if (g_files.size() == 1) g_files = Siblings(files[0], g_at);
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int w = (wa.right - wa.left) * 4 / 5, h = (wa.bottom - wa.top) * 4 / 5;
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"MoonRadialPreview", L"Предпросмотр",
                            WS_POPUP | WS_BORDER, wa.left + (wa.right - wa.left - w) / 2,
                            wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, g_inst, nullptr);
    if (!g_wnd) return;
    DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(g_wnd, 33, &round, sizeof(round));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));
    Load();
    g_shownAt = GetTickCount64();
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}

void Show() {
    size_t focus = 0;
    std::vector<std::wstring> sel = Selection(focus);
    if (sel.empty()) return;
    ShowFiles(sel, focus);
}

void Shutdown() { Close(); }

}  // namespace preview
