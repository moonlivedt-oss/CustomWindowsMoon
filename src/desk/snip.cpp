#include "desk/snip.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>

#include "core/mem.h"
#include "core/fx.h"
#include "core/config.h"
#include "core/night.h"
#include "core/text.h"
#include "desk/ocr.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialSnip";
constexpr UINT WM_OCR_DONE = WM_APP + 1;
constexpr float kW = 420, kH = 150, kM = 26;
constexpr ULONGLONG kShowMs = 7000;
const CLSID kPng = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};

std::wstring OwnerExe() {
    HWND w = GetClipboardOwner();
    if (!w) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring r;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) r = Lower(PathFindFileNameW(buf));
    CloseHandle(p);
    return r;
}

// DIB из буфера обмена -> картинка GDI+ (через BMP-файл в памяти).
std::unique_ptr<Bitmap> FromDib(const BYTE* dib, SIZE_T size) {
    if (size < sizeof(BITMAPINFOHEADER)) return nullptr;
    auto* bih = (const BITMAPINFOHEADER*)dib;
    DWORD colors = bih->biClrUsed ? bih->biClrUsed : (bih->biBitCount <= 8 ? 1u << bih->biBitCount : 0);
    DWORD masks = (bih->biSize == sizeof(BITMAPINFOHEADER) && bih->biCompression == BI_BITFIELDS) ? 12 : 0;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfSize = (DWORD)(sizeof(fh) + size);
    fh.bfOffBits = (DWORD)(sizeof(fh) + bih->biSize + masks + colors * 4);
    std::vector<BYTE> file(sizeof(fh) + size);
    memcpy(file.data(), &fh, sizeof(fh));
    memcpy(file.data() + sizeof(fh), dib, size);
    IStream* st = SHCreateMemStream(file.data(), (UINT)file.size());
    if (!st) return nullptr;
    std::unique_ptr<Bitmap> tmp(Bitmap::FromStream(st));
    std::unique_ptr<Bitmap> out;
    if (tmp && tmp->GetLastStatus() == Ok) {
        // своя копия: исходная держит поток
        out = std::make_unique<Bitmap>((INT)tmp->GetWidth(), (INT)tmp->GetHeight(), PixelFormat32bppRGB);
        Graphics g(out.get());
        g.DrawImage(tmp.get(), 0, 0, (INT)tmp->GetWidth(), (INT)tmp->GetHeight());
    }
    tmp.reset();
    st->Release();
    return out;
}

std::vector<uint8_t> ToPng(Bitmap* b) {
    std::vector<uint8_t> out;
    IStream* st = SHCreateMemStream(nullptr, 0);
    if (!st) return out;
    if (b->Save(st, &kPng, nullptr) == Ok) {
        STATSTG s{};
        st->Stat(&s, STATFLAG_NONAME);
        out.resize((size_t)s.cbSize.QuadPart);
        LARGE_INTEGER zero{};
        st->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG got = 0;
        st->Read(out.data(), (ULONG)out.size(), &got);
        out.resize(got);
    }
    st->Release();
    return out;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr);
    CloseHandle(f);
    return ok && put == data.size();
}

bool CopyText(HWND owner, const std::wstring& s) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
    return h != nullptr;
}

}  // namespace

void SnipCard::Create(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClass, L"Снимок",
                           WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    AddClipboardFormatListener(wnd_);
}

void SnipCard::Destroy() {
    if (wnd_) RemoveClipboardFormatListener(wnd_);
}

void SnipCard::Apply(const Config& cfg) {
    enabled_ = cfg.snipCard;
    theme_ = CurrentTheme(cfg);
    dirty_ = true;
}

std::wstring SnipCard::SaveFolder() const {
    for (const KNOWNFOLDERID* id : {&FOLDERID_Screenshots, &FOLDERID_Pictures}) {
        PWSTR p = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_CREATE, nullptr, &p))) dir = p;
        CoTaskMemFree(p);
        if (!dir.empty()) return dir;
    }
    return L"";
}

void SnipCard::OnClipboard() {
    if (!enabled_) return;
    std::wstring owner = OwnerExe();
    if (owner != L"screenclippinghost.exe" && owner != L"snippingtool.exe") return;
    if (!IsClipboardFormatAvailable(CF_DIB)) return;
    bool opened = false;
    for (int i = 0; i < 10 && !(opened = OpenClipboard(wnd_)); ++i) Sleep(20);  // «Ножницы» ещё держат буфер
    if (!opened) return;
    std::unique_ptr<Bitmap> b;
    if (HANDLE h = GetClipboardData(CF_DIB)) {
        if (auto* p = (const BYTE*)GlobalLock(h)) {
            b = FromDib(p, GlobalSize(h));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (!b) return;
    shot_ = std::move(b);
    shotW_ = (int)shot_->GetWidth();
    shotH_ = (int)shot_->GetHeight();
    png_ = ToPng(shot_.get());
    status_.clear();
    savedPath_.clear();
    busy_ = false;
    Show();
}

void SnipCard::Show() {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    work_ = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    k_ = dx / 96.0;
    // превью заранее уменьшено под рамку: кадр рисуется быстро
    int pw = (int)std::lround(150 * k_), ph = (int)std::lround(122 * k_);
    thumb_ = std::make_unique<Bitmap>(pw, ph, PixelFormat32bppPARGB);
    {
        Graphics g(thumb_.get());
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(255, 14, 15, 20));
        float s = std::min(pw / (float)shotW_, ph / (float)shotH_);
        float w = shotW_ * s, h = shotH_ * s;
        g.DrawImage(shot_.get(), RectF((pw - w) / 2, (ph - h) / 2, w, h));
    }
    open_ = true;
    until_ = GetTickCount64() + kShowMs;
    dirty_ = true;
}

RectF SnipCard::Rect(Hit h) const {
    float k = (float)k_, m = kM * k;
    float x0 = m + 178 * k, right = m + (kW - 16) * k;
    switch (h) {
        case H_PREVIEW:
            return RectF(m + 14 * k, m + 14 * k, 150 * k, 122 * k);
        case H_CLOSE:
            return RectF(right - 26 * k, m + 12 * k, 26 * k, 26 * k);
        case H_SAVE:
            return RectF(x0, m + 102 * k, 106 * k, 34 * k);
        case H_TEXT:
            return RectF(x0 + 114 * k, m + 102 * k, 106 * k, 34 * k);
        case H_BODY:
            return RectF(m, m, kW * k, kH * k);
        default:
            return RectF();
    }
}

SnipCard::Hit SnipCard::HitAt(POINT p) const {
    PointF pt((REAL)p.x, (REAL)p.y);
    for (Hit h : {H_CLOSE, H_SAVE, H_TEXT, H_PREVIEW})
        if (Rect(h).Contains(pt)) return h;
    return Rect(H_BODY).Contains(pt) ? H_BODY : H_NONE;
}

void SnipCard::Tick() {
    if (!wnd_ || (slide_ <= 0 && !open_)) return;
    ULONGLONG now = GetTickCount64();
    bool hovered = hot_ != H_NONE;
    if (hovered || busy_) until_ = std::max(until_, now + 2500);  // пока на карточку смотрят, она не уходит
    if (open_ && now > until_) open_ = false;
    bool anim = false;
    auto step = [&](float& v, float t, float up, float down) {
        if (v == t) return;
        v = v < t ? std::min(t, v + fx::Step(up)) : std::max(t, v - fx::Step(down));
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
    if (anim || dirty_ || busy_) Render();
}

void SnipCard::Render() {
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
        for (int i = 6; i >= 1; --i) {
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
        LinearGradientBrush tint(PointF(body.X, 0), PointF(body.X + body.Width, 0), WithAlpha(ac, 26),
                                 WithAlpha(ac, 0));
        g.FillPath(&tint, &path);
        LinearGradientBrush shine(PointF(0, body.Y), PointF(0, body.Y + body.Height * 0.6f), Color(22, 255, 255, 255),
                                  Color(0, 255, 255, 255));
        g.FillPath(&shine, &path);
        Pen rim(Color(40, 255, 255, 255), 1 * k);
        g.DrawPath(&rim, &path);

        // превью
        RectF pv = Rect(H_PREVIEW);
        GraphicsPath pp;
        RoundRectPath(pp, pv, 12 * k);
        if (thumb_) {
            GraphicsState st = g.Save();
            g.SetClip(&pp);
            g.DrawImage(thumb_.get(), pv);
            if (hover_[H_PREVIEW] > 0) {
                SolidBrush hb(Color((BYTE)(40 * hover_[H_PREVIEW]), 255, 255, 255));
                g.FillRectangle(&hb, pv);
            }
            g.Restore(st);
        }
        Pen pr(Color(50, 255, 255, 255), 1 * k);
        g.DrawPath(&pr, &pp);

        // текст
        FontFamily fam(L"Segoe UI");
        Font fT(Family(), 14.5f * k, FontStyleRegular, UnitPixel);
        Font fS(&fam, 11.5f * k, FontStyleRegular, UnitPixel);
        Font fB(Family(), 12 * k, FontStyleRegular, UnitPixel);
        StringFormat sf;
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        float x0 = Rect(H_SAVE).X, tw = body.GetRight() - 16 * k - x0;
        SolidBrush tB(fg), mB(muted);
        g.DrawString(L"Снимок области", -1, &fT, RectF(x0, body.Y + 14 * k, tw - 30 * k, 24 * k), &sf, &tB);
        std::wstring size = std::to_wstring(shotW_) + L" x " + std::to_wstring(shotH_);
        g.DrawString(size.c_str(), -1, &fS, RectF(x0, body.Y + 37 * k, tw, 18 * k), &sf, &mB);
        if (!status_.empty()) {
            StringFormat wf;
            wf.SetTrimming(StringTrimmingEllipsisWord);
            SolidBrush sB(busy_         ? muted
                          : statusGood_ ? MixColor(ac, Color(255, 255, 255), 0.35)
                                        : Color(240, 140, 120));
            g.DrawString(status_.c_str(), -1, &fS, RectF(x0, body.Y + 58 * k, tw, 36 * k), &wf, &sB);
        }

        // кнопки: «Текст» главная, «Сохранить» стеклянная
        for (Hit h : {H_SAVE, H_TEXT}) {
            RectF r = Rect(h);
            float hv = hover_[h];
            GraphicsPath bp;
            RoundRectPath(bp, r, r.Height / 2);
            bool primary = h == H_TEXT;
            SolidBrush bb(primary ? MixColor(ac, Color(255, 255, 255), hv * 0.15)
                                  : Color((BYTE)(26 + 30 * hv), 255, 255, 255));
            g.FillPath(&bb, &bp);
            if (!primary) {
                Pen bpn(Color(50, 255, 255, 255), 1 * k);
                g.DrawPath(&bpn, &bp);
            }
            std::wstring label = h == H_TEXT          ? (busy_ ? L"Читаю..." : L"Текст")
                                 : savedPath_.empty() ? L"Сохранить"
                                                      : L"Папка";
            double lum = (0.299 * ac.GetR() + 0.587 * ac.GetG() + 0.114 * ac.GetB()) / 255;
            SolidBrush lb(primary ? (lum > 0.72 ? Color(255, 24, 20, 28) : Color(255, 255, 255, 255)) : fg);
            StringFormat cf;
            cf.SetAlignment(StringAlignmentCenter);
            cf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(label.c_str(), -1, &fB, r, &cf, &lb);
        }
        // закрыть
        RectF cr = Rect(H_CLOSE);
        if (hover_[H_CLOSE] > 0) {
            SolidBrush hb(Color((BYTE)(36 * hover_[H_CLOSE]), 255, 255, 255));
            g.FillEllipse(&hb, cr);
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

void SnipCard::Save(bool thenOpen) {
    if (png_.empty()) return;
    if (!savedPath_.empty()) {
        if (thenOpen) {
            std::wstring arg = L"/select,\"" + savedPath_ + L"\"";
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[80];
    swprintf(name, 80, L"Снимок %04d-%02d-%02d %02d%02d%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    std::wstring dir = SaveFolder();
    std::wstring path = dir + L"\\" + name;
    if (!dir.empty() && WriteFileBytes(path, png_)) {
        savedPath_ = path;
        status_ = std::wstring(L"Сохранено: ") + name;
        statusGood_ = true;
    } else {
        status_ = L"Не удалось сохранить";
        statusGood_ = false;
    }
    dirty_ = true;
}

LRESULT CALLBACK SnipCard::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* self = (SnipCard*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self && self->wnd_ == h) return self->Handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT SnipCard::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            OnClipboard();
            return 0;
        case WM_OCR_DONE: {
            std::unique_ptr<std::wstring> text((std::wstring*)lp);
            busy_ = false;
            if (wp == 0 && text) {
                CopyText(wnd_, *text);
                int lines = 1 + (int)std::count(text->begin(), text->end(), L'\n');
                std::wstring first = text->substr(0, text->find(L'\r'));
                status_ = L"В буфере, " + Plural(lines, L"строка", L"строки", L"строк") + L": " + first;
                statusGood_ = true;
            } else {
                status_ = text ? *text : L"Распознать не получилось";
                statusGood_ = false;
            }
            dirty_ = true;
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
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
            if (h == H_CLOSE) open_ = false;
            if (h == H_SAVE) Save(true);
            if (h == H_TEXT && !busy_ && !png_.empty()) {
                busy_ = true;
                status_ = L"Распознаю текст...";
                statusGood_ = true;
                ocr::RecognizeAsync(png_, wnd_, WM_OCR_DONE);
                dirty_ = true;
            }
            if (h == H_PREVIEW && !png_.empty()) {
                // открыть снимок в просмотрщике: временный файл
                wchar_t tmp[MAX_PATH];
                GetTempPathW(MAX_PATH, tmp);
                std::wstring p = std::wstring(tmp) + L"radial-snip.png";
                if (WriteFileBytes(p, png_))
                    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}
