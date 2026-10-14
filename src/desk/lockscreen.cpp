#include "desk/lockscreen.h"

#include <inspectable.h>
#include <roapi.h>
#include <shlwapi.h>
#include <winstring.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/config.h"
#include "core/files.h"
#include "core/themes.h"
#include "desk/lock_internal.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

// Windows.System.UserProfile.LockScreen описан вручную, как в ocr.cpp: заголовков WinRT в MinGW нет.
namespace lock_abi {

struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

struct ILockScreenStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_OriginalImageFile(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetImageStream(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetImageFileAsync(IInspectable* file, IInspectable** action) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetImageStreamAsync(IInspectable* stream, IInspectable** action) = 0;
};

}  // namespace lock_abi

using namespace lock_abi;

namespace lockscreen {

using namespace detail;

namespace {

const IID IID_AsyncInfo = {0x00000036, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const IID IID_LockScreenStatics = {0x3EE9D3AD, 0xB607, 0x40AE, {0xB4, 0x26, 0x76, 0x31, 0xD9, 0x82, 0x12, 0x69}};
const IID IID_RandomAccessStream = {0x905A0FE1, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};

template <class T>
struct Ptr {
    T* p = nullptr;
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    ~Ptr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// Функции WinRT берутся из combase.dll и shcore.dll, как в ocr.cpp.
struct Api {
    HRESULT(WINAPI* roInit)(RO_INIT_TYPE) = nullptr;
    void(WINAPI* roUninit)() = nullptr;
    HRESULT(WINAPI* factory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* create)(PCNZWCH, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* del)(HSTRING) = nullptr;
    HRESULT(WINAPI* overStream)(IUnknown*, int, REFIID, void**) = nullptr;
    bool ok = false;
    Api() {
        HMODULE cb = LoadLibraryW(L"combase.dll"), sh = LoadLibraryW(L"shcore.dll");
        if (!cb || !sh) return;
        roInit = (decltype(roInit))(void*)GetProcAddress(cb, "RoInitialize");
        roUninit = (decltype(roUninit))(void*)GetProcAddress(cb, "RoUninitialize");
        factory = (decltype(factory))(void*)GetProcAddress(cb, "RoGetActivationFactory");
        create = (decltype(create))(void*)GetProcAddress(cb, "WindowsCreateString");
        del = (decltype(del))(void*)GetProcAddress(cb, "WindowsDeleteString");
        overStream = (decltype(overStream))(void*)GetProcAddress(sh, "CreateRandomAccessStreamOverStream");
        ok = roInit && roUninit && factory && create && del && overStream;
    }
};

const Api& A() {
    static Api api;
    return api;
}

std::wstring MoonDir() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", ad, MAX_PATH);
    CreateDirectoryW(ad, nullptr);
    return ad;
}

std::wstring SourceFile(const Config& cfg) {
    if (!cfg.lockSource.empty()) return ExpandPath(cfg.lockSource);
    wchar_t wp[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
    return wp;
}

bool JpegClsid(CLSID& out) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/jpeg")) {
            out = codecs[i].Clsid;
            return true;
        }
    return false;
}

double Hue(double r, double g, double b, double& sat) {
    double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    sat = mx > 0 ? d / mx : 0;
    if (d < 1e-6) return 0;
    double h = mx == r ? std::fmod((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    h *= 60;
    return h < 0 ? h + 360 : h;
}

double Smooth(double a, double b, double x) {
    double t = Clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}

// Цветокоррекция под тему (см. описание ApplyAsync).
void Grade(std::vector<uint32_t>& px, int w, int h, const ThemeInfo& t) {
    const double bgR = GetRValue(t.bg) / 255.0, bgG = GetGValue(t.bg) / 255.0, bgB = GetBValue(t.bg) / 255.0;
    const double acR = GetRValue(t.ac) / 255.0, acG = GetGValue(t.ac) / 255.0, acB = GetBValue(t.ac) / 255.0;
    double acSat;
    const double acHue = Hue(acR, acG, acB, acSat);
    const double seamY = h * 0.935;
    uint32_t seed = 0x9E3779B9u;
    for (int y = 0; y < h; ++y) {
        double yy = (y + 0.5) / h, ny = yy * 2 - 1;
        double top = yy < 0.42 ? 0.34 * std::pow(1 - yy / 0.42, 1.6) : 0;
        double bottom = yy > 0.70 ? 0.52 * std::pow((yy - 0.70) / 0.30, 1.5) : 0;
        double dy = y + 0.5 - seamY, seamV = std::exp(-(dy * dy) / 2.2);
        for (int x = 0; x < w; ++x) {
            uint32_t& p = px[(size_t)y * w + x];
            double r = (p >> 16 & 255) / 255.0, g = (p >> 8 & 255) / 255.0, b = (p & 255) / 255.0;
            double L = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            // цвета, близкие к акценту, остаются сочными, остальные притихают
            double s, hu = Hue(r, g, b, s);
            double dh = std::fabs(hu - acHue);
            dh = std::min(dh, 360 - dh);
            double close = s * std::max(0.0, 1 - dh / 55);
            double f = 0.70 + 0.42 * close;
            r = L + (r - L) * f;
            g = L + (g - L) * f;
            b = L + (b - L) * f;
            // тени уходят в графит темы
            double sh = (1 - L) * (1 - L) * 0.30;
            r += (bgR - r) * sh;
            g += (bgG - g) * sh;
            b += (bgB - b) * sh;
            // края темнее, сверху вуаль под часы, снизу под подписи
            double nx = (x + 0.5) / w * 2 - 1;
            double vig = Smooth(0.55, 1.45, std::sqrt(nx * nx * 0.75 + ny * ny)) * 0.45;
            // под аватаром и полем пароля - мягкая тень, чтобы они читались и на чётком фоне
            double ex = (nx) / 0.34, ey = (yy - 0.55) / 0.30;
            double spot = 0.26 * std::exp(-(ex * ex + ey * ey) * 1.6);
            double veil = std::min(0.85, vig + top + bottom + spot - vig * (top + bottom));
            r += (bgR * 0.55 - r) * veil;
            g += (bgG * 0.55 - g) * veil;
            b += (bgB * 0.55 - b) * veil;
            // световой шов цвета акцента
            if (seamV > 0.01) {
                double cx = (x + 0.5) / w - 0.5;
                double a = seamV * std::exp(-(cx * cx) / (2 * 0.16 * 0.16)) * 0.9;
                r += (std::min(1.0, acR * 1.25 + 0.12) - r) * a;
                g += (std::min(1.0, acG * 1.25 + 0.12) - g) * a;
                b += (std::min(1.0, acB * 1.25 + 0.12) - b) * a;
            }
            // зерно: без него плавные вуали в JPEG идут полосами
            seed = seed * 1664525u + 1013904223u;
            double n = ((seed >> 24) / 255.0 - 0.5) * 2.4 / 255.0;
            auto q = [&](double v) { return (uint32_t)std::lround(Clamp01(v + n) * 255); };
            p = 0xFF000000u | q(r) << 16 | q(g) << 8 | q(b);
        }
    }
}

bool SaveJpeg(const std::vector<uint32_t>& px, int w, int h, const std::wstring& file) {
    Bitmap bmp(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)px.data());
    CLSID jpg;
    if (!JpegClsid(jpg)) return false;
    EncoderParameters ep{};
    ULONG quality = 95;
    ep.Count = 1;
    ep.Parameter[0].Guid = EncoderQuality;
    ep.Parameter[0].Type = EncoderParameterValueTypeLong;
    ep.Parameter[0].NumberOfValues = 1;
    ep.Parameter[0].Value = &quality;
    return bmp.Save(file.c_str(), &jpg, &ep) == Ok;
}

// Картинку на экран блокировки ставит WinRT LockScreen.SetImageStreamAsync (без прав администратора).
bool SetLockImage(const std::wstring& file, std::wstring& err) {
    const Api& a = A();
    if (!a.ok) {
        err = L"нет WinRT";
        return false;
    }
    a.roInit(RO_INIT_MULTITHREADED);
    bool ok = false;
    {
        HSTRING cls = nullptr;
        const wchar_t* name = L"Windows.System.UserProfile.LockScreen";
        a.create(name, (UINT32)wcslen(name), &cls);
        Ptr<ILockScreenStatics> ls;
        HRESULT hr = a.factory(cls, IID_LockScreenStatics, (void**)&ls);
        a.del(cls);
        IStream* raw = nullptr;
        if (SUCCEEDED(hr))
            hr = SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &raw);
        Ptr<IInspectable> ras;
        if (SUCCEEDED(hr)) hr = a.overStream(raw, 0, IID_RandomAccessStream, (void**)&ras);
        if (raw) raw->Release();
        Ptr<IInspectable> action;
        if (SUCCEEDED(hr)) hr = ls->SetImageStreamAsync(ras.p, &action);
        Ptr<IAsyncInfoAbi> info;
        if (SUCCEEDED(hr)) hr = action->QueryInterface(IID_AsyncInfo, (void**)&info);
        if (SUCCEEDED(hr)) {
            ULONGLONG until = GetTickCount64() + 10000;
            int st = 0;
            while (SUCCEEDED(info->get_Status(&st)) && st == 0 && GetTickCount64() < until) Sleep(10);
            if (st == 1) {
                ok = true;
            } else {
                HRESULT code = E_FAIL;
                info->get_ErrorCode(&code);
                hr = st == 0 ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : code;
            }
        }
        if (!ok) {
            wchar_t b[32];
            swprintf(b, 32, L"ошибка 0x%08lX", (unsigned long)hr);
            err = b;
        }
    }
    a.roUninit();
    return ok;
}

struct Work {
    std::shared_ptr<Job> job;
    std::wstring source;
    ThemeInfo theme;
};

DWORD WINAPI Worker(void* p) {
    std::unique_ptr<Work> w((Work*)p);
    RECT mr = PrimaryRect();
    int W = mr.right - mr.left, H = mr.bottom - mr.top;
    std::vector<uint32_t> px;
    std::wstring msg, err;
    bool ok = false;
    if (w->source.empty() || !Exists(w->source)) {
        msg = L"Нет картинки: укажи её или поставь обои";
    } else if (!LoadCover(w->source, W, H, px)) {
        msg = L"Не получилось открыть картинку";
    } else {
        Grade(px, W, H, w->theme);
        if (!SaveJpeg(px, W, H, OutputFile()))
            msg = L"Не получилось сохранить картинку";
        else if (!SetLockImage(OutputFile(), err))
            msg = L"Windows не приняла картинку: " + err;
        else {
            ok = true;
            msg = L"Экран блокировки обновлён - проверь Win + L";
        }
    }
    AcquireSRWLockExclusive(&w->job->lock);
    w->job->ok = ok;
    w->job->message = msg;
    ReleaseSRWLockExclusive(&w->job->lock);
    w->job->done = true;
    if (w->job->notify) InvalidateRect(w->job->notify, nullptr, FALSE);
    return 0;
}

}  // namespace

namespace detail {

double InOut(double t) {
    t = Clamp01(t);
    return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

RECT PrimaryRect() {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

// Картинка, растянутая с обрезкой на весь прямоугольник w x h; пиксели BGRA, непрозрачные.
bool LoadCover(const std::wstring& file, int w, int h, std::vector<uint32_t>& px) {
    Bitmap src(file.c_str());
    if (src.GetLastStatus() != Ok || !src.GetWidth() || !src.GetHeight()) return false;
    Bitmap dst(w, h, PixelFormat32bppARGB);
    {
        Graphics g(&dst);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        double s = std::max((double)w / src.GetWidth(), (double)h / src.GetHeight());
        double dw = src.GetWidth() * s, dh = src.GetHeight() * s;
        g.DrawImage(&src, RectF((REAL)((w - dw) / 2), (REAL)((h - dh) / 2), (REAL)dw, (REAL)dh));
    }
    BitmapData bd;
    Rect r(0, 0, w, h);
    if (dst.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return false;
    px.resize((size_t)w * h);
    for (int y = 0; y < h; ++y) memcpy(&px[(size_t)y * w], (BYTE*)bd.Scan0 + (size_t)y * bd.Stride, (size_t)w * 4);
    dst.UnlockBits(&bd);
    for (auto& p : px) p |= 0xFF000000u;
    return true;
}

}  // namespace detail

std::wstring OutputFile() { return MoonDir() + L"\\lockscreen-theme.jpg"; }

bool BuildAvatar(const Config& cfg, std::wstring& out, std::wstring& msg) {
    constexpr int S = 448;
    const ThemeInfo t = CurrentTheme(cfg);
    const RingColors pal = RingPalette(cfg);
    const REAL c = S / 2.0f, R = S / 2.0f;
    Bitmap bmp(S, S, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.Clear(ToColor(t.bg));

    // фон: графит с подсветкой из середины (Windows сама обрежет картинку кругом)
    {
        GraphicsPath disk;
        disk.AddEllipse(-R * 0.2f, -R * 0.2f, S + R * 0.4f, S + R * 0.4f);
        PathGradientBrush pg(&disk);
        pg.SetCenterPoint(PointF(c, c * 0.92f));
        pg.SetCenterColor(MixColor(ToColor(t.panel), ToColor(t.ac), 0.10));
        Color edge = ToColor(t.bg);
        int n = 1;
        pg.SetSurroundColors(&edge, &n);
        g.FillPath(&pg, &disk);
    }

    // кольцо секторов, как у меню: один сектор «наведён» - цвет акцента и выдвинут наружу
    const int kSectors = 7;
    const REAL rIn = R * 0.60f, rOut = R * 0.84f, gapDeg = 3.2f, hotLift = R * 0.035f;
    const int hot = 1;
    for (int i = 0; i < kSectors; ++i) {
        REAL a0 = -90.0f - 180.0f / kSectors + i * 360.0f / kSectors + gapDeg / 2;
        REAL sweep = 360.0f / kSectors - gapDeg;
        REAL ro = rOut + (i == hot ? hotLift : 0);
        GraphicsPath p;
        p.AddArc(c - ro, c - ro, 2 * ro, 2 * ro, a0, sweep);
        p.AddArc(c - rIn, c - rIn, 2 * rIn, 2 * rIn, a0 + sweep, -sweep);
        p.CloseFigure();
        if (i == hot) {
            double mid = Rad(a0 + sweep / 2);
            PointF pin((REAL)(c + std::cos(mid) * rIn), (REAL)(c + std::sin(mid) * rIn));
            PointF pout((REAL)(c + std::cos(mid) * ro), (REAL)(c + std::sin(mid) * ro));
            LinearGradientBrush fill(pin, pout, WithAlpha(pal.hotIn, 245), WithAlpha(pal.hotOut, 245));
            fill.SetWrapMode(WrapModeTileFlipXY);
            g.FillPath(&fill, &p);
            Pen rim(WithAlpha(pal.rim, 235), R * 0.022f);
            rim.SetStartCap(LineCapRound);
            rim.SetEndCap(LineCapRound);
            REAL rr = ro + R * 0.045f;
            g.DrawArc(&rim, c - rr, c - rr, 2 * rr, 2 * rr, a0 + 1.5f, sweep - 3);
        } else {
            SolidBrush fill(WithAlpha(MixColor(ToColor(t.panel), ToColor(t.muted), 0.22), 240));
            g.FillPath(&fill, &p);
        }
        Pen border(WithAlpha(pal.border, 110), 1.6f);
        g.DrawPath(&border, &p);
    }

    // середина: своя картинка в круге или серебряный полумесяц со свечением акцента
    const REAL rc = R * 0.52f;
    std::wstring src = cfg.avatarSource.empty() ? L"" : ExpandPath(cfg.avatarSource);
    std::unique_ptr<Bitmap> photo;
    if (!src.empty()) {
        photo = std::make_unique<Bitmap>(src.c_str());
        if (photo->GetLastStatus() != Ok || !photo->GetWidth()) photo.reset();
    }
    for (int i = 6; i >= 1; --i) {  // мягкое свечение под серединой
        REAL rr = rc + R * 0.035f * i;
        SolidBrush glow(WithAlpha(ToColor(t.ac), (BYTE)(10 + 4 * (6 - i))));
        g.FillEllipse(&glow, c - rr, c - rr, 2 * rr, 2 * rr);
    }
    SolidBrush hub(MixColor(ToColor(t.bg), ToColor(t.panel), 0.55));
    g.FillEllipse(&hub, c - rc, c - rc, 2 * rc, 2 * rc);
    if (photo) {
        GraphicsPath clip;
        clip.AddEllipse(c - rc, c - rc, 2 * rc, 2 * rc);
        GraphicsState st = g.Save();
        g.SetClip(&clip);
        double s = 2 * rc / std::min(photo->GetWidth(), photo->GetHeight());
        REAL dw = (REAL)(photo->GetWidth() * s), dh = (REAL)(photo->GetHeight() * s);
        g.DrawImage(photo.get(), RectF(c - dw / 2, c - dh / 2, dw, dh));
        g.Restore(st);
    } else {
        // полумесяц: круг без смещённого круга, заливка от серебра к свету
        REAL rm = rc * 0.62f;
        GraphicsPath full, bite;
        full.AddEllipse(c - rm, c - rm, 2 * rm, 2 * rm);
        bite.AddEllipse(c - rm + rm * 0.52f, c - rm - rm * 0.30f, 2 * rm, 2 * rm);
        Region moon(&full);
        moon.Exclude(&bite);
        LinearGradientBrush mg(PointF(c - rm, c - rm), PointF(c + rm, c + rm), ToColor(t.fg), ToColor(t.gold));
        g.FillRegion(&mg, &moon);
        // маленькая звезда-искра цвета акцента
        REAL sx = c + rm * 0.55f, sy = c - rm * 0.05f, sr = rm * 0.13f;
        PointF star[8];
        for (int k = 0; k < 8; ++k) {
            double a = Rad(-90 + k * 45.0);
            REAL d = k % 2 ? sr * 0.32f : sr;
            star[k] = PointF((REAL)(sx + std::cos(a) * d), (REAL)(sy + std::sin(a) * d));
        }
        SolidBrush sb(MixColor(ToColor(t.ac), Color(255, 255, 255, 255), 0.35));
        g.FillPolygon(&sb, star, 8);
    }
    Pen hubRing(WithAlpha(pal.hubRing, 230), R * 0.018f);
    g.DrawEllipse(&hubRing, c - rc, c - rc, 2 * rc, 2 * rc);

    out = MoonDir() + L"\\avatar.png";
    CLSID png{};
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, codecs);
    bool found = false;
    for (UINT i = 0; i < n; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png")) {
            png = codecs[i].Clsid;
            found = true;
        }
    if (!found || bmp.Save(out.c_str(), &png, nullptr) != Ok) {
        msg = L"Не получилось сохранить аватар";
        return false;
    }
    msg = L"Аватар готов";
    return true;
}

std::shared_ptr<Job> ApplyAsync(const Config& cfg, HWND notify) {
    auto job = std::make_shared<Job>();
    job->notify = notify;
    auto* w = new Work{job, SourceFile(cfg), CurrentTheme(cfg)};
    if (HANDLE th = CreateThread(nullptr, 0, Worker, w, 0, nullptr)) {
        CloseHandle(th);
    } else {
        delete w;
        job->message = L"Не получилось запустить сборку";
        job->done = true;
    }
    return job;
}

}  // namespace lockscreen
