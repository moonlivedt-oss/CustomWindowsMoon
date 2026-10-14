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
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/files.h"
#include "core/fx.h"
#include "core/perf.h"
#include "gfx/sound.h"
#include "core/themes.h"
#include "desk/whreg.h"
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

namespace {

const IID IID_AsyncInfo = {0x00000036, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const IID IID_LockScreenStatics = {0x3EE9D3AD, 0xB607, 0x40AE, {0xB4, 0x26, 0x76, 0x31, 0xD9, 0x82, 0x12, 0x69}};
const IID IID_RandomAccessStream = {0x905A0FE1, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};
constexpr wchar_t kClass[] = L"MoonRadialLockReveal";
constexpr UINT_PTR kTimer = 1, kWakeTimer = 2, kInputTimer = 3;
constexpr double kCurtainMs = 300, kFadeMs = 720, kFadeDelayMs = 120;
constexpr double kPortalMs = 1900, kPortalCloseMs = 950;

double InOut(double t) {
    t = Clamp01(t);
    return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

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

Reveal* g_reveal = nullptr;

}  // namespace

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

// ---------- переход между блокировкой и столом ----------

void Reveal::Create(HINSTANCE inst, HWND sessionWnd) {
    g_reveal = this;
    session_ = sessionWnd;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
}

void Reveal::Destroy() {
    Hide();
    if (wnd_) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

void Reveal::Apply(const Config& cfg) {
    enabled_ = cfg.lockReveal;
    portal_ = cfg.lockPortal;
    pattern_ = cfg.lockPattern;
    bootPortal_ = cfg.bootPortal;
    wakePortal_ = cfg.wakePortal;
    sounds_ = cfg.sounds;
    theme_ = CurrentTheme(cfg);
}

LRESULT CALLBACK Reveal::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_TIMER && wp == kTimer && g_reveal) {
        g_reveal->Tick();
        return 0;
    }
    if (msg == WM_TIMER && wp == kWakeTimer && g_reveal) {
        KillTimer(h, kWakeTimer);
        if (g_reveal->state_ == HOLD && g_reveal->waitWake_ && !g_reveal->locked_) g_reveal->Open();
        return 0;
    }
    if (msg == WM_TIMER && wp == kInputTimer && g_reveal) {
        // запасной путь: экран мог включиться без уведомления - смотрим, был ли ввод после того, как всё погасло
        LASTINPUTINFO li{sizeof(li)};
        auto& r = *g_reveal;
        if (r.state_ != HOLD || !r.waitWake_) {
            KillTimer(h, kInputTimer);
        } else if (GetLastInputInfo(&li) && li.dwTime > (DWORD)(r.holdAt_ + 1500) && !r.locked_) {
            KillTimer(h, kInputTimer);
            r.Open();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool Reveal::Prepare() {
    if (memDC_) return true;
    mon_ = PrimaryRect();
    int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    std::wstring file = OutputFile();
    if (!Exists(file)) {
        // своей картинки ещё нет - берём ту, что стоит на экране блокировки сейчас
        wchar_t wp[MAX_PATH] = {};
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
        file = wp;
    }
    std::vector<uint32_t> px;
    if (file.empty() || !LoadCover(file, W, H, px)) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    void* bits = nullptr;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_) {
        DeleteDC(memDC_);
        memDC_ = nullptr;
        return false;
    }
    old_ = SelectObject(memDC_, dib_);
    bits_ = (uint32_t*)bits;
    if (whreg::LogonCrisp()) {
        // экран входа показывает картинку как есть - копия такая же, иначе на входе будет скачок
        src_ = std::move(px);
        Plain();
        if (pattern_) BuildCells();
        return true;
    }
    // размытие как у экрана входа: уменьшить, размыть, растянуть, притемнить
    constexpr int kDown = 8;
    int sw = std::max(1, W / kDown), sh = std::max(1, H / kDown);
    std::vector<uint32_t> small((size_t)sw * sh);
    Downscale(px.data(), W, H, small.data(), sw, sh, kDown);
    BoxBlur(small.data(), sw, sh, 4);
    for (auto& p : small) {
        uint32_t r = (p >> 16 & 255) * 72 / 100, g = (p >> 8 & 255) * 72 / 100, b = (p & 255) * 72 / 100;
        p = 0xFF000000u | r << 16 | g << 8 | b;
    }
    src_.resize((size_t)W * H);
    UpscaleBilinear(small.data(), sw, sh, src_.data(), W, H, W);
    Plain();
    if (pattern_) BuildCells();
    return true;
}

void Reveal::Plain() { memcpy(bits_, src_.data(), src_.size() * 4); }

// Узор стекла: соты или осколки. Для каждого пикселя - номер ячейки и насколько
// он далеко от её середины (0 - середина, 255 - граница), для каждой ячейки -
// середина и случайный сдвиг. Считается один раз при блокировке (около 50 мс),
// потом кадры только читают таблицы.
void Reveal::BuildCells() {
    PERF_SCOPE("lock.BuildCells");
    const int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    const double k = H / 1080.0;
    cell_.assign((size_t)W * H, 0);
    edge_.assign((size_t)W * H, 0);
    tex_.assign((size_t)W * H, 0);
    cellX_.clear();
    cellY_.clear();
    cellJit_.clear();
    auto hash = [](int a, int b) {
        uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return (h & 0xFFFFFF) / 16777216.0;
    };
    std::unordered_map<int64_t, uint16_t> ids;
    auto id = [&](int a, int b, double x, double y) {
        int64_t key = (int64_t)(a + 100000) << 32 | (uint32_t)(b + 100000);
        auto it = ids.find(key);
        if (it != ids.end()) return it->second;
        uint16_t n = (uint16_t)cellX_.size();
        ids.emplace(key, n);
        cellX_.push_back((float)x);
        cellY_.push_back((float)y);
        cellJit_.push_back((float)hash(a + 7, b - 3));
        return n;
    };

    if (pattern_ == 1) {
        // соты с плоским верхом: s - радиус до угла
        const double s = 34 * k, a = s * std::sqrt(3.0) / 2;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                double px = x + 0.5, py = y + 0.5;
                double q = (2.0 / 3 * px) / s, r = (-1.0 / 3 * px + std::sqrt(3.0) / 3 * py) / s;
                // округление кубических координат
                double cxq = q, czq = r, cyq = -q - r;
                double rx = std::round(cxq), ry = std::round(cyq), rz = std::round(czq);
                double dx = std::fabs(rx - cxq), dy = std::fabs(ry - cyq), dz = std::fabs(rz - czq);
                if (dx > dy && dx > dz)
                    rx = -ry - rz;
                else if (dy > dz)
                    ry = -rx - rz;
                else
                    rz = -rx - ry;
                int iq = (int)rx, ir = (int)rz;
                double hx = s * 1.5 * iq, hy = s * std::sqrt(3.0) * (ir + iq / 2.0);
                double ox = std::fabs(px - hx), oy = std::fabs(py - hy);
                double m = std::max(oy, ox * std::sqrt(3.0) / 2 + oy / 2) / a;  // 0 середина, 1 грань
                size_t i = (size_t)y * W + x;
                cell_[i] = id(iq, ir, hx, hy);
                edge_[i] = (uint8_t)std::min(255.0, m * 255);
            }
        }
    } else {
        // осколки: ячейки Вороного на сетке со случайными центрами - вытянутые
        // грани, как трещины
        const double g = 88 * k;
        int gw = (int)(W / g) + 2, gh = (int)(H / g) + 2;
        std::vector<float> sx((size_t)gw * gh), sy((size_t)gw * gh);
        for (int j = 0; j < gh; ++j)
            for (int i = 0; i < gw; ++i) {
                sx[(size_t)j * gw + i] = (float)((i + 0.1 + 0.8 * hash(i, j)) * g);
                sy[(size_t)j * gw + i] = (float)((j + 0.1 + 0.8 * hash(j + 31, i - 17)) * g);
            }
        for (int y = 0; y < H; ++y) {
            int gj = (int)(y / g);
            for (int x = 0; x < W; ++x) {
                int gi = (int)(x / g);
                double d1 = 1e18, d2 = 1e18;
                int bi = 0, bj = 0;
                for (int j = std::max(0, gj - 1); j <= std::min(gh - 1, gj + 1); ++j)
                    for (int i = std::max(0, gi - 1); i <= std::min(gw - 1, gi + 1); ++i) {
                        double ddx = x + 0.5 - sx[(size_t)j * gw + i], ddy = y + 0.5 - sy[(size_t)j * gw + i];
                        double d = ddx * ddx + ddy * ddy;
                        if (d < d1) {
                            d2 = d1;
                            d1 = d;
                            bi = i;
                            bj = j;
                        } else if (d < d2) {
                            d2 = d;
                        }
                    }
                d1 = std::sqrt(d1);
                d2 = std::sqrt(d2);
                double m = 2 * d1 / (d1 + d2);  // 0 у центра, 1 на трещине
                size_t idx = (size_t)y * W + x;
                cell_[idx] = id(bi, bj, sx[(size_t)bj * gw + bi], sy[(size_t)bj * gw + bi]);
                edge_[idx] = (uint8_t)std::min(255.0, m * 255);
            }
        }
    }

    // фактура стекла: каждая грань чуть своей яркости, по границам тёмная линия с
    // тонким бликом рядом
    for (size_t i = 0; i < tex_.size(); ++i) {
        double m = edge_[i] / 255.0;
        double facet = (cellJit_[cell_[i]] - 0.5) * (pattern_ == 1 ? 0.10 : 0.16);
        double line = m > 0.90 ? -0.55 * std::pow((m - 0.90) / 0.10, 1.5) : 0;
        double glint = m > 0.76 && m <= 0.90 ? 0.12 * std::sin((m - 0.76) / 0.14 * kPi) : 0;
        tex_[i] = (int8_t)std::clamp((facet + line + glint) * 127, -127.0, 127.0);
    }
}

// Кадр портала; t от 0 до 1 - вся сцена, в обратную сторону она же схлопывает
// портал.
//   0.00-0.30  зажигание: на стекле проступает узор, секторы по одному встают
//   вокруг курсора,
//              последним вспыхивает багровый
//   0.16-1.00  раскрытие: фронт идёт от курсора к краям экрана; с узором стекло
//   рассыпается
//              по ячейкам (каждая сжимается к своей середине и гаснет, ячейки
//              на фронте вспыхивают кромкой), без узора - круглый вырез с тенью
//              и светлой кромкой
void Reveal::Portal(double t) {
    PERF_SCOPE("lock.Portal");
    const int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    const double k = H / 1080.0;
    const double cx = origin_.x,
                 cy = origin_.y;  // откуда раскрывается: где был курсор
    const double maxR = std::hypot(std::max(cx, W - cx), std::max(cy, H - cy));

    const double ig = Clamp01(t / 0.30);           // зажигание
    const double op = Clamp01((t - 0.16) / 0.84);  // раскрытие
    const double e = InOut(op);
    const double vis = std::pow(1 - Clamp01((op - 0.30) / 0.70),
                                1.3);  // кольцо и кромка тают к концу
    const Color ac = ToColor(theme_.ac), hi = MixColor(ac, Color(255, 255, 255, 255), 0.45);
    const uint32_t hr = hi.GetR(), hg = hi.GetG(), hb = hi.GetB();
    double hole;  // где сейчас фронт

    if (pattern_ && !cell_.empty()) {
        const double span = (pattern_ == 1 ? 120 : 150) * k;  // за сколько пикселей позади фронта ячейка исчезает
        hole = e * (maxR + 2 * span);
        // на каждую ячейку: сколько от неё осталось (по радиусу), прозрачность,
        // накал кромки
        const size_t n = cellX_.size();
        keep_.resize(n);
        fade_.resize(n);
        heat_.resize(n);
        lift_.resize(n);
        lightX_.resize(n);
        lightY_.resize(n);
        // приподнятые соты: вокруг курсора поднимаются, пока зажигается кольцо,
        // и волна приподнятых бежит перед фронтом
        const double liftR = 230 * k, igE = EaseOut(ig);
        for (size_t c = 0; c < n; ++c) {
            double d = std::hypot(cellX_[c] - cx, cellY_[c] - cy) + (cellJit_[c] - 0.5) * span * 0.9;
            double p = Clamp01((hole - d) / span);                              // 0 - цела, 1 - исчезла
            double front = std::exp(-std::pow((d - hole) / (span * 0.55), 2));  // ячейки у фронта горят
            keep_[c] = (uint16_t)(256 * (1 - p * p * (3 - 2 * p)));
            fade_[c] = (uint16_t)(256 * (1 - p * p * p));
            heat_[c] = (uint16_t)(256 * std::min(1.0, front * vis * 1.1));
            double dc = std::hypot(cellX_[c] - cx, cellY_[c] - cy);
            double close = std::exp(-std::pow(dc / liftR, 2)) * igE;
            double ahead = op > 0 ? 0.8 * std::exp(-std::pow((d - hole - span * 0.9) / (span * 0.7), 2)) : 0;
            lift_[c] = (uint16_t)(256 * std::max(close, ahead) * (1 - p));
            // свет идёт от курсора: грань, обращённая к нему, светлее
            double lx = cx - cellX_[c], ly = cy - cellY_[c], ll = std::max(1.0, std::hypot(lx, ly));
            lightX_[c] = (float)(lx / ll);
            lightY_[c] = (float)(ly / ll);
        }
        const int pat = (int)(256 * EaseOut(ig));  // узор проступает при зажигании
        for (size_t i = 0, total = (size_t)W * H; i < total; ++i) {
            uint16_t c = cell_[i];
            uint32_t ed = edge_[i], kp = keep_[c];
            if (ed >= (kp * 255 >> 8) && kp < 256) {
                bits_[i] = 0;
                continue;
            }
            uint32_t p = src_[i];
            int m = 256 + (pat * tex_[i] >> 7);
            int lit = 0;  // доля света с курсора на грани приподнятой соты
            if (uint32_t lf = lift_[c]) {
                m += (int)(lf * 56 >> 8);  // приподнятая сота светлее
                if (ed > 150) {
                    // направление от середины соты к пикселю
                    int y = (int)(i / W), x = (int)(i - (size_t)y * W);
                    double dx = x + 0.5 - cellX_[c], dy = y + 0.5 - cellY_[c];
                    double dl = std::max(1.0, std::sqrt(dx * dx + dy * dy));
                    double sh = (dx * lightX_[c] + dy * lightY_[c]) / dl;  // +1 к свету, -1 от него
                    double rw = (ed - 150) / 105.0;
                    m += (int)(lf * rw * sh * 150 / 256);
                    if (sh > 0) lit = (int)(lf * rw * sh * 0.7);
                    if (ed > 230) m -= (int)(lf * 110 >> 8);  // шов под приподнятой сотой темнее - тень
                }
            }
            m = std::max(m, 0);  // тень на гранях может увести множитель в минус - цвет переполнится
            int r = std::min(255, (int)(p >> 16 & 255) * m >> 8), g = std::min(255, (int)(p >> 8 & 255) * m >> 8),
                b = std::min(255, (int)(p & 255) * m >> 8);
            if (lit) {
                r += ((int)hr - r) * lit >> 8;
                g += ((int)hg - g) * lit >> 8;
                b += ((int)hb - b) * lit >> 8;
            }
            // кромка ячейки на фронте: светится у оставшейся границы
            uint32_t ht = heat_[c];
            if (ht) {
                int lim = (int)(kp * 255 >> 8);
                int dist = lim - (int)ed;  // насколько пиксель внутри от края того, что осталось
                if (dist < 40) {
                    int gl = (int)ht * (40 - dist) / 40;
                    r += ((int)hr - r) * gl >> 8;
                    g += ((int)hg - g) * gl >> 8;
                    b += ((int)hb - b) * gl >> 8;
                }
            }
            uint32_t a = fade_[c];
            bits_[i] = (std::min(255u, 255 * a >> 8)) << 24 | (uint32_t)(r * a >> 8) << 16 |
                       (uint32_t)(g * a >> 8) << 8 | (uint32_t)(b * a >> 8);
        }
    } else {
        const double f = 30 * k;   // мягкость края картинки
        const double sh = 46 * k;  // тень внутри выреза: портал «глубже» стола
        hole = e * (maxR + 2 * f);
        const double inner = hole - f, outer = hole + f, deep = inner - sh;
        const double rim = 0.32 * vis, shadow = 0.38 * vis;
        for (int y = 0; y < H; ++y) {
            uint32_t* row = bits_ + (size_t)y * W;
            const uint32_t* src = src_.data() + (size_t)y * W;
            double dy = y + 0.5 - cy, dy2 = dy * dy;
            if (outer <= 0 || dy2 >= outer * outer) {
                memcpy(row, src, (size_t)W * 4);
                continue;
            }
            int xo0 = std::clamp((int)std::floor(cx - std::sqrt(outer * outer - dy2)), 0, W);
            int xo1 = std::clamp((int)std::ceil(cx + std::sqrt(outer * outer - dy2)), xo0, W);
            int xi0 = xo1, xi1 = xo1;
            if (deep > 0 && dy2 < deep * deep) {
                double half = std::sqrt(deep * deep - dy2);
                xi0 = std::clamp((int)std::ceil(cx - half), xo0, xo1);
                xi1 = std::clamp((int)std::floor(cx + half), xi0, xo1);
            }
            memcpy(row, src, (size_t)xo0 * 4);
            memcpy(row + xo1, src + xo1, (size_t)(W - xo1) * 4);
            if (xi1 > xi0) memset(row + xi0, 0, (size_t)(xi1 - xi0) * 4);
            auto band = [&](int a, int b) {
                for (int x = a; x < b; ++x) {
                    double dx = x + 0.5 - cx, d = std::sqrt(dx * dx + dy2);
                    double s = Clamp01((d - deep) / sh);
                    if (d < inner) {
                        row[x] = (uint32_t)(shadow * s * s * 255) << 24;
                        continue;
                    }
                    double m = Clamp01((d - inner) / (2 * f));
                    m = m * m * (3 - 2 * m);
                    double glow = rim * std::exp(-std::fabs(d - hole) / (0.8 * f));
                    uint32_t p = src[x];
                    double r = (p >> 16 & 255) * m, g = (p >> 8 & 255) * m, b = (p & 255) * m, a = 255 * m;
                    r += (hr - r) * glow;
                    g += (hg - g) * glow;
                    b += (hb - b) * glow;
                    a += (255 - a) * glow;
                    a = a + (255 - a) * shadow * s * s * (1 - m);
                    row[x] = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
                }
            };
            band(xo0, xi1 > xi0 ? xi0 : xo1);
            if (xi1 > xi0) band(xi1, xo1);
        }
    }

    Bitmap surf(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
    Graphics g(&surf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    const Color steel = MixColor(ToColor(theme_.muted), ToColor(theme_.fg), 0.25);

    // вспышка у курсора в момент, когда портал начинает раскрываться
    double bloom = std::exp(-std::pow((op - 0.06) / 0.09, 2)) * (t > 0.12 ? 1 : 0);
    if (bloom > 0.01) {
        double br = 70 * k + std::min(hole, 400 * k) * 1.2 + 60 * k * op;
        GraphicsPath bp;
        bp.AddEllipse((REAL)(cx - br), (REAL)(cy - br), (REAL)(2 * br), (REAL)(2 * br));
        PathGradientBrush pg(&bp);
        pg.SetCenterPoint(PointF((REAL)cx, (REAL)cy));
        pg.SetCenterColor(WithAlpha(hi, (BYTE)(150 * bloom)));
        Color edge = WithAlpha(ac, 0);
        int n = 1;
        pg.SetSurroundColors(&edge, &n);
        g.FillPath(&pg, &bp);
    }

    // без узора - кромка выреза и бегущая по ней искра
    if (!pattern_ && hole > 2 && vis > 0.003) {
        for (int i = 3; i >= 1; --i) {
            Pen glow(WithAlpha(ac, (BYTE)(36 * vis * (4 - i))), (REAL)(i * 9 * k));
            g.DrawEllipse(&glow, (REAL)(cx - hole), (REAL)(cy - hole), (REAL)(2 * hole), (REAL)(2 * hole));
        }
        Pen core(WithAlpha(hi, (BYTE)(235 * vis)), (REAL)(2.2 * k));
        g.DrawEllipse(&core, (REAL)(cx - hole), (REAL)(cy - hole), (REAL)(2 * hole), (REAL)(2 * hole));
        if (hole > 30 * k) {
            double ca = -90 + 520 * e;
            for (int i = 0; i < 6; ++i) {
                Pen tail(WithAlpha(i ? hi : Color(255, 255, 255, 255), (BYTE)(230 * vis * (1 - i / 6.0))),
                         (REAL)((3.4 - i * 0.4) * k));
                tail.SetStartCap(LineCapRound);
                tail.SetEndCap(LineCapRound);
                g.DrawArc(&tail, (REAL)(cx - hole), (REAL)(cy - hole), (REAL)(2 * hole), (REAL)(2 * hole),
                          (REAL)(ca - i * 6), 6.5f);
            }
        }
    }

    // кольцо секторов, как на аватаре: встают по одному, потом идут впереди
    // фронта, поворачиваются и тают
    const int kSectors = 7, kHot = 1;
    const double ringR = 96 * k + hole;
    const double rot = -90 - 180.0 / kSectors + 150 * e;
    const double gap = 3.2, width = 22 * k * (1 + 0.6 * e);
    for (int j = 0; j < kSectors; ++j) {
        int i = (kHot + 1 + j) % kSectors;  // по часовой, «наведённый» последним
        double sp = Clamp01((ig - j * 0.09) / 0.36);
        if (sp <= 0) continue;
        double pop = EaseOutBack(sp, 1.8);
        double a0 = rot + i * 360.0 / kSectors + gap / 2, sweep = (360.0 / kSectors - gap) * std::min(1.0, pop);
        bool hot = i == kHot;
        double rr = ringR * (0.82 + 0.18 * pop) + width / 2 + (hot ? 6 * k : 0);
        double a = std::min(1.0, sp * 2) * vis;
        Pen seg(WithAlpha(hot ? ac : steel, (BYTE)((hot ? 235 : 150) * a)), (REAL)width);
        g.DrawArc(&seg, (REAL)(cx - rr), (REAL)(cy - rr), (REAL)(2 * rr), (REAL)(2 * rr), (REAL)a0, (REAL)sweep);
        if (hot) {
            double flash = std::exp(-std::pow((sp - 0.55) / 0.2, 2));
            double r2 = rr + width / 2 + 7 * k;
            Pen rimPen(WithAlpha(hi, (BYTE)(std::min(255.0, 220 * a + 35 * flash))), (REAL)((2.6 + 2.4 * flash) * k));
            rimPen.SetStartCap(LineCapRound);
            rimPen.SetEndCap(LineCapRound);
            g.DrawArc(&rimPen, (REAL)(cx - r2), (REAL)(cy - r2), (REAL)(2 * r2), (REAL)(2 * r2), (REAL)(a0 + 1.5),
                      (REAL)std::max(0.0, sweep - 3));
        }
    }

    // искры: срываются с фронта и улетают наружу, как лепестки на картинке
    if (hole > 10 * k && vis > 0.01) {
        uint32_t seed = 0xA341316Cu;
        auto rnd = [&] {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) / 16777216.0;
        };
        SolidBrush dot(Color(0, 0, 0, 0));
        for (int i = 0; i < 34; ++i) {
            double ang = Rad(rnd() * 360), start = 0.05 + rnd() * 0.5, speed = 120 + rnd() * 300,
                   size = (2.4 + rnd() * 3.4) * k;
            double life = Clamp01((op - start) / 0.45);
            if (life <= 0 || life >= 1) continue;
            double born = InOut(start) * maxR;
            double r = born + 96 * k * rnd() + speed * k * EaseOut(life);
            double fade = (1 - life) * (1 - life);
            PointF p((REAL)(cx + std::cos(ang) * r), (REAL)(cy + std::sin(ang) * r));
            PointF q((REAL)(cx + std::cos(ang) * (r - 26 * k * (1 - life))),
                     (REAL)(cy + std::sin(ang) * (r - 26 * k * (1 - life))));
            Pen trail(WithAlpha(ac, (BYTE)(140 * fade)), (REAL)(size * 0.7));
            trail.SetStartCap(LineCapRound);
            trail.SetEndCap(LineCapRound);
            g.DrawLine(&trail, q, p);
            dot.SetColor(WithAlpha(i % 3 ? hi : ac, (BYTE)(230 * fade)));
            g.FillEllipse(&dot, (REAL)(p.X - size / 2), (REAL)(p.Y - size / 2), (REAL)size, (REAL)size);
        }
    }
}
void Reveal::AimAtCursor() {
    POINT pt;
    GetCursorPos(&pt);
    int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    origin_ = {std::clamp((int)(pt.x - mon_.left), 0, W - 1), std::clamp((int)(pt.y - mon_.top), 0, H - 1)};
}

void Reveal::Push(BYTE alpha) {
    HDC scr = GetDC(nullptr);
    POINT pos{mon_.left, mon_.top}, src{0, 0};
    SIZE size{mon_.right - mon_.left, mon_.bottom - mon_.top};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &pos, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Reveal::Open() {
    KillTimer(wnd_, kWakeTimer);
    KillTimer(wnd_, kInputTimer);
    waitWake_ = false;
    AimAtCursor();
    state_ = FADE;
    sounded_ = false;
    t0_ = GetTickCount64();
    SetTimer(wnd_, kTimer, 15, nullptr);
}

void Reveal::Await() {
    state_ = HOLD;
    waitWake_ = true;
    holdAt_ = GetTickCount64();
    KillTimer(wnd_, kTimer);
    SetTimer(wnd_, kInputTimer, 400, nullptr);
}

void Reveal::Intro() {
    if (!enabled_ || !bootPortal_ || fx::Game() || state_ != IDLE || !Prepare()) return;
    AimAtCursor();
    state_ = INTRO;
    t0_ = GetTickCount64();
    Push(0);
    SetTimer(wnd_, kTimer, 15, nullptr);
}

bool Reveal::ScreenOff() {
    if (!enabled_ || !wakePortal_ || fx::Game() || state_ != IDLE || !Prepare()) return false;
    offAfter_ = true;
    state_ = CURTAIN;
    AimAtCursor();
    t0_ = GetTickCount64();
    if (portal_) Portal(1);
    Push(0);
    SetTimer(wnd_, kTimer, 15, nullptr);
    return true;
}

void Reveal::OnDisplay(bool on) {
    // экран включился: чуть подождать - вдруг вместе с ним придёт блокировка
    if (on && state_ == HOLD && waitWake_) SetTimer(wnd_, kWakeTimer, 600, nullptr);
}

void Reveal::OnPower(WPARAM event) {
    if (event == PBT_APMSUSPEND) {
        if (!enabled_ || !wakePortal_ || fx::Game() || state_ != IDLE || !Prepare()) return;
        Push(255);  // перед сном: после пробуждения стол откроется порталом
        Await();
    } else if (event == PBT_APMRESUMEAUTOMATIC || event == PBT_APMRESUMESUSPEND) {
        if (state_ == HOLD && waitWake_) SetTimer(wnd_, kWakeTimer, 900, nullptr);
    }
}

void Reveal::OnSession(WPARAM code) {
    if (code == WTS_SESSION_LOCK) locked_ = true;
    if (code == WTS_SESSION_UNLOCK) locked_ = false;
    if (code == WTS_SESSION_LOCK) {
        if (state_ == HOLD || !enabled_ || fx::Game()) return;
        KillTimer(wnd_, kTimer);
        if (!Prepare()) return;
        Push(255);  // за экраном блокировки: на стол его не видно
        state_ = HOLD;
    } else if (code == WTS_SESSION_UNLOCK) {
        if (state_ == HOLD) Open();
    }
}

void Reveal::LockWithCurtain() {
    if (!enabled_ || fx::Game() || !Prepare()) {
        LockWorkStation();
        return;
    }
    state_ = CURTAIN;
    AimAtCursor();
    t0_ = GetTickCount64();
    if (portal_) Portal(1);
    Push(0);
    SetTimer(wnd_, kTimer, 15, nullptr);
}

void Reveal::Demo() {
    if (state_ != IDLE) return;
    // картинку могли только что пересобрать
    Hide();
    if (!Prepare()) return;
    demo_ = true;
    state_ = CURTAIN;
    AimAtCursor();
    t0_ = GetTickCount64();
    if (portal_) Portal(1);
    Push(0);
    SetTimer(wnd_, kTimer, 15, nullptr);
}

// Под конец раскрытия остатки картинки по углам тают, чтобы не было резкого обрыва.
BYTE Reveal::PortalAlpha(double t) const {
    double op = Clamp01((t - 0.16) / 0.84);
    return (BYTE)(255 * (1 - Clamp01((op - 0.82) / 0.18)));
}

void Reveal::Tick() {
    double el = (double)(LONGLONG)(GetTickCount64() - t0_);  // t0_ бывает в будущем: пауза перед угасанием
    if (state_ == INTRO) {
        // картинка проступает поверх только что показанного стола, потом раскрывается
        double d = fx::Dur(280);
        Push((BYTE)(255 * EaseOut(el / d)));
        if (el >= d) Open();
        return;
    }
    if (state_ == CURTAIN) {
        double d = fx::Dur(portal_ ? kPortalCloseMs : kCurtainMs);
        if (portal_) {
            // портал схлопывается к месту аватара: та же сцена в обратную сторону
            double t = 1 - Clamp01(el / d);
            Portal(t);
            Push(PortalAlpha(t));
        } else {
            Push((BYTE)(255 * EaseOut(el / d)));
        }
        if (el >= d) Plain();
        if (el >= d && offAfter_) {
            offAfter_ = false;
            Await();
            PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
        } else if (el >= d && demo_) {
            demo_ = false;
            state_ = FADE;
            t0_ = GetTickCount64() + 700;  // подержать картинку, как будто вошли
        } else if (el >= d) {
            KillTimer(wnd_, kTimer);
            state_ = HOLD;
            if (!LockWorkStation()) {
                state_ = FADE;  // не заблокировалось - убрать занавес
                t0_ = GetTickCount64();
                SetTimer(wnd_, kTimer, 15, nullptr);
            }
        }
    } else if (state_ == FADE) {
        double d = fx::Dur(portal_ ? kPortalMs : kFadeMs), t = (el - kFadeDelayMs) / d;
        if (t >= 1) {
            Hide();
            return;
        }
        if (portal_) {
            if (t >= 0 && !sounded_) {
                sounded_ = true;
                if (sounds_) Sound::Play(Sound::Portal);
            }
            Portal(std::max(0.0, t));
            Push(PortalAlpha(std::max(0.0, t)));
        } else {
            Push((BYTE)(255 * (1 - EaseOut(std::max(0.0, t)))));
        }
    } else {
        KillTimer(wnd_, kTimer);
    }
}

void Reveal::Hide() {
    if (wnd_) {
        KillTimer(wnd_, kTimer);
        ShowWindow(wnd_, SW_HIDE);
    }
    state_ = IDLE;
    demo_ = false;
    offAfter_ = waitWake_ = false;
    if (wnd_) {
        KillTimer(wnd_, kWakeTimer);
        KillTimer(wnd_, kInputTimer);
    }
    sounded_ = false;
    if (memDC_) {
        SelectObject(memDC_, old_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
        dib_ = nullptr;
    }
    bits_ = nullptr;
    src_.clear();
    src_.shrink_to_fit();
    for (auto* v : {&cellX_, &cellY_, &cellJit_}) std::vector<float>().swap(*v);
    std::vector<uint16_t>().swap(cell_);
    std::vector<uint8_t>().swap(edge_);
    std::vector<int8_t>().swap(tex_);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

}  // namespace lockscreen
