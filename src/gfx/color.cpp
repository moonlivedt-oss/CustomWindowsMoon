#include "gfx/color.h"

#include <algorithm>
#include <cmath>

using namespace Gdiplus;

Color ToColor(COLORREF c, BYTE a) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

Color MixColor(Color a, Color b, double t, BYTE alpha) {
    auto ch = [t](BYTE x, BYTE y) { return (BYTE)(x + (y - x) * t + 0.5); };
    return Color(alpha, ch(a.GetR(), b.GetR()), ch(a.GetG(), b.GetG()), ch(a.GetB(), b.GetB()));
}

Color HueShift(Color c, double deg) {
    if (deg == 0) return c;
    double r = c.GetR() / 255.0, g = c.GetG() / 255.0, b = c.GetB() / 255.0;
    double mx = std::max({r, g, b}), mn = std::min({r, g, b}), l = (mx + mn) / 2, d = mx - mn;
    if (d < 1e-6) return c;  // серый: оттенка нет
    double s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    double h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    h = std::fmod(h * 60 + deg + 360, 360) / 360;
    auto f = [](double p, double q, double t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    auto to = [](double v) { return (BYTE)(std::clamp(v, 0.0, 1.0) * 255 + 0.5); };
    return Color(c.GetA(), to(f(p, q, h + 1.0 / 3)), to(f(p, q, h)), to(f(p, q, h - 1.0 / 3)));
}
