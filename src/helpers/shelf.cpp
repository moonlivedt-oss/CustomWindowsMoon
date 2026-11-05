// Полка у края экрана: окно со списком, приём файлов броском, вынос всех одним перетаскиванием.
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>

#include "core/config.h"
#include "core/text.h"
#include "core/themes.h"
#include "gfx/color.h"
#include "helpers/shelf.h"
#include "phone/phone.h"

using namespace Gdiplus;

namespace shelf {

namespace {

HINSTANCE g_inst = nullptr;
const Config* g_cfg = nullptr;
HWND g_wnd = nullptr;
std::vector<std::wstring> g_files;
POINT g_down{};
bool g_pressed = false;
int g_hot = -1;
constexpr int kW = 250, kHead = 118, kRow = 26;  // до масштаба
enum { B_DRAG = -10, B_PHONE = -11, B_CLEAR = -12, B_NONE = -1 };

float K() { return g_wnd ? GetDpiForWindow(g_wnd) / 96.0f : 1.0f; }

std::vector<std::wstring> DropFiles(IDataObject* d) {
    std::vector<std::wstring> out;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (!d || FAILED(d->GetData(&fe, &sm))) return out;
    if (auto* hd = (HDROP)GlobalLock(sm.hGlobal)) {
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n && i < 500; ++i) {
            UINT len = DragQueryFileW(hd, i, nullptr, 0);
            std::wstring f(len + 1, L' ');
            DragQueryFileW(hd, i, f.data(), len + 1);
            f.resize(len);
            out.push_back(f);
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return out;
}

class Target : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // живёт всё время программы
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        ok_ = !DropFiles(d).empty();
        *eff = ok_ ? DROPEFFECT_LINK : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override {
        *eff = ok_ ? DROPEFFECT_LINK : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        std::vector<std::wstring> f = DropFiles(d);
        *eff = f.empty() ? DROPEFFECT_NONE : DROPEFFECT_LINK;  // файлы остаются на месте - на полке только ссылки
        if (!f.empty()) Add(f);
        return S_OK;
    }

private:
    ULONG refs_ = 1;
    bool ok_ = false;
};
Target g_target;

class Source : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
Source g_source;

void DragAll() {  // все файлы полки - одним перетаскиванием (из разных папок)
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const std::wstring& f : g_files)
        if (PIDLIST_ABSOLUTE p = ILCreateFromPathW(f.c_str())) ids.push_back(p);
    IShellItemArray* arr = nullptr;
    IDataObject* data = nullptr;
    if (!ids.empty() &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &arr)) &&
        SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) {
        DWORD eff = 0;
        HRESULT hr = DoDragDrop(data, &g_source, DROPEFFECT_MOVE | DROPEFFECT_COPY | DROPEFFECT_LINK, &eff);
        if (hr == DRAGDROP_S_DROP && eff == DROPEFFECT_MOVE) {  // перенесли - полка пустеет (то, что уехало)
            g_files.erase(std::remove_if(g_files.begin(), g_files.end(),
                                         [](const std::wstring& f) {
                                             return GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES;
                                         }),
                          g_files.end());
        }
    }
    if (data) data->Release();
    if (arr) arr->Release();
    for (PIDLIST_ABSOLUTE p : ids) ILFree(p);
    // «оптимизированный» перенос возвращает NONE, а файлы уже уехали - сверим ещё раз
    g_files.erase(
        std::remove_if(g_files.begin(), g_files.end(),
                       [](const std::wstring& f) { return GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES; }),
        g_files.end());
}

void Place() {
    if (!g_wnd) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
    float k = K();
    int w = (int)(kW * k);
    int h = (int)((kHead + std::max<size_t>(1, g_files.size()) * kRow + 12) * k);
    h = std::min<int>(h, (mi.rcWork.bottom - mi.rcWork.top) * 4 / 5);
    SetWindowPos(g_wnd, HWND_TOPMOST, mi.rcWork.right - w - (int)(8 * k),
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, SWP_NOACTIVATE);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

RECT Btn(int id) {
    float k = K();
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int x = (int)(10 * k), w = rc.right - 2 * x;
    if (id == B_DRAG) return {x, (int)(38 * k), x + w, (int)(76 * k)};
    int half = (w - (int)(8 * k)) / 2;
    if (id == B_PHONE) return {x, (int)(82 * k), x + half, (int)(108 * k)};
    return {rc.right - x - half, (int)(82 * k), rc.right - x, (int)(108 * k)};
}

int HitAt(POINT p) {
    for (int id : {B_DRAG, B_PHONE, B_CLEAR}) {
        RECT r = Btn(id);
        if (PtInRect(&r, p)) return id;
    }
    float k = K();
    int y0 = (int)(kHead * k), row = (int)(kRow * k);
    if (p.y >= y0) {
        int i = (p.y - y0) / row;
        if (i >= 0 && i < (int)g_files.size()) return i;
    }
    return B_NONE;
}

void Paint(HDC dc) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        ThemeInfo th = CurrentTheme(*g_cfg);
        Color bg = ToColor(th.bg), fg = ToColor(th.fg), ac = ToColor(th.ac), mu = ToColor(th.muted);
        g.Clear(bg);
        float k = K();
        FontFamily fam(L"Segoe UI");
        Font fT(&fam, 14 * k, FontStyleBold, UnitPixel), fS(&fam, 12.5f * k, FontStyleRegular, UnitPixel);
        SolidBrush tb(fg), mb(MixColor(mu, fg, 0.3));
        g.DrawString(Title(g_files).c_str(), -1, &fT, PointF(10 * k, 10 * k), &tb);
        StringFormat c;
        c.SetAlignment(StringAlignmentCenter);
        c.SetLineAlignment(StringAlignmentCenter);
        auto button = [&](int id, const wchar_t* text, bool primary) {
            RECT r = Btn(id);
            SolidBrush b(primary ? WithAlpha(ac, (BYTE)(g_hot == id ? 255 : 210))
                                 : MixColor(bg, fg, g_hot == id ? 0.22 : 0.12));
            g.FillRectangle(&b, (REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
            SolidBrush t(primary ? Color(255, 255, 255, 255) : fg);
            g.DrawString(text, -1, &fS,
                         RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)), &c, &t);
        };
        button(B_DRAG, g_files.empty() ? L"Бросьте сюда файлы" : L"Тащить все в папку", true);
        button(B_PHONE, L"На телефон", false);
        button(B_CLEAR, L"Очистить", false);
        StringFormat one;
        one.SetFormatFlags(StringFormatFlagsNoWrap);
        one.SetTrimming(StringTrimmingEllipsisPath);
        one.SetLineAlignment(StringAlignmentCenter);
        int y = (int)(kHead * k), row = (int)(kRow * k);
        for (size_t i = 0; i < g_files.size() && y + row <= rc.bottom; ++i, y += row) {
            if ((int)i == g_hot) {
                SolidBrush hb(MixColor(bg, fg, 0.1));
                g.FillRectangle(&hb, 0.0f, (REAL)y, (REAL)rc.right, (REAL)row);
            }
            const std::wstring& f = g_files[i];
            std::wstring name = (int)i == g_hot ? L"убрать с полки: " + f.substr(f.find_last_of(L'\\') + 1)
                                                : f.substr(f.find_last_of(L'\\') + 1);
            g.DrawString(name.c_str(), -1, &fS, RectF(10 * k, (REAL)y, rc.right - 20 * k, (REAL)row), &one,
                         (int)i == g_hot ? &tb : &mb);
        }
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
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
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            int hot = HitAt(p);
            if (hot != g_hot) {
                g_hot = hot;
                InvalidateRect(h, nullptr, FALSE);
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
            }
            if (g_pressed && !g_files.empty() && (std::abs(p.x - g_down.x) > 6 || std::abs(p.y - g_down.y) > 6)) {
                g_pressed = false;
                DragAll();
                Place();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hot = B_NONE;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            g_pressed = HitAt(p) == B_DRAG;
            g_down = p;
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            bool was = g_pressed;
            g_pressed = false;
            int hit = HitAt(p);
            if (hit == B_PHONE && !g_files.empty())
                phone::Offer(g_files);
            else if (hit == B_CLEAR)
                g_files.clear();
            else if (hit >= 0)
                g_files.erase(g_files.begin() + hit);
            (void)was;
            if (hit != B_DRAG && hit != B_NONE) Place();
            return 0;
        }
        case WM_RBUTTONUP:
            ShowWindow(h, SW_HIDE);
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
    wc.lpszClassName = L"MoonRadialShelf";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Полка",
                            WS_POPUP | WS_BORDER, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
    if (g_wnd) RegisterDragDrop(g_wnd, &g_target);
}

void Toggle() {
    if (!g_wnd) return;
    if (IsWindowVisible(g_wnd)) {
        ShowWindow(g_wnd, SW_HIDE);
        return;
    }
    Place();
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
}

void Add(const std::vector<std::wstring>& files) {
    Merge(g_files, files);
    if (!g_wnd) return;
    Place();
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
}

void Shutdown() {
    if (g_wnd) {
        RevokeDragDrop(g_wnd);
        DestroyWindow(g_wnd);
    }
    g_wnd = nullptr;
}

}  // namespace shelf
