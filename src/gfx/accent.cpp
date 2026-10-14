#include "gfx/accent.h"
#include "core/perf.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "gfx/icons.h"

using namespace Gdiplus;

namespace {

void ToHsv(double r, double g, double b, double& h, double& s, double& v) {
    double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0)
        h = 0;
    else if (mx == r)
        h = 60 * std::fmod((g - b) / d + 6, 6.0);
    else if (mx == g)
        h = 60 * ((b - r) / d + 2);
    else
        h = 60 * ((r - g) / d + 4);
}

COLORREF FromHsv(double h, double s, double v) {
    double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2.0) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    int seg = (int)(h / 60) % 6;
    switch (seg) {
        case 0:
            r = c, g = x;
            break;
        case 1:
            r = x, g = c;
            break;
        case 2:
            g = c, b = x;
            break;
        case 3:
            g = x, b = c;
            break;
        case 4:
            r = x, b = c;
            break;
        default:
            r = c, b = x;
            break;
    }
    auto ch = [m](double t) { return (BYTE)std::clamp((t + m) * 255 + 0.5, 0.0, 255.0); };
    return RGB(ch(r), ch(g), ch(b));
}

}  // namespace

bool WallpaperAccent(COLORREF& out) {
    PERF_SCOPE("fx.WallpaperAccent");
    wchar_t path[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) || !path[0]) return false;
    // те же обои (путь и время изменения) - тот же цвет, картинку заново не читаем
    static std::wstring lastKey;
    static bool lastOk = false;
    static COLORREF lastColor = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(path, GetFileExInfoStandard, &fa);
    std::wstring key = std::wstring(path) + L"|" + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime) + L"|" +
                       std::to_wstring(fa.ftLastWriteTime.dwHighDateTime);
    if (key == lastKey) {
        out = lastColor;
        return lastOk;
    }
    lastKey = key;
    lastOk = false;
    auto bmp = LoadImageFile(path, 96);
    if (!bmp) return false;

    // гистограмма оттенков, где вес - насыщенность и яркость: серые и тёмные пиксели почти не голосуют
    constexpr int kBins = 36;
    double weight[kBins] = {}, sr[kBins] = {}, sg[kBins] = {}, sb[kBins] = {};
    int w = bmp->GetWidth(), h = bmp->GetHeight();
    BitmapData bd;
    Rect rc(0, 0, w, h);
    if (bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return false;
    double total = 0;
    for (int y = 0; y < h; ++y) {
        const BYTE* row = (const BYTE*)bd.Scan0 + y * bd.Stride;
        for (int x = 0; x < w; ++x) {
            double b = row[x * 4] / 255.0, g = row[x * 4 + 1] / 255.0, r = row[x * 4 + 2] / 255.0;
            double hu, s, v;
            ToHsv(r, g, b, hu, s, v);
            if (s < 0.4 || v < 0.18) continue;  // небо и туман не в счёт, нужен яркий цвет
            double wt = s * s * s * v;
            int bin = std::min(kBins - 1, (int)(hu / (360.0 / kBins)));
            weight[bin] += wt;
            sr[bin] += r * wt, sg[bin] += g * wt, sb[bin] += b * wt;
            total += wt;
        }
    }
    bmp->UnlockBits(&bd);
    if (total < w * h * 0.005) return false;

    // соседние корзины складываем, чтобы цвет на границе двух корзин не проигрывал
    int best = 0;
    double bestW = -1;
    for (int i = 0; i < kBins; ++i) {
        double s = weight[i] + 0.5 * (weight[(i + 1) % kBins] + weight[(i + kBins - 1) % kBins]);
        if (s > bestW) bestW = s, best = i;
    }
    double r = 0, g = 0, b = 0, wsum = 0;
    for (int d = -1; d <= 1; ++d) {
        int i = (best + d + kBins) % kBins;
        r += sr[i], g += sg[i], b += sb[i], wsum += weight[i];
    }
    double hu, s, v;
    ToHsv(r / wsum, g / wsum, b / wsum, hu, s, v);
    out = FromHsv(hu, std::max(s, 0.68), std::clamp(v, 0.72, 0.88));  // читаемый акцент на тёмном фоне
    lastColor = out;
    lastOk = true;
    return true;
}
