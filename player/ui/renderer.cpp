#include "ui/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace ui {
namespace {
void GraphicsError(const char* operation, HRESULT error) {
    // Bounded local diagnostics: no track paths or other personal data.
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    auto file = std::filesystem::path(exe).parent_path() / L"renderer.log";
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(file, ec);
    FILE* out = _wfopen(file.c_str(), !ec && bytes > 65536 ? L"wb" : L"ab");
    if (!out) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::fprintf(out, "%04u-%02u-%02u %02u:%02u:%02u %s: 0x%08lX\n", time.wYear, time.wMonth,
                 time.wDay, time.wHour, time.wMinute, time.wSecond, operation, static_cast<unsigned long>(error));
    std::fclose(out);
}
}  // namespace

bool Renderer::Init(HWND hwnd) {
    hwnd_ = hwnd;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
        return false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_ = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    height_ = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    requestedWidth_ = width_;
    requestedHeight_ = height_;
    return CreateDevice();
}

void Renderer::Shutdown() {
    ReleaseDevice();
    wic_.Reset();
    dwrite_.Reset();
    factory_.Reset();
}

void Renderer::ReleaseDevice() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    fxAttached_ = false;  // новый визуал появится уже без эффектов
    visual_.Reset();
    dcompTarget_.Reset();
    dcomp_.Reset();
    if (waitable_) {
        CloseHandle(waitable_);
        waitable_ = nullptr;
    }
    dc_.Reset();
    d2dDevice_.Reset();
    swapChain_.Reset();
    dxgiDevice_.Reset();
    d3d_.Reset();
}

bool Renderer::CreateDevice() {
    // Аппаратное ускорение; если не вышло (виртуальная машина без GPU) - программный WARP
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &d3d_, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d_,
                               nullptr, nullptr);
    if (FAILED(hr)) return false;
    if (FAILED(d3d_.As(&dxgiDevice_))) return false;

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(dxgiDevice_->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory))))
        return false;

    // Основной путь - swap chain для композиции (прозрачность поверх акрила). Если DirectComposition
    // недоступна (старые драйверы, удалённый стол, Wine), - обычный swap chain окна без прозрачности.
    composition_ = CreateCompositionChain(dxgiFactory.Get());
    if (!composition_) {
        dcompTarget_.Reset();
        visual_.Reset();
        dcomp_.Reset();
        swapChain_.Reset();
        if (!CreateWindowChain(dxgiFactory.Get())) return false;
    }

    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swapChain_.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);  // не копим кадры впрок: меньше задержка и память
        waitable_ = sc2->GetFrameLatencyWaitableObject();
    }

    if (FAILED(factory_->CreateDevice(dxgiDevice_.Get(), &d2dDevice_))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // на прозрачном фоне ClearType не годится

    ++generation_;
    return CreateTarget();
}

DXGI_SWAP_CHAIN_DESC1 Renderer::ChainDesc(bool alpha) const {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // premultiplied - прозрачность поверх системного материала
    desc.AlphaMode = alpha ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    return desc;
}

bool Renderer::CreateCompositionChain(IDXGIFactory2* dxgiFactory) {
    const DXGI_SWAP_CHAIN_DESC1 desc = ChainDesc(true);
    if (FAILED(dxgiFactory->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr, &swapChain_))) return false;
    if (FAILED(DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_))) return false;
    if (FAILED(dcomp_->CreateVisual(&visual_))) return false;
    if (FAILED(visual_->SetContent(swapChain_.Get())) || FAILED(dcompTarget_->SetRoot(visual_.Get()))) return false;
    return SUCCEEDED(dcomp_->Commit());
}

bool Renderer::CreateWindowChain(IDXGIFactory2* dxgiFactory) {
    const DXGI_SWAP_CHAIN_DESC1 desc = ChainDesc(false);
    return SUCCEEDED(dxgiFactory->CreateSwapChainForHwnd(d3d_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_));
}

bool Renderer::CreateTarget() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi_, dpi_);
    return true;
}

bool Renderer::Resize(UINT width, UINT height) {
    // WM_SIZE also arrives during minimize and nested SetWindowPos/ShowWindow calls.
    // Never resize a composition surface to 1x1 or call the driver from that callback.
    if (width && height) {
        requestedWidth_ = width;
        requestedHeight_ = height;
    }
    return true;
}

void Renderer::ResetDevice() { ReleaseDevice(); }

bool Renderer::SyncSize() {
    RECT client{};
    if (GetClientRect(hwnd_, &client) && !IsIconic(hwnd_) && client.right > 0 && client.bottom > 0)
        Resize(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom));
    if (width_ == requestedWidth_ && height_ == requestedHeight_) return true;
    width_ = requestedWidth_;
    height_ = requestedHeight_;
    if (!dc_ || !swapChain_) return true;  // CreateDevice below uses the latest dimensions.
    dc_->SetTarget(nullptr);
    target_.Reset();
    dc_->Flush();
    ComPtr<ID3D11DeviceContext> immediate;
    d3d_->GetImmediateContext(&immediate);
    immediate->ClearState();
    immediate->Flush();
    const HRESULT hr = swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN,
                                                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) {
        GraphicsError("ResizeBuffers", hr);
        ReleaseDevice();
        return true;
    }
    if (!CreateTarget()) ReleaseDevice();
    return true;
}

void Renderer::SetDpi(float dpi) {
    dpi_ = dpi;
    if (dc_) dc_->SetDpi(dpi_, dpi_);
}

ID2D1DeviceContext* Renderer::BeginFrame() {
    if (IsIconic(hwnd_)) return nullptr;
    if (fxAttached_ && pendingFx_ == Entrance::None && GetTickCount64() >= fxEnd_) FinishEntrance();
    SyncSize();
    if (!dc_ || !target_) {
        ReleaseDevice();
        if (!CreateDevice()) {
            GraphicsError("CreateDevice", E_FAIL);
            return nullptr;
        }
    }
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return dc_.Get();
}

bool Renderer::EndFrame() {
    if (!dc_ || !swapChain_) return false;
    HRESULT hr = dc_->EndDraw();
    if (SUCCEEDED(hr)) hr = swapChain_->Present(syncInterval_, 0);
    if (FAILED(hr)) {
        GraphicsError("EndDraw/Present", hr);
        // Includes invalid resources after a mode change, not only DEVICE_REMOVED.
        ReleaseDevice();
        return false;
    }
    if (pendingFx_ != Entrance::None) StartEntrance();
    return true;
}

void Renderer::PlayEntrance(Entrance fx, double seconds, std::function<double(double)> curve) {
    if (!composition_ || !dcomp_ || !visual_) return;
    if (fx == Entrance::None) {
        pendingFx_ = Entrance::None;
        if (fxAttached_) FinishEntrance();
        return;
    }
    pendingFx_ = fx;
    fxSeconds_ = std::max(0.05, seconds);
    fxCurve_ = std::move(curve);
    ComPtr<IDCompositionEffectGroup> hide;
    if (FAILED(dcomp_->CreateEffectGroup(&hide))) return;
    hide->SetOpacity(0.f);
    visual_->SetTransform(static_cast<IDCompositionTransform*>(nullptr));
    visual_->SetClip(static_cast<IDCompositionClip*>(nullptr));
    visual_->SetEffect(hide.Get());
    fxAttached_ = true;
    fxEnd_ = GetTickCount64() + 2000;  // подстраховка, если кадра так и не будет
    dcomp_->Commit();
}

// Кусочно-линейная выборка кривой: DirectComposition знает только многочлены, а пружина - нет
ComPtr<IDCompositionAnimation> Renderer::Animate(float from, float to, double seconds,
                                                 const std::function<double(double)>& curve, double delay) {
    ComPtr<IDCompositionAnimation> a;
    if (FAILED(dcomp_->CreateAnimation(&a))) return nullptr;
    if (delay > 0) a->AddCubic(0, from, 0, 0, 0);
    constexpr int kSteps = 32;
    const double step = seconds / kSteps;
    double v0 = from + (to - from) * curve(0);
    for (int i = 0; i < kSteps; ++i) {
        const double v1 = from + (to - from) * curve(double(i + 1) / kSteps);
        a->AddCubic(delay + i * step, static_cast<float>(v0), static_cast<float>((v1 - v0) / step), 0, 0);
        v0 = v1;
    }
    a->End(delay + seconds, to);
    return a;
}

void Renderer::StartEntrance() {
    const Entrance fx = pendingFx_;
    pendingFx_ = Entrance::None;
    BuildEffect(fx, false);
}

// Уход окна - то же движение в обратную сторону, с разгоном вместо пружины; начинается сразу
void Renderer::PlayExit(Entrance fx, double seconds) {
    if (!composition_ || !dcomp_ || !visual_ || fx == Entrance::None) return;
    pendingFx_ = Entrance::None;
    fxSeconds_ = std::max(0.05, seconds);
    BuildEffect(fx, true);
}

void Renderer::BuildEffect(Entrance fx, bool exit) {
    if (!dcomp_ || !visual_) return;
    const float w = static_cast<float>(width_), h = static_cast<float>(height_);
    const double T = fxSeconds_;
    const std::function<double(double)> easeOut = [](double u) { return 1 - std::pow(1 - std::clamp(u, 0.0, 1.0), 3); };
    const std::function<double(double)> easeIn = [](double u) { return std::pow(std::clamp(u, 0.0, 1.0), 2.2); };
    const auto& curve = exit || !fxCurve_ ? easeIn : fxCurve_;
    // Для ухода концы меняются местами: окно движется из обычного вида в исходное положение эффекта
    auto Anim = [&](float from, float to, double seconds, const std::function<double(double)>& c) {
        return exit ? Animate(to, from, seconds, easeIn) : Animate(from, to, seconds, c);
    };

    ComPtr<IDCompositionEffectGroup> group;
    if (FAILED(dcomp_->CreateEffectGroup(&group))) return;
    // Прозрачность короче и без перелёта: окно сначала проявляется, потом «доезжает»
    const double fade = exit ? T : std::min(T * 0.7, 0.24);
    group->SetOpacity(Anim(0, 1, fade, easeOut).Get());

    ComPtr<IDCompositionScaleTransform> scale;
    ComPtr<IDCompositionTranslateTransform> shift;
    auto scaleFrom = [&](float s) {
        if (FAILED(dcomp_->CreateScaleTransform(&scale))) return;
        scale->SetCenterX(w / 2);
        scale->SetCenterY(h / 2);
        const auto a = Anim(s, 1, T, curve);
        scale->SetScaleX(a.Get());
        scale->SetScaleY(a.Get());
    };
    auto shiftFrom = [&](float dy) {
        if (FAILED(dcomp_->CreateTranslateTransform(&shift))) return;
        shift->SetOffsetY(Anim(dy, 0, T, curve).Get());
    };
    ComPtr<IDCompositionRectangleClip> clip;
    switch (fx) {
        case Entrance::Pop: scaleFrom(0.9f); break;
        case Entrance::Rise:
            scaleFrom(0.97f);
            shiftFrom(28);
            break;
        case Entrance::Drop: shiftFrom(-22); break;
        case Entrance::Unfold: {
            // Окно откидывается от верхнего края, как крышка, с перспективой
            ComPtr<IDCompositionRotateTransform3D> rotate;
            ComPtr<IDCompositionTranslateTransform3D> toCenter, back;
            ComPtr<IDCompositionMatrixTransform3D> perspective;
            if (FAILED(dcomp_->CreateRotateTransform3D(&rotate)) || FAILED(dcomp_->CreateTranslateTransform3D(&toCenter)) ||
                FAILED(dcomp_->CreateTranslateTransform3D(&back)) || FAILED(dcomp_->CreateMatrixTransform3D(&perspective)))
                break;
            rotate->SetAxisX(1.f);
            rotate->SetAxisY(0.f);
            rotate->SetAxisZ(0.f);
            rotate->SetCenterX(w / 2);
            rotate->SetCenterY(0.f);
            rotate->SetAngle(Anim(-28, 0, T, curve).Get());
            toCenter->SetOffsetX(-w / 2);
            toCenter->SetOffsetY(-h / 2);
            back->SetOffsetX(w / 2);
            back->SetOffsetY(h / 2);
            D3DMATRIX m{};  // у MinGW SetMatrix принимает D3DMATRIX (те же поля _11.._44)
            m._11 = m._22 = m._33 = m._44 = 1;
            m._34 = -1.f / (1.6f * std::max(w, h));
            perspective->SetMatrix(m);
            IDCompositionTransform3D* parts[] = {rotate.Get(), toCenter.Get(), perspective.Get(), back.Get()};
            ComPtr<IDCompositionTransform3D> chain;
            if (SUCCEEDED(dcomp_->CreateTransform3DGroup(parts, 4, &chain))) group->SetTransform3D(chain.Get());
            break;
        }
        case Entrance::Iris: {
            // Круг из центра растёт, пока не накроет углы окна
            if (FAILED(dcomp_->CreateRectangleClip(&clip))) break;
            const float r = std::hypot(w, h) / 2 + 2;
            const auto radius = Anim(0, r, T, easeOut);
            clip->SetLeft(Anim(w / 2, w / 2 - r, T, easeOut).Get());
            clip->SetRight(Anim(w / 2, w / 2 + r, T, easeOut).Get());
            clip->SetTop(Anim(h / 2, h / 2 - r, T, easeOut).Get());
            clip->SetBottom(Anim(h / 2, h / 2 + r, T, easeOut).Get());
            clip->SetTopLeftRadiusX(radius.Get());
            clip->SetTopLeftRadiusY(radius.Get());
            clip->SetTopRightRadiusX(radius.Get());
            clip->SetTopRightRadiusY(radius.Get());
            clip->SetBottomLeftRadiusX(radius.Get());
            clip->SetBottomLeftRadiusY(radius.Get());
            clip->SetBottomRightRadiusX(radius.Get());
            clip->SetBottomRightRadiusY(radius.Get());
            scaleFrom(0.98f);
            break;
        }
        case Entrance::Curtain:
            // Шторка сверху вниз, содержимое чуть съезжает следом
            if (FAILED(dcomp_->CreateRectangleClip(&clip))) break;
            clip->SetLeft(0.f);
            clip->SetRight(w);
            clip->SetTop(0.f);
            clip->SetBottom(Anim(0, h, T, easeOut).Get());
            shiftFrom(-10);
            break;
        case Entrance::Split:
            // Полоска раскрывается из середины в стороны
            if (FAILED(dcomp_->CreateRectangleClip(&clip))) break;
            clip->SetTop(0.f);
            clip->SetBottom(h);
            clip->SetLeft(Anim(w / 2, 0, T, easeOut).Get());
            clip->SetRight(Anim(w / 2, w, T, easeOut).Get());
            break;
        default: break;
    }
    IDCompositionTransform* parts[2];
    UINT count = 0;
    if (scale) parts[count++] = scale.Get();
    if (shift) parts[count++] = shift.Get();
    ComPtr<IDCompositionTransform> transform;
    if (count == 1) transform = parts[0];
    else if (count == 2) dcomp_->CreateTransformGroup(parts, count, &transform);
    visual_->SetTransform(transform.Get());
    visual_->SetClip(clip.Get());
    visual_->SetEffect(group.Get());
    fxAttached_ = true;
    // После ухода визуал не возвращается сам: дальше окно закрывают или следующий вид играет вход
    fxEnd_ = exit ? ~0ULL : GetTickCount64() + static_cast<ULONGLONG>(std::max(T, fade) * 1000) + 50;
    dcomp_->Commit();
}

// После анимации визуал снова без эффектов: клип не должен обрезать окно, если его потом растянут
void Renderer::FinishEntrance() {
    fxAttached_ = false;
    if (!dcomp_ || !visual_) return;
    visual_->SetTransform(static_cast<IDCompositionTransform*>(nullptr));
    visual_->SetClip(static_cast<IDCompositionClip*>(nullptr));
    visual_->SetEffect(nullptr);
    dcomp_->Commit();
}

}  // namespace ui
