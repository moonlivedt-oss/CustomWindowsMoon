#include "ui/renderer.h"

#include <algorithm>
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
    if (SUCCEEDED(hr)) hr = swapChain_->Present(1, 0);
    if (FAILED(hr)) {
        GraphicsError("EndDraw/Present", hr);
        // Includes invalid resources after a mode change, not only DEVICE_REMOVED.
        ReleaseDevice();
        return false;
    }
    return true;
}

}  // namespace ui
