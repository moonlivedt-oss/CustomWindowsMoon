#include "gfx/icons.h"
#include "core/perf.h"

#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <vector>

using namespace Gdiplus;

namespace {

IWICImagingFactory* Wic() {
    static IWICImagingFactory* f = nullptr;
    if (!f)
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f);
    return f;
}

// Bitmap, который сам владеет пикселями (обёртка над чужим буфером опасна после его удаления).
std::unique_ptr<Bitmap> CopyPixels(int w, int h, const BYTE* bits, int stride) {
    auto bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    BitmapData bd;
    Rect r(0, 0, w, h);
    if (bmp->LockBits(&r, ImageLockModeWrite, PixelFormat32bppPARGB, &bd) != Ok) return nullptr;
    for (int y = 0; y < h; ++y) memcpy((BYTE*)bd.Scan0 + y * bd.Stride, bits + y * stride, (size_t)w * 4);
    bmp->UnlockBits(&bd);
    return bmp;
}

std::unique_ptr<Bitmap> Fit(Bitmap& src, int px) {
    auto dst = std::make_unique<Bitmap>(px, px, PixelFormat32bppPARGB);
    Graphics g(dst.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));
    int w = src.GetWidth(), h = src.GetHeight();
    if (w <= 0 || h <= 0) return dst;
    double k = (double)px / (w > h ? w : h);
    int dw = (int)(w * k + 0.5), dh = (int)(h * k + 0.5);
    g.DrawImage(&src, Rect((px - dw) / 2, (px - dh) / 2, dw, dh), 0, 0, w, h, UnitPixel);
    return dst;
}

std::unique_ptr<Bitmap> FromShell(const std::wstring& path, int px) {
    PERF_SCOPE("icons.FromShell");
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP hbmp = nullptr;
    // крупнее, чем нужно: уменьшение даёт чище картинку, чем системный размер 1:1
    HRESULT hr = f->GetImage(SIZE{px * 2, px * 2}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
    f->Release();
    if (FAILED(hr) || !hbmp) return nullptr;

    BITMAP bm{};
    GetObjectW(hbmp, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    std::vector<BYTE> bits((size_t)w * h * 4);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // сверху вниз
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, hbmp, 0, h, bits.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(hbmp);

    bool hasAlpha = false;
    for (size_t i = 3; i < bits.size(); i += 4)
        if (bits[i]) {
            hasAlpha = true;
            break;
        }
    if (!hasAlpha)
        for (size_t i = 3; i < bits.size(); i += 4) bits[i] = 255;

    auto src = CopyPixels(w, h, bits.data(), w * 4);
    return src ? Fit(*src, px) : nullptr;
}

}  // namespace

std::unique_ptr<Bitmap> LoadImageFile(const std::wstring& path, int maxSide) {
    PERF_SCOPE("icons.LoadImageFile");
    IWICImagingFactory* wic = Wic();
    if (!wic || path.empty()) return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(
            wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return nullptr;
    std::unique_ptr<Bitmap> out;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(dec->GetFrame(0, &frame))) {
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        IWICBitmapSource* src = frame;
        if (maxSide > 0 && (int)std::max(w, h) > maxSide) {
            double k = (double)maxSide / std::max(w, h);
            UINT nw = std::max(1u, (UINT)(w * k + 0.5)), nh = std::max(1u, (UINT)(h * k + 0.5));
            if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(frame, nw, nh, WICBitmapInterpolationModeFant))) {
                src = scaler;
                w = nw;
                h = nh;
            }
        }
        if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            std::vector<BYTE> bits((size_t)w * h * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)bits.size(), bits.data())))
                out = CopyPixels(w, h, bits.data(), w * 4);
        }
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    dec->Release();
    return out;
}

Bitmap* IconCache::Get(const std::wstring& png, const std::wstring& shellPath, int px) {
    std::wstring key = std::to_wstring(px) + L"|" + png + L"|" + shellPath;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();

    std::unique_ptr<Bitmap> bmp;
    if (!png.empty()) {
        if (auto src = LoadImageFile(png, px * 2)) bmp = Fit(*src, px);
    }
    if (!bmp && !shellPath.empty()) bmp = FromShell(shellPath, px);
    Bitmap* raw = bmp.get();
    cache_[key] = std::move(bmp);  // nullptr тоже запоминаем, чтобы не дёргать диск каждый кадр
    return raw;
}
