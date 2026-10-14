#include "gfx/draw.h"

#include <algorithm>
#include <vector>

using namespace Gdiplus;

FontFamily* Family() {
    static FontFamily* fam = nullptr;
    if (!fam) {
        fam = new FontFamily(L"Segoe UI Semibold");
        if (fam->GetLastStatus() != Ok) {
            delete fam;
            fam = new FontFamily(L"Segoe UI");
        }
    }
    return fam;
}

void BoxBlur(uint32_t* px, int w, int h, int r) {
    std::vector<uint32_t> tmp((size_t)w * h);
    auto pass = [&](const uint32_t* src, uint32_t* dst, int len, int count, int step, int lineStep) {
        for (int l = 0; l < count; ++l) {
            const uint32_t* s = src + (size_t)l * lineStep;
            uint32_t* d = dst + (size_t)l * lineStep;
            int sum[3] = {};
            auto ch = [&](int i, int c) {
                return (int)((s[(size_t)std::clamp(i, 0, len - 1) * step] >> (c * 8)) & 0xFF);
            };
            for (int i = -r; i <= r; ++i)
                for (int c = 0; c < 3; ++c) sum[c] += ch(i, c);
            for (int i = 0; i < len; ++i) {
                int n = 2 * r + 1;
                d[(size_t)i * step] = 0xFF000000u | ((sum[2] / n) << 16) | ((sum[1] / n) << 8) | (sum[0] / n);
                for (int c = 0; c < 3; ++c) sum[c] += ch(i + r + 1, c) - ch(i - r, c);
            }
        }
    };
    for (int k = 0; k < 3; ++k) {
        pass(px, tmp.data(), w, h, 1, w);  // по строкам
        pass(tmp.data(), px, h, w, w, 1);  // по столбцам
    }
}

RectF Inset(const RectF& r, float dx, float dy) {
    return RectF(r.X + dx, r.Y + dy, r.Width - 2 * dx, r.Height - 2 * dy);
}

RectF Intersect(const RectF& a, const RectF& b) {
    float x0 = std::max(a.X, b.X), y0 = std::max(a.Y, b.Y);
    float x1 = std::min(a.X + a.Width, b.X + b.Width), y1 = std::min(a.Y + a.Height, b.Y + b.Height);
    return RectF(x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0));
}

bool Inside(const RectF& in, const RectF& out) {
    return in.X >= out.X - 0.5f && in.Y >= out.Y - 0.5f && in.X + in.Width <= out.X + out.Width + 0.5f &&
           in.Y + in.Height <= out.Y + out.Height + 0.5f;
}

void Downscale(const uint32_t* src, int w, int h, uint32_t* dst, int dw, int dh, int factor) {
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            uint32_t sum[3] = {};
            int n = 0;
            for (int yy = y * factor; yy < std::min(h, (y + 1) * factor); ++yy) {
                const uint32_t* row = src + (size_t)yy * w;
                for (int xx = x * factor; xx < std::min(w, (x + 1) * factor); ++xx, ++n) {
                    uint32_t p = row[xx];
                    sum[0] += p & 0xFF;
                    sum[1] += (p >> 8) & 0xFF;
                    sum[2] += (p >> 16) & 0xFF;
                }
            }
            if (!n) n = 1;
            dst[(size_t)y * dw + x] = 0xFF000000u | ((sum[2] / n) << 16) | ((sum[1] / n) << 8) | (sum[0] / n);
        }
    }
}

void UpscaleBilinear(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh, int dstStride) {
    // фиксированная точка 16.16: на каждый пиксель только целочисленная арифметика
    const int64_t fx = ((int64_t)sw << 16) / dw, fy = ((int64_t)sh << 16) / dh;
    for (int y = 0; y < dh; ++y) {
        int64_t syf = std::max<int64_t>(0, (y * fy + fy / 2) - 0x8000);
        int y0 = std::min(sh - 1, (int)(syf >> 16)), y1 = std::min(sh - 1, y0 + 1);
        uint32_t wy = (uint32_t)(syf & 0xFFFF) >> 8;  // 0..255
        const uint32_t* r0 = src + (size_t)y0 * sw;
        const uint32_t* r1 = src + (size_t)y1 * sw;
        uint32_t* out = dst + (size_t)y * dstStride;
        for (int x = 0; x < dw; ++x) {
            int64_t sxf = std::max<int64_t>(0, (x * fx + fx / 2) - 0x8000);
            int x0 = std::min(sw - 1, (int)(sxf >> 16)), x1 = std::min(sw - 1, x0 + 1);
            uint32_t wx = (uint32_t)(sxf & 0xFFFF) >> 8;
            uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
            uint32_t res = 0xFF000000u;
            for (int sh8 = 0; sh8 < 24; sh8 += 8) {
                uint32_t ca = (a >> sh8) & 0xFF, cb = (b >> sh8) & 0xFF, cc = (c >> sh8) & 0xFF, cd = (d >> sh8) & 0xFF;
                uint32_t top = ca * (256 - wx) + cb * wx, bot = cc * (256 - wx) + cd * wx;
                res |= (((top * (256 - wy) + bot * wy) >> 16) & 0xFF) << sh8;
            }
            out[x] = res;
        }
    }
}

void RoundRectPath(GraphicsPath& p, const RectF& r, float rad) {
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
