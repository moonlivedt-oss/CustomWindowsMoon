#include "core/usage.h"
#include "media/widget.h"

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
#include "desktop/corners.h"
#include "media/playermoon.h"
#include "media/volume.h"
#include "media/widget_internal.h"
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

// пружина мелкими шагами: кадры приходят неровно
void Spring(float& x, float& v, float target, float k, float c, double dt) {
    int n = std::max(1, (int)std::ceil(dt / 0.004));
    float h = (float)(dt / n);
    for (int i = 0; i < n; ++i) {
        v += (k * (target - x) - c * v) * h;
        x += v * h;
    }
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

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
    if (!hasArt_) {
        accent_ = accentFrom_ = theme_.ac;
        if (k_ > 0) SetArt();  // яблоко без обложки красится цветом темы
    }
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
        slide_ = slideV_ = 0;
        swap_ = 0;
        haveNext_ = false;
        if (wnd_) ShowWindow(wnd_, SW_HIDE);
        FreeFrame();
    }
    dirty_ = true;
}

void MediaWidget::Destroy() {
    watch_.Stop();
    FreeFrame();
    if (wnd_ && IsWindow(wnd_)) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

// Окно одного размера для обеих карточек: маленькая вырастает в большую на месте, с запасом слева под пластинку.
SIZE MediaWidget::WindowSize() const {
    float k = (float)k_;
    return {(LONG)std::ceil((kRollL + kW + kM * 2) * k), (LONG)std::ceil((kLyr + kH + kM * 2) * k)};
}

RectF MediaWidget::Body() const {
    float k = (float)k_, mo = std::clamp(morph_, -0.04f, 1.06f);
    float cx = (kM + kRollL + kW / 2) * k, bottom = (kM + kLyr + kH) * k;
    float w = Lerp(kTW, kW, mo) * k, h = (Lerp(kTH, kH, mo) + kLyr * (float)EaseOut(expand_)) * k;
    return RectF(cx - w / 2, bottom - h, w, h);
}

RectF MediaWidget::RecordRect() const {
    float k = (float)k_, mo = std::clamp(morph_, 0.0f, 1.04f);
    float bottom = (kM + kLyr + kH) * k, cx = (kM + kRollL + kW / 2) * k;
    float fx = cx - kW * k / 2 + 18 * k, mx = cx - kTW * k / 2 + 12 * k;
    float d = Lerp(kRecMini, kRec, mo) * k;
    return RectF(Lerp(mx, fx, mo), bottom - Lerp(12, 18, mo) * k - d, d, d);
}

RectF MediaWidget::Rect(Hit h) const {
    float k = (float)k_, m = kM * k, mx = (kM + kRollL) * k;
    if (mode_ == M_MINI) {
        float x0 = mx + (kW - kTW) / 2 * k, y0 = m + (kLyr + kH - kTH) * k;
        if (h == H_COVER) return RecordRect();
        if (h == H_TEXT) return RectF(x0 + 78 * k, y0 + 10 * k, (kTW - 92) * k, 58 * k);
        if (h == H_BODY) return Body();
        return RectF();
    }
    float by = m + kLyr * k;  // верх карточки без текста песни
    float x0 = mx + 166 * k, w = (kW - 166 - 22) * k;
    float cx = x0 + w / 2;
    switch (h) {
        case H_COVER:
            return RectF(mx + 18 * k, by + 18 * k, kRec * k, kRec * k);
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
    FreeFrame();
    BuildDisc();
    SetArt();
}

// Пластинка рисуется один раз: бороздки круглые, а блик от лампы стоит на месте - вращается только яблоко.
void MediaWidget::BuildDisc() {
    float k = (float)k_, pad = 12 * k, d = kRec * k, r = d / 2, c = pad + r;
    int size = (int)std::ceil(d + pad * 2);
    disc_ = std::make_unique<Bitmap>(size, size, PixelFormat32bppPARGB);
    Graphics g(disc_.get());
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    for (int i = 6; i >= 1; --i) {  // тень
        SolidBrush sb(Color((BYTE)(14 + (6 - i) * 7), 0, 0, 0));
        float e = i * 1.4f * k;
        g.FillEllipse(&sb, c - r - e, c - r - e + 6 * k, d + 2 * e, d + 2 * e);
    }
    SolidBrush base(Color(255, 21, 17, 26));
    g.FillEllipse(&base, c - r, c - r, d, d);
    Pen groove(Color(255, 38, 32, 44), std::max(0.8f, 1.1f * k));
    for (float gr = r - 2 * k; gr > r * 0.5f; gr -= 3 * k) g.DrawEllipse(&groove, c - gr, c - gr, gr * 2, gr * 2);
    {
        GraphicsPath clip;
        clip.AddEllipse(c - r, c - r, d, d);
        g.SetClip(&clip);
        for (float mid : {-50.f, 130.f}) {  // две полосы блика напротив друг друга
            for (float a = -26; a < 26; a += 2) {
                float f = 1 - (a / 26) * (a / 26);
                SolidBrush b(Color((BYTE)(44 * f), 255, 255, 255));
                g.FillPie(&b, c - r, c - r, d, d, mid + a, 2.2f);
            }
        }
        g.ResetClip();
    }
    Pen rim(Color(60, 255, 255, 255), 1 * k);
    g.DrawEllipse(&rim, c - r + 0.5f, c - r + 0.5f, d - 1, d - 1);
}

void MediaWidget::SetArt() {
    artId_ = track_.artId;
    artBg_.reset();
    accentFrom_ = acMix_ >= 1 ? accent_ : accentFrom_;
    acMix_ = 0;
    accent_ = theme_.ac;
    hasArt_ = false;
    layerDirty_ = true;
    if (k_ <= 0) return;
    int ld = (int)std::lround(kRec * 0.46 * k_);
    std::unique_ptr<Bitmap> face;
    if (!track_.art.empty()) {
        face = Fit(track_.art, ld, ld);
        if (auto small = Fit(track_.art, 24, 24)) {
            COLORREF c;
            if (ArtAccent(small.get(), c)) accent_ = c;
        }
        artBg_ = Fit(track_.art, 40, 40);  // крошечная копия: растянутая и размытая, она станет фоном
        if (artBg_) BlurOpaque(artBg_.get(), 3);
        hasArt_ = face != nullptr;
    }
    label_ = std::make_unique<Bitmap>(ld, ld, PixelFormat32bppPARGB);
    Graphics g(label_.get());
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    GraphicsPath p;
    p.AddEllipse(0, 0, ld, ld);
    if (face) {
        TextureBrush tb(face.get());
        g.FillPath(&tb, &p);
    } else {
        LinearGradientBrush lb(PointF(0, 0), PointF((REAL)ld, (REAL)ld), ToColor(accent_), ToColor(theme_.glow));
        g.FillPath(&lb, &p);
        if (note_) g.DrawImage(note_.get(), RectF(ld * 0.24f, ld * 0.24f, ld * 0.52f, ld * 0.52f));
    }
}

void MediaWidget::RequestLyrics() {
    if (track_.title.empty()) return;
    fetch_.Request(track_.artist, track_.title, (int)(track_.end / 10000000), wnd_, WM_LYRICS);
}

void MediaWidget::OnTrack() { FeedTrack(watch_.Get()); }

void MediaWidget::FeedTrack(media::Track t) {
    if (haveNext_) {  // старая пластинка ещё катится: новое подождёт её
        next_ = std::move(t);
        return;
    }
    if (t.id != lastTrackId_ && lastTrackId_ != 0 && open_ && mode_ == M_FULL && slide_ > 0.6f) {
        next_ = std::move(t);
        haveNext_ = true;
        if (!swap_) BeginSwap();
        return;
    }
    ApplyTrack(t);
}

void MediaWidget::ApplyTrack(const media::Track& t) {
    track_ = t;
    if (track_.artId != artId_) SetArt();
    if (track_.id != lastTrackId_) {
        bool first = lastTrackId_ == 0;
        lastTrackId_ = track_.id;
        lyrPos_ = 0;
        plainOff_ = 0;
        titleOff_ = 0;
        titleW_ = -1;
        if (lyricsOn_) RequestLyrics();
        if (!first && track_.status == media::kPlaying) Toast();
    }
    dirty_ = true;
}

// Пластинка укатывается; новый трек покажем, когда она скроется (или вернём её, если трек так и не сменился).
void MediaWidget::BeginSwap() {
    swap_ = 1;
    swapSince_ = GetTickCount64();
    rxT_ = -kRoll;
}

void MediaWidget::Close() {
    open_ = false;
    closingSince_ = GetTickCount64();
    rxT_ = -kRoll;
    watch_.SetFast(false);
}

float MediaWidget::Rev(float delay, float dur) const {
    if (fx::Game() || !revealAt_) return 1;
    ULONGLONG now = GetTickCount64();
    if (now < revealAt_) return 0;
    return (float)Clamp01(((now - revealAt_) / fx::Dur(1000) - delay) / dur);
}

Color MediaWidget::Ac() const {
    return acMix_ >= 1 ? ToColor(accent_) : MixColor(ToColor(accentFrom_), ToColor(accent_), EaseOut(acMix_));
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
    usage::Hit(L"media_toast");
    ULONGLONG nowMs = GetTickCount64();
    bool fresh = slide_ <= 0;
    mode_ = M_MINI;
    SetScale(dx / 96.0);
    mon_ = mi.rcMonitor;
    pos_.x = (mon_.left + mon_.right) / 2 - (LONG)((kM + kRollL + kW / 2) * k_);
    pos_.y = mon_.bottom - (LONG)((kLift + kLyr + kH + kM) * k_);
    if (fresh) {
        morph_ = morphV_ = 0;
        slide_ = slideV_ = 0;
        rx_ = -kRoll;
        rxV_ = 0;
        spin_ = 0;
        needleAt_ = nowMs + (ULONGLONG)fx::Dur(300);
        revealAt_ = nowMs + (ULONGLONG)fx::Dur(90);
    }
    rxT_ = 0;
    layerDirty_ = true;
    toastUntil_ = nowMs + kToast;
    toastPt_ = pt;
    miniOverSince_ = 0;
    open_ = true;
    watch_.SetFast(true);
}

void MediaWidget::Open(POINT pt, const RECT& mon) {
    usage::Hit(L"media");
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy);
    ULONGLONG now = GetTickCount64();
    bool fromMini = mode_ == M_MINI && slide_ > 0, fresh = slide_ <= 0;
    mode_ = M_FULL;
    SetScale(dx / 96.0);
    mon_ = mon;
    if (!fromMini) {  // из маленькой карточки большая растёт на её же месте, а не прыгает к курсору
        LONG mx = (LONG)((kM + kRollL) * k_), bw = (LONG)(kW * k_);
        pos_.x = std::clamp(pt.x - mx - bw / 2, mon.left - mx, std::max(mon.left - mx, mon.right - mx - bw));
        pos_.y = mon.bottom - (LONG)((kLift + kLyr + kH + kM) * k_);
    }
    if (fresh) {
        morph_ = 1;
        morphV_ = 0;
        slide_ = slideV_ = 0;
        rx_ = -kRoll;
        rxV_ = 0;
        spin_ = 0;
        needleAt_ = now + (ULONGLONG)fx::Dur(560);
        revealAt_ = now + (ULONGLONG)fx::Dur(110);
    } else if (fromMini) {
        spin_ += 240;  // рост подталкивает пластинку
        revealAt_ = now + (ULONGLONG)fx::Dur(120);
    } else {
        needleAt_ = now + (ULONGLONG)fx::Dur(200);
    }
    rxT_ = 0;
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
    double dtRaw = std::min<double>(50, (double)(now - lastTick_));
    lastTick_ = now;
    double dt = dtRaw / fx::Dur(1000);  // секунды с поправкой на скорость движения
    POINT pt;
    GetCursorPos(&pt);
    RectF body = Body();
    LONG bl = pos_.x + (LONG)body.X, br = pos_.x + (LONG)body.GetRight(), bt = pos_.y + (LONG)body.Y;

    if (open_ && mode_ == M_MINI) {
        // задержались на маленькой карточке - раскрывается большая. Мимоходом не считается, и курсор, под которым
        // она сама выехала, тоже: он должен сдвинуться
        bool over = slide_ > 0.6f && pt.x >= bl && pt.x <= br && pt.y >= bt && pt.y <= mon_.bottom;
        if (over && (pt.x != toastPt_.x || pt.y != toastPt_.y)) {
            if (!miniOverSince_) miniOverSince_ = now;
            if (now - miniOverSince_ >= kMiniDwell) Open(pt, mon_);
        } else {
            miniOverSince_ = 0;
            if (now >= toastUntil_) Close();
        }
    } else if (open_) {
        // карточка держится, пока курсор над ней или под ней у края
        LONG pad = (LONG)(26 * k_);
        bool inside = pt.x >= bl - pad && pt.x <= br + pad && pt.y >= bt - pad && pt.y <= mon_.bottom + 2;
        if (inside || demoHold_ || GetCapture() == wnd_) {
            awaySince_ = 0;
        } else if (!awaySince_) {
            awaySince_ = now;
        } else if (now - awaySince_ > kLinger) {
            Close();
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
        if (!edge) edgeHold_ = false;
        if (!edge || edgeHold_ || blocked || buttons || (cfg.skipFullscreen && corners::FullscreenOn(mon, r))) {
            edgeSince_ = 0;
        } else if (!edgeSince_) {
            edgeSince_ = now;
            watch_.SetFast(true);  // освежить трек, пока курсор ждёт
        } else if (now - edgeSince_ >= kDwell && track_.has && !track_.title.empty()) {
            edgeSince_ = 0;
            Open(pt, r);
        } else if (now - edgeSince_ >= kLaunchDwell && !(track_.has && !track_.title.empty()) &&
                   playermoon::Installed()) {
            // Карточке нечего показать: плеер закрыт (или в нём нет трека) - запускаем его или выводим вперёд
            edgeSince_ = 0;
            edgeHold_ = true;
            playermoon::Open(wnd_);
        }
    }

    // другой трек: пока старая пластинка катится прочь, карточка показывает её
    if (swap_) {
        bool away = rx_ < -kRoll * 0.7f || now - swapSince_ > (ULONGLONG)fx::Dur(320);
        if (haveNext_ && (away || !open_)) {
            haveNext_ = false;
            swap_ = 0;
            ApplyTrack(next_);
            rx_ = -kRoll;
            rxV_ = 0;
            rxT_ = open_ ? 0 : -kRoll;
            needleAt_ = now + (ULONGLONG)fx::Dur(420);
        } else if (!haveNext_ && now - swapSince_ > 1200) {  // плеер так и не сменил трек
            swap_ = 0;
            rxT_ = open_ ? 0 : -kRoll;
            needleAt_ = now + (ULONGLONG)fx::Dur(300);
        }
    }

    bool playing = track_.status == media::kPlaying;
    bool needle = open_ && playing && !swap_ && now >= needleAt_;
    float f = track_.end > 0 ? (float)std::clamp((double)track_.Now() / track_.end, 0.0, 1.0) : 0;
    float armT = needle ? -14 + 20 * f : -22;

    bool frame = false, anim = false;  // frame - движется верхний слой, anim - перерисовать и неподвижный
    float rxBefore = rx_, armBefore = arm_, slideBefore = slide_;
    bool closing = !open_ && now - closingSince_ >= (ULONGLONG)fx::Dur(130);
    if (fx::Game()) {  // в игре - сразу на месте
        slide_ = open_ ? 1.0f : 0.0f;
        slideV_ = 0;
        morph_ = mode_ == M_FULL ? 1.0f : 0.0f;
        morphV_ = 0;
        rx_ = rxT_;
        rxV_ = 0;
        arm_ = armT;
        armV_ = 0;
        spin_ = 0;
    } else {
        Spring(slide_, slideV_, open_ || !closing ? 1.0f : 0.0f, 170, open_ ? 20.0f : 24.0f, dt);
        float morphBefore = morph_;
        Spring(morph_, morphV_, mode_ == M_FULL ? 1.0f : 0.0f, 150, 16, dt);
        if (std::fabs(morph_ - (mode_ == M_FULL ? 1.0f : 0.0f)) < 0.001f && std::fabs(morphV_) < 0.01f)
            morph_ = mode_ == M_FULL ? 1.0f : 0.0f, morphV_ = 0;
        anim |= morph_ != morphBefore;
        Spring(rx_, rxV_, rxT_, 95, 13.5f, dt);
        if (std::fabs(rx_ - rxT_) < 0.15f && std::fabs(rxV_) < 1) rx_ = rxT_, rxV_ = 0;
        Spring(arm_, armV_, armT, 90, 14, dt);
        if (std::fabs(arm_ - armT) < 0.02f && std::fabs(armV_) < 0.05f) arm_ = armT, armV_ = 0;
        float target = needle ? 200.0f : 0.0f;
        spin_ += (target - spin_) * (float)(1 - std::exp(-dt / (needle ? 0.28 : 0.55)));
        if (std::fabs(spin_ - target) < 0.3f) spin_ = target;
        // катится по-настоящему: поворот равен пройденному пути
        if (k_ > 0) {  // до первого показа масштаба ещё нет
            float recR = RecordRect().Width / 2 / (float)k_;
            ang_ = (float)std::fmod(ang_ + spin_ * dt + (rx_ - rxBefore) / recR * 57.3f, 360.0);
        }
    }
    if (!open_ && closing && slide_ <= 0.01f)
        slide_ = slideV_ = 0;  // хвост пружины: иначе маленькой карточке негде выглянуть
    frame |= rx_ != rxBefore || arm_ != armBefore || slide_ != slideBefore || spin_ != 0;

    auto step = [&](float& v, float target, float up, float down) {
        if (v == target) return;
        v = Approach(v, target, fx::Step(up), fx::Step(down));
        anim = true;
    };
    step(expand_, lyricsOn_ && mode_ == M_FULL ? 1.0f : 0.0f, 0.08f, 0.1f);
    step(volShow_, now < volUntil_ ? 1.0f : 0.0f, 0.25f, 0.08f);
    step(glow_, playing ? 1.0f : 0.4f, 0.05f, 0.04f);
    step(acMix_, 1, 0.035f, 0.035f);
    step(titleA_, swap_ ? 0.0f : 1.0f, 0.08f, 0.12f);
    for (int i = 0; i < kHits; ++i) step(hover_[i], i == hot_ ? 1.0f : 0.0f, 0.2f, 0.14f);
    {
        float before = eq_;
        eq_ = Approach(eq_, needle ? 1.0f : 0.0f, fx::Step(0.1f), fx::Step(0.08f));
        frame |= eq_ != before || eq_ > 0;
    }
    if (revealAt_ && now < revealAt_ + (ULONGLONG)fx::Dur(1000)) anim = true;  // строки появляются одна за другой
    // длинное название прокручивается, пока на него смотрят
    float over = titleW_ - Rect(H_TEXT).Width + 12 * (float)k_;
    if (hot_ == H_TEXT && over > 0 && !swap_) {
        if (!titleOverSince_) titleOverSince_ = now;
        if (now - titleOverSince_ > 250 && titleOff_ < over) {
            titleOff_ = std::min(over, titleOff_ + 45 * (float)(k_ * dt));
            anim = true;
        }
    } else {
        titleOverSince_ = 0;
        if (titleOff_ > 0) {
            titleOff_ = titleOff_ < 1 ? 0 : titleOff_ * (float)std::exp(-dt / 0.1);
            anim = true;
        }
    }
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
    if (slide_ > 0 || edgeSince_) fx::Active();  // панель на экране или курсор ждёт у нижнего края
    if (slide_ <= 0) {
        if (IsWindowVisible(wnd_)) {
            ShowWindow(wnd_, SW_HIDE);
            FreeFrame();
            mem::TrimSoon();
        }
        return;
    }
    if (dirty_ || anim) layerDirty_ = true;
    dirty_ = false;
    if (discBaked_ != (rx_ == rxT_ && rx_ == 0)) layerDirty_ = true;  // пластинка встала или покатилась
    if (layerDirty_ || frame || !IsWindowVisible(wnd_)) Render();
}

bool MediaWidget::SnapFrame(const Config& cfg, const media::Track& t, const SnapState& st, double k,
                            const std::wstring& out) {
    const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    theme_ = CurrentTheme(cfg);
    track_ = t;
    mode_ = st.morph < 0.5f ? M_MINI : M_FULL;
    morph_ = st.morph;
    rx_ = rxT_ = st.rx;
    arm_ = st.arm;
    ang_ = st.ang;
    expand_ = st.expand;
    slide_ = 1;
    eq_ = t.status == media::kPlaying ? 1.0f : 0.0f;
    glow_ = t.status == media::kPlaying ? 1.0f : 0.4f;
    if (!note_) note_ = LoadImageFile(ExeDir() + L"\\icons\\music.png", 160);
    SetScale(k);
    acMix_ = 1;
    SIZE sz = WindowSize();
    Bitmap frame(sz.cx, sz.cy, PixelFormat32bppARGB);
    Graphics g(&frame);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(255, 46, 42, 50));
    DrawStatic(g);
    DrawDynamic(g);
    return frame.Save(out.c_str(), &png, nullptr) == Ok;
}

void MediaWidget::DemoRun(HINSTANCE inst, const Config& cfg, const media::Track& a, const media::Track& b,
                          const std::wstring& dir) {
    const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    Create(inst);
    theme_ = CurrentTheme(cfg);
    accent_ = accentFrom_ = theme_.ac;
    enabled_ = demoHold_ = true;
    toastOn_ = true;
    skipFull_ = false;
    ApplyTrack(a);
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    POINT mid{(mi.rcMonitor.left + mi.rcMonitor.right) / 2, mi.rcMonitor.bottom - 1};
    Open(mid, mi.rcMonitor);
    ULONGLONG t0 = GetTickCount64();
    std::wstring log;
    struct Mark {
        int ms;
        const wchar_t* name;
    };
    const Mark marks[] = {{250, L"open-rolling"},  {750, L"open-needle"}, {1500, L"playing"},
                          {2200, L"paused"},       {3000, L"resumed"},    {3350, L"next-rollout"},
                          {3800, L"next-rollin"},  {4600, L"next-done"},  {5100, L"scratch"},
                          {5500, L"closing"},      {6000, L"closed"},     {6500, L"mini"},
                          {7400, L"mini-growing"}, {8200, L"grown"}};
    size_t nextMark = 0;
    int stage = 0;
    for (;;) {
        ULONGLONG ms = GetTickCount64() - t0;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        if (stage == 0 && ms >= 1600) {  // пауза
            track_.status = media::kPaused;
            stage = 1;
        } else if (stage == 1 && ms >= 2500) {  // снова играет
            track_.status = media::kPlaying;
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            track_.updated = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            stage = 2;
        } else if (stage == 2 && ms >= 3200) {  // нажали «дальше»: пластинка укатывается сразу
            BeginSwap();
            stage = 3;
        } else if (stage == 3 && ms >= 3500) {  // плеер сообщил новый трек
            FeedTrack(b);
            stage = 4;
        } else if (stage == 4 && ms >= 5000) {  // перемотка
            spin_ = 1300;
            stage = 5;
        } else if (stage == 5 && ms >= 5400) {  // курсор ушёл
            Close();
            stage = 6;
        } else if (stage == 6 && ms >= 6300) {  // сменился трек, карточка спрятана: выглядывает маленькая
            lastTrackId_ = 0;                   // следующий ApplyTrack не откроет лишнего
            ApplyTrack(a);
            Toast();
            stage = 7;
        } else if (stage == 7 && ms >= 7200) {  // задержались на маленькой
            Open(mid, mi.rcMonitor);
            stage = 8;
        }
        Tick(cfg, false);
        if (nextMark < sizeof(marks) / sizeof(marks[0]) && ms >= (ULONGLONG)marks[nextMark].ms) {
            wchar_t line[256];
            swprintf(line, 256,
                     L"%5d %-14ls slide=%.2f morph=%.2f rx=%.0f rxT=%.0f arm=%.1f spin=%.0f eq=%.2f swap=%d visible=%d "
                     L"open=%d title=%ls\r\n",
                     (int)ms, marks[nextMark].name, slide_, morph_, rx_, rxT_, arm_, spin_, eq_, swap_,
                     IsWindowVisible(wnd_) ? 1 : 0, open_ ? 1 : 0, track_.title.c_str());
            log += line;
            if (bits_) {
                Bitmap frame(dibW_, dibH_, PixelFormat32bppARGB);
                Graphics g(&frame);
                g.Clear(Color(255, 46, 42, 50));
                Bitmap src(dibW_, dibH_, dibW_ * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
                g.DrawImage(&src, 0, 0);
                frame.Save((dir + L"\\sim-" + marks[nextMark].name + L".png").c_str(), &png, nullptr);
            }
            ++nextMark;
        }
        if (nextMark >= sizeof(marks) / sizeof(marks[0])) break;
        Sleep(16);
    }
    if (wnd_) ShowWindow(wnd_, SW_HIDE);
    HANDLE f = CreateFileW((dir + L"\\sim.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::string u(WideCharToMultiByte(CP_UTF8, 0, log.c_str(), (int)log.size(), nullptr, 0, nullptr, nullptr), 0);
        WideCharToMultiByte(CP_UTF8, 0, log.c_str(), (int)log.size(), u.data(), (int)u.size(), nullptr, nullptr);
        DWORD put = 0;
        WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
        CloseHandle(f);
    }
}

void MediaWidget::Bench(const Config& cfg, const media::Track& t, double k, double& spinMs, double& fullMs) {
    theme_ = CurrentTheme(cfg);
    track_ = t;
    mode_ = M_FULL;
    morph_ = 1;
    rx_ = rxT_ = 0;
    arm_ = -8;
    slide_ = 1;
    eq_ = 1;
    SetScale(k);
    acMix_ = 1;
    SIZE sz = WindowSize();
    std::vector<uint32_t> layer((size_t)sz.cx * sz.cy), frameBits((size_t)sz.cx * sz.cy);
    LARGE_INTEGER fq, a, b;
    QueryPerformanceFrequency(&fq);
    auto settle = [](Graphics& g) {
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    };
    const int n = 200;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < n; ++i) {
        std::fill(layer.begin(), layer.end(), 0);
        Bitmap bmp(sz.cx, sz.cy, sz.cx * 4, PixelFormat32bppPARGB, (BYTE*)layer.data());
        Graphics g(&bmp);
        settle(g);
        DrawStatic(g);
    }
    QueryPerformanceCounter(&b);
    fullMs = (b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart / n;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < n; ++i) {
        memcpy(frameBits.data(), layer.data(), layer.size() * 4);
        ang_ += 4;
        Bitmap bmp(sz.cx, sz.cy, sz.cx * 4, PixelFormat32bppPARGB, (BYTE*)frameBits.data());
        Graphics g(&bmp);
        settle(g);
        DrawDynamic(g);
    }
    QueryPerformanceCounter(&b);
    spinMs = (b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart / n;
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
    AppendMenuW(m, MF_STRING, 3, L"Открыть PlayerMoon");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (lyricsOn_ ? MF_CHECKED : 0), 2, L"Текст песни");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1, L"Выключить панель");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(wnd_);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, p.x, p.y, 0, wnd_, nullptr);
    PostMessageW(wnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == 3) playermoon::Open(wnd_);
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
            if (h == H_NEXT && CanNext()) {
                watch_.Send(media::CMD_NEXT);
                if (!swap_) BeginSwap();  // пластинка укатывается сразу, не дожидаясь плеера
            }
            if (h == H_PREV && CanPrev()) {
                watch_.Send(media::CMD_PREV);
                if (track_.Now() > 30000000 && track_.canSeek && track_.end > 0) {
                    track_.position = 0;  // в начало трека: показать сразу
                    track_.updated = NowFileTime();
                    spin_ = -1300;  // царапает назад
                    dirty_ = true;
                } else if (!swap_) {
                    BeginSwap();
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
                spin_ = to > track_.Now() ? 1300.0f : -1300.0f;  // перемотка царапает пластинку
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
