#include "desk/widget.h"

#include <shlwapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/mem.h"
#include "core/fx.h"
#include "core/config.h"
#include "core/night.h"
#include "core/files.h"
#include "desk/corners.h"
#include "desk/volume.h"
#include "desk/widget_internal.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/icons.h"

using namespace Gdiplus;

using namespace widget_detail;

namespace {

// Картинка из байтов (PNG, JPG), обрезанная по центру до пропорций w:h и уменьшенная до w x h.
std::unique_ptr<Bitmap> Fit(const std::vector<uint8_t>& bytes, int w, int h) {
    IStream* st = SHCreateMemStream(bytes.data(), (UINT)bytes.size());
    if (!st) return nullptr;
    std::unique_ptr<Bitmap> src(Bitmap::FromStream(st));
    if (!src || src->GetLastStatus() != Ok || !src->GetWidth()) {
        src.reset();
        st->Release();
        return nullptr;
    }
    float sw = (float)src->GetWidth(), sh = (float)src->GetHeight();
    float k = std::max(w / sw, h / sh);
    float cw = w / k, ch = h / k;
    auto out = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    {
        Graphics g(out.get());
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(src.get(), RectF(0, 0, (REAL)w, (REAL)h), (sw - cw) / 2, (sh - ch) / 2, cw, ch, UnitPixel);
    }
    src.reset();
    st->Release();
    return out;
}

void BlurOpaque(Bitmap* b, int radius) {
    BitmapData d;
    Rect r(0, 0, (INT)b->GetWidth(), (INT)b->GetHeight());
    if (b->LockBits(&r, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppPARGB, &d) != Ok) return;
    if (d.Stride == r.Width * 4) BoxBlur((uint32_t*)d.Scan0, r.Width, r.Height, radius);
    b->UnlockBits(&d);
}

// Мягкая тень карточки bw x bh с запасом pad вокруг: размывается только прозрачность.
std::unique_ptr<Bitmap> Shadow(float bw, float bh, float pad, float radius, int blur, BYTE alpha) {
    int w = (int)std::ceil(bw + pad * 2), h = (int)std::ceil(bh + pad * 2);
    std::vector<int> a((size_t)w * h, 0), t((size_t)w * h);
    {
        Bitmap mask(w, h, PixelFormat32bppPARGB);
        {
            Graphics g(&mask);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.Clear(Color(0, 0, 0, 0));
            GraphicsPath p;
            RoundRect(p, RectF(pad, pad, bw, bh), radius);
            SolidBrush b(Color(255, 255, 255, 255));
            g.FillPath(&b, &p);
        }
        BitmapData d;
        Rect r(0, 0, w, h);
        mask.LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &d);
        for (int y = 0; y < h; ++y) {
            const uint32_t* row = (const uint32_t*)((BYTE*)d.Scan0 + (size_t)y * d.Stride);
            for (int x = 0; x < w; ++x) a[(size_t)y * w + x] = (int)(row[x] >> 24);
        }
        mask.UnlockBits(&d);
    }
    auto pass = [&](std::vector<int>& src, std::vector<int>& dst, bool horiz) {
        int len = horiz ? w : h, lines = horiz ? h : w, n = blur * 2 + 1;
        for (int l = 0; l < lines; ++l) {
            auto at = [&](int i) -> int& {
                i = std::clamp(i, 0, len - 1);
                return horiz ? src[(size_t)l * w + i] : src[(size_t)i * w + l];
            };
            int sum = 0;
            for (int i = -blur; i <= blur; ++i) sum += at(i);
            for (int i = 0; i < len; ++i) {
                (horiz ? dst[(size_t)l * w + i] : dst[(size_t)i * w + l]) = sum / n;
                sum += at(i + blur + 1) - at(i - blur);
            }
        }
    };
    for (int k = 0; k < 3; ++k) {
        pass(a, t, true);
        pass(t, a, false);
    }
    auto out = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    BitmapData d;
    Rect r(0, 0, w, h);
    out->LockBits(&r, ImageLockModeWrite, PixelFormat32bppPARGB, &d);
    for (int y = 0; y < h; ++y) {
        uint32_t* row = (uint32_t*)((BYTE*)d.Scan0 + (size_t)y * d.Stride);
        for (int x = 0; x < w; ++x) row[x] = (uint32_t)(a[(size_t)y * w + x] * alpha / 255) << 24;
    }
    out->UnlockBits(&d);
    return out;
}

// Яркий цвет обложки: средний по насыщенным пикселям, подтянутый до сочного.
bool ArtAccent(Bitmap* small, COLORREF& out) {
    BitmapData d;
    Rect r(0, 0, (INT)small->GetWidth(), (INT)small->GetHeight());
    if (small->LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &d) != Ok) return false;
    double sr = 0, sg = 0, sb = 0, sw = 0;
    for (int y = 0; y < r.Height; ++y) {
        const uint32_t* row = (const uint32_t*)((BYTE*)d.Scan0 + (size_t)y * d.Stride);
        for (int x = 0; x < r.Width; ++x) {
            double R = (row[x] >> 16 & 255) / 255.0, G = (row[x] >> 8 & 255) / 255.0, B = (row[x] & 255) / 255.0;
            double mx = std::max({R, G, B}), mn = std::min({R, G, B});
            double s = mx > 0 ? (mx - mn) / mx : 0;
            double w = s * s * mx * (mx > 0.18 ? 1 : 0) + 1e-4;
            sr += R * w;
            sg += G * w;
            sb += B * w;
            sw += w;
        }
    }
    small->UnlockBits(&d);
    double R = sr / sw, G = sg / sw, B = sb / sw;
    double mx = std::max({R, G, B}), mn = std::min({R, G, B});
    if (mx - mn < 0.06) return false;  // обложка почти серая: оставляем цвет темы
    // в HSV: насыщенность не меньше 0.55, яркость 0.92
    double h = 0, dlt = mx - mn;
    if (mx == R)
        h = std::fmod((G - B) / dlt, 6.0);
    else if (mx == G)
        h = (B - R) / dlt + 2;
    else
        h = (R - G) / dlt + 4;
    h = h < 0 ? h + 6 : h;
    double s = std::max(0.55, dlt / mx), v = 0.92;
    double c = v * s, x = c * (1 - std::fabs(std::fmod(h, 2.0) - 1)), m = v - c;
    double rr = 0, gg = 0, bb = 0;
    int seg = (int)h % 6;
    if (seg == 0) rr = c, gg = x;
    if (seg == 1) rr = x, gg = c;
    if (seg == 2) gg = c, bb = x;
    if (seg == 3) gg = x, bb = c;
    if (seg == 4) rr = x, bb = c;
    if (seg == 5) rr = c, bb = x;
    out = RGB((int)((rr + m) * 255), (int)((gg + m) * 255), (int)((bb + m) * 255));
    return true;
}

int64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

float Approach(float v, float target, float up, float down) {
    return v < target ? std::min(target, v + up) : std::max(target, v - down);
}

}  // namespace

void MediaWidget::Create(HINSTANCE inst) {
    inst_ = inst;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    note_ = LoadImageFile(ExeDir() + L"\\icons\\music.png", 160);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClass,
                           L"Сейчас играет", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
}

void MediaWidget::Apply(const Config& cfg) {
    theme_ = CurrentTheme(cfg);
    if (!art_) accent_ = theme_.ac;
    toastOn_ = cfg.mediaToast;
    skipFull_ = cfg.skipFullscreen;
    if (cfg.media && !enabled_) {
        enabled_ = true;
        watch_.Start(wnd_, WM_TRACK);
        watch_.SetFast(false);
    } else if (!cfg.media && enabled_) {
        enabled_ = false;
        watch_.Stop();
        track_ = media::Track();
        open_ = false;
        slide_ = 0;
        if (wnd_) ShowWindow(wnd_, SW_HIDE);
    }
    dirty_ = true;
}

void MediaWidget::Destroy() {
    watch_.Stop();
    if (wnd_ && IsWindow(wnd_)) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

SIZE MediaWidget::WindowSize() const {
    float k = (float)k_;
    if (mode_ == M_MINI) return {(LONG)std::ceil((kTW + kM * 2) * k), (LONG)std::ceil((kTH + kM * 2) * k)};
    return {(LONG)std::ceil((kW + kM * 2) * k), (LONG)std::ceil((kLyr + kH + kM * 2) * k)};
}

RectF MediaWidget::Body() const {
    float k = (float)k_, m = kM * k;
    if (mode_ == M_MINI) return RectF(m, m, kTW * k, kTH * k);
    float ex = (float)EaseOut(expand_), grow = kLyr * k * ex;
    return RectF(m, m + kLyr * k - grow, kW * k, kH * k + grow);
}

RectF MediaWidget::Rect(Hit h) const {
    float k = (float)k_, m = kM * k;
    if (mode_ == M_MINI) {
        if (h == H_COVER) return RectF(m + 12 * k, m + 12 * k, 54 * k, 54 * k);
        if (h == H_TEXT) return RectF(m + 78 * k, m + 12 * k, (kTW - 92) * k, 54 * k);
        if (h == H_BODY) return Body();
        return RectF();
    }
    float by = m + kLyr * k;  // верх карточки без текста песни
    float x0 = m + 166 * k, w = (kW - 166 - 22) * k;
    float cx = x0 + w / 2;
    switch (h) {
        case H_COVER:
            return RectF(m + 18 * k, by + 18 * k, 128 * k, 128 * k);
        case H_TEXT:
            return RectF(x0, by + 16 * k, w, 70 * k);
        case H_BAR:
            return RectF(x0, by + 92 * k, w, 4 * k);
        case H_PREV:
            return RectF(cx - 70 * k, by + 116 * k, 34 * k, 34 * k);
        case H_PLAY:
            return RectF(cx - 23 * k, by + 110 * k, 46 * k, 46 * k);
        case H_NEXT:
            return RectF(cx + 36 * k, by + 116 * k, 34 * k, 34 * k);
        case H_LYR:
            return RectF(x0 + w - 32 * k, by + 118 * k, 32 * k, 30 * k);
        case H_LYRICS: {
            RectF b = Body();
            return RectF(b.X + 24 * k, b.Y + 18 * k, b.Width - 48 * k, std::max(0.0f, by - b.Y - 18 * k));
        }
        case H_BODY:
            return Body();
        default:
            return RectF();
    }
}

MediaWidget::Hit MediaWidget::HitAt(POINT p, int* src) const {
    PointF pt((REAL)p.x, (REAL)p.y);
    if (mode_ == M_MINI) return Body().Contains(pt) ? H_BODY : H_NONE;
    for (int i = 0; i < (int)srcRects_.size(); ++i) {
        if (srcRects_[i].Contains(pt)) {
            if (src) *src = i;
            return H_SRC;
        }
    }
    for (Hit h : {H_PREV, H_PLAY, H_NEXT, H_LYR, H_COVER, H_TEXT}) {
        if (Rect(h).Contains(pt)) return h;
    }
    RectF bar = Rect(H_BAR);
    float k = (float)k_;
    bar.Inflate(4 * k, 8 * k);
    if (track_.end > 0 && bar.Contains(pt)) return H_BAR;
    if (expand_ > 0.3f && Rect(H_LYRICS).Contains(pt)) return H_LYRICS;
    return Body().Contains(pt) ? H_BODY : H_NONE;
}

void MediaWidget::SetScale(double k) {
    if (k == k_ && shadowFull_) return;
    k_ = k;
    float kk = (float)k;
    int blur = (int)(9 * k);
    shadowFull_ = Shadow(kW * kk, kH * kk, kPad * kk, kRadius * kk, blur, 150);
    shadowTall_ = Shadow(kW * kk, (kH + kLyr) * kk, kPad * kk, kRadius * kk, blur, 150);
    shadowMini_ = Shadow(kTW * kk, kTH * kk, kPad * kk, 18 * kk, blur, 140);
    SetArt();
}

void MediaWidget::SetArt() {
    artId_ = track_.artId;
    art_.reset();
    artSmall_.reset();
    artBg_.reset();
    accent_ = theme_.ac;
    if (track_.art.empty() || k_ <= 0) return;
    art_ = Fit(track_.art, (int)std::lround(128 * k_), (int)std::lround(128 * k_));
    artSmall_ = Fit(track_.art, (int)std::lround(54 * k_), (int)std::lround(54 * k_));
    if (auto small = Fit(track_.art, 24, 24)) {
        COLORREF c;
        if (ArtAccent(small.get(), c)) accent_ = c;
    }
    artBg_ = Fit(track_.art, 40, 40);  // крошечная копия: растянутая и размытая, она станет фоном
    if (artBg_) BlurOpaque(artBg_.get(), 3);
}

void MediaWidget::RequestLyrics() {
    if (track_.title.empty()) return;
    fetch_.Request(track_.artist, track_.title, (int)(track_.end / 10000000), wnd_, WM_LYRICS);
}

void MediaWidget::OnTrack() {
    track_ = watch_.Get();
    if (track_.artId != artId_) SetArt();
    if (track_.id != lastTrackId_) {
        bool first = lastTrackId_ == 0;
        lastTrackId_ = track_.id;
        lyrPos_ = 0;
        plainOff_ = 0;
        if (lyricsOn_) RequestLyrics();
        if (!first && track_.status == media::kPlaying) Toast();
    }
    dirty_ = true;
}

void MediaWidget::Toast() {
    if (!toastOn_ || track_.title.empty() || (open_ && mode_ == M_FULL) || (slide_ > 0 && mode_ == M_FULL)) return;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    if (skipFull_ && corners::FullscreenOn(hm, mi.rcMonitor)) return;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    mode_ = M_MINI;
    SetScale(dx / 96.0);
    mon_ = mi.rcMonitor;
    SIZE sz = WindowSize();
    pos_.x = (mon_.left + mon_.right) / 2 - sz.cx / 2;
    pos_.y = mon_.bottom - (LONG)((kLift + kTH + kM) * k_);
    toastUntil_ = GetTickCount64() + kToast;
    open_ = true;
    watch_.SetFast(true);
}

void MediaWidget::Open(POINT pt, const RECT& mon) {
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy);
    bool fromMini = mode_ == M_MINI && slide_ > 0;
    mode_ = M_FULL;
    SetScale(dx / 96.0);
    mon_ = mon;
    SIZE sz = WindowSize();
    LONG m = (LONG)(kM * k_);
    pos_.x = std::clamp(pt.x - sz.cx / 2, mon.left - m, std::max(mon.left - m, mon.right - sz.cx + m));
    pos_.y = mon.bottom - (LONG)((kLift + kLyr + kH + kM) * k_);
    if (fromMini) slide_ = 1;  // маленькая карточка уже на месте: большая просто сменяет её
    open_ = true;
    awaySince_ = 0;
    hot_ = H_NONE;
    if (lyricsOn_) RequestLyrics();
    watch_.SetFast(true);
    dirty_ = true;
}

void MediaWidget::Tick(const Config& cfg, bool blocked) {
    if (!enabled_ || !wnd_) return;
    ULONGLONG now = GetTickCount64();
    POINT pt;
    GetCursorPos(&pt);
    RectF body = Body();
    LONG bl = pos_.x + (LONG)body.X, br = pos_.x + (LONG)body.GetRight(), bt = pos_.y + (LONG)body.Y;

    if (open_ && mode_ == M_MINI) {
        // навели на маленькую карточку - раскрывается большая
        if (slide_ > 0.6f && pt.x >= bl && pt.x <= br && pt.y >= bt && pt.y <= mon_.bottom) {
            Open(pt, mon_);
        } else if (now >= toastUntil_) {
            open_ = false;
            watch_.SetFast(false);
        }
    } else if (open_) {
        // карточка держится, пока курсор над ней или под ней у края
        LONG pad = (LONG)(26 * k_);
        bool inside = pt.x >= bl - pad && pt.x <= br + pad && pt.y >= bt - pad && pt.y <= mon_.bottom + 2;
        if (inside || GetCapture() == wnd_) {
            awaySince_ = 0;
        } else if (!awaySince_) {
            awaySince_ = now;
        } else if (now - awaySince_ > kLinger) {
            open_ = false;
            watch_.SetFast(false);
        }
    } else if (slide_ <= 0) {
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        const RECT& r = mi.rcMonitor;
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
        LONG gap = (LONG)(kEdgeGap * dx / 96);
        bool edge = pt.y >= r.bottom - 1 && pt.x > r.left + gap && pt.x < r.right - 1 - gap &&
                    !MonitorFromPoint({pt.x, r.bottom}, MONITOR_DEFAULTTONULL);
        bool buttons = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000;
        if (!edge || blocked || buttons || (cfg.skipFullscreen && corners::FullscreenOn(mon, r))) {
            edgeSince_ = 0;
        } else if (!edgeSince_) {
            edgeSince_ = now;
            watch_.SetFast(true);  // освежить трек, пока курсор ждёт
        } else if (now - edgeSince_ >= kDwell && track_.has && !track_.title.empty()) {
            edgeSince_ = 0;
            Open(pt, r);
        }
    }

    bool anim = false;
    auto step = [&](float& v, float target, float up, float down) {
        if (v == target) return;
        v = Approach(v, target, fx::Step(up), fx::Step(down));
        anim = true;
    };
    step(slide_, open_ ? 1.0f : 0.0f, 0.09f, 0.12f);
    step(expand_, lyricsOn_ && mode_ == M_FULL ? 1.0f : 0.0f, 0.08f, 0.1f);
    step(volShow_, now < volUntil_ ? 1.0f : 0.0f, 0.25f, 0.08f);
    for (int i = 0; i < kHits; ++i) step(hover_[i], i == hot_ ? 1.0f : 0.0f, 0.2f, 0.14f);
    if (expand_ > 0) {
        lyrics::Lyrics ly = fetch_.Get(track_.artist, track_.title);
        if (ly.state == lyrics::kSynced) {
            float target = (float)std::max(0, ly.LineAt(track_.Now()));
            if (std::fabs(lyrPos_ - target) > 0.002f) {
                lyrPos_ += (target - lyrPos_) * 0.18f;
                if (std::fabs(lyrPos_ - target) < 0.01f) lyrPos_ = target;
                anim = true;
            }
        }
    }
    int sec = (int)(track_.Now() / 10000000);
    if (sec != lastSecond_) {
        lastSecond_ = sec;
        dirty_ = true;
    }
    if (slide_ <= 0) {
        if (IsWindowVisible(wnd_)) {
            ShowWindow(wnd_, SW_HIDE);
            mem::TrimSoon();
        }
        return;
    }
    if (anim || dirty_) Render();
}

void MediaWidget::Wheel(int delta, POINT client) {
    float notches = delta / (float)WHEEL_DELTA;
    lyrics::Lyrics ly = fetch_.Get(track_.artist, track_.title);
    if (mode_ == M_FULL && HitAt(client) == H_LYRICS && ly.state == lyrics::kPlain) {
        plainOff_ = std::clamp(plainOff_ - notches * 3, 0.0f, (float)std::max(0, (int)ly.lines.size() - 4));
        dirty_ = true;
        return;
    }
    float v = vol::AddApp(track_.aumid, 0.04f * notches);
    volApp_ = v >= 0;
    if (v < 0) v = vol::AddMaster(0.04f * notches);  // плеер не нашёлся среди звуковых сеансов
    vol_ = v;
    volUntil_ = GetTickCount64() + kVolShow;
    dirty_ = true;
}
void MediaWidget::ContextMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (lyricsOn_ ? MF_CHECKED : 0), 2, L"Текст песни");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1, L"Выключить панель");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(wnd_);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, p.x, p.y, 0, wnd_, nullptr);
    PostMessageW(wnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == 2) {
        lyricsOn_ = !lyricsOn_;
        if (lyricsOn_) RequestLyrics();
    }
    if (cmd == 1 && onTurnOff) onTurnOff();
}

LRESULT CALLBACK MediaWidget::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* self = (MediaWidget*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self && self->wnd_ == h) return self->Handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT MediaWidget::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TRACK:
            OnTrack();
            return 0;
        case WM_LYRICS:
            dirty_ = true;
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR: {
            bool hand = hot_ != H_BODY && hot_ != H_NONE &&
                        !(hot_ == H_LYRICS && fetch_.Get(track_.artist, track_.title).state != lyrics::kSynced);
            SetCursor(LoadCursorW(nullptr, hand || mode_ == M_MINI ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, wnd_, 0};
                TrackMouseEvent(&t);
                tracking_ = true;
            }
            int s = -1;
            hot_ = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)}, &s);
            hotSrc_ = s;
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            hot_ = H_NONE;
            hotSrc_ = -1;
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(wnd_, &p);
            if (mode_ == M_FULL) Wheel((short)HIWORD(wp), p);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int s = -1;
            down_ = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)}, &s);
            downSrc_ = s;
            SetCapture(wnd_);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            ReleaseCapture();
            int s = -1;
            Hit h = HitAt(p, &s);
            if (h != down_ || (h == H_SRC && s != downSrc_)) return 0;
            if (mode_ == M_MINI) {
                POINT sp;
                GetCursorPos(&sp);
                Open(sp, mon_);
                return 0;
            }
            if (h == H_PLAY) {
                watch_.Send(media::CMD_TOGGLE);
                // сразу показать новое состояние, не дожидаясь опроса
                int64_t cur = track_.Now();
                track_.status = track_.status == media::kPlaying ? media::kPaused : media::kPlaying;
                track_.position = cur;
                track_.updated = NowFileTime();
                dirty_ = true;
            }
            if (h == H_NEXT && CanNext()) watch_.Send(media::CMD_NEXT);
            if (h == H_PREV && CanPrev()) {
                watch_.Send(media::CMD_PREV);
                if (track_.Now() > 30000000 && track_.canSeek && track_.end > 0) {
                    track_.position = 0;  // в начало трека: показать сразу
                    track_.updated = NowFileTime();
                    dirty_ = true;
                }
            }
            if (h == H_COVER || h == H_TEXT) media::FocusSource(track_.aumid);
            if (h == H_SRC && s >= 0 && s < (int)track_.sources.size()) {
                watch_.SetSource(track_.sources[s].aumid);
                dirty_ = true;
            }
            if (h == H_LYR) {
                lyricsOn_ = !lyricsOn_;
                if (lyricsOn_) RequestLyrics();
            }
            auto seek = [&](int64_t to) {
                watch_.Send(media::CMD_SEEK, to);
                track_.position = to;
                track_.updated = NowFileTime();
                dirty_ = true;
            };
            if (h == H_BAR && track_.end > 0) {
                RectF bar = Rect(H_BAR);
                seek((int64_t)(std::clamp((p.x - bar.X) / bar.Width, 0.0f, 1.0f) * track_.end));
            }
            if (h == H_LYRICS) {
                // клик по строке синхронизированного текста - перемотать к ней
                lyrics::Lyrics ly = fetch_.Get(track_.artist, track_.title);
                RectF area = Rect(H_LYRICS);
                int i = (int)std::lround(lyrPos_ + (p.y - (area.Y + area.Height * 0.5f)) / (30 * k_));
                if (ly.state == lyrics::kSynced && i >= 0 && i < (int)ly.lines.size()) seek(ly.lines[i].at);
            }
            return 0;
        }
        case WM_RBUTTONUP:
            ContextMenu();
            return 0;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}
