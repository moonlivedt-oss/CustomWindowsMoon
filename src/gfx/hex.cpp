#include "gfx/hex.h"

#include <algorithm>
#include <cmath>

namespace hex {

namespace {
constexpr double kSqrt3 = 1.7320508075688772;
}

Cell Center(int q, int r, double s) {
    Cell c;
    c.q = q;
    c.r = r;
    c.x = s * 1.5 * q;
    c.y = s * kSqrt3 * (r + q / 2.0);
    return c;
}

Cell At(double px, double py, double s) {
    double q = (2.0 / 3 * px) / s, r = (-1.0 / 3 * px + kSqrt3 / 3 * py) / s;
    // округление кубических координат
    double x = q, z = r, y = -q - r;
    double rx = std::round(x), ry = std::round(y), rz = std::round(z);
    double dx = std::fabs(rx - x), dy = std::fabs(ry - y), dz = std::fabs(rz - z);
    if (dx > dy && dx > dz)
        rx = -ry - rz;
    else if (dy > dz)
        ry = -rx - rz;
    else
        rz = -rx - ry;
    Cell c = Center((int)rx, (int)rz, s);
    double ox = std::fabs(px - c.x), oy = std::fabs(py - c.y);
    c.m = std::max(oy, ox * kSqrt3 / 2 + oy / 2) / (s * kSqrt3 / 2);
    return c;
}

void Path(Gdiplus::GraphicsPath& p, double x, double y, double s, double inset) {
    Gdiplus::PointF pt[6];
    double r = s * inset;
    for (int i = 0; i < 6; ++i) {
        double a = 3.14159265358979323846 / 3 * i;
        pt[i] = Gdiplus::PointF((Gdiplus::REAL)(x + std::cos(a) * r), (Gdiplus::REAL)(y + std::sin(a) * r));
    }
    p.AddPolygon(pt, 6);
}

}  // namespace hex
