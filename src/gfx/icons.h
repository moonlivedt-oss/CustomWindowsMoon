#pragma once
#include <map>
#include <memory>
#include <string>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// Картинка любого формата, который знает WIC (PNG, JPG, WebP...), уменьшенная так,
// чтобы большая сторона была не больше maxSide (0 - без уменьшения). PARGB.
std::unique_ptr<Gdiplus::Bitmap> LoadImageFile(const std::wstring& path, int maxSide);

// Значки, заранее уменьшенные до нужного размера, чтобы кадр рисовался быстро.
class IconCache {
public:
    // png - картинка; если её нет или она не читается, берётся значок оболочки для shellPath.
    Gdiplus::Bitmap* Get(const std::wstring& png, const std::wstring& shellPath, int px);
    void Clear() { cache_.clear(); }

private:
    std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> cache_;
};
