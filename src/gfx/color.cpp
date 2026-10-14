#include "gfx/color.h"

using namespace Gdiplus;

Color ToColor(COLORREF c, BYTE a) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

Color MixColor(Color a, Color b, double t, BYTE alpha) {
    auto ch = [t](BYTE x, BYTE y) { return (BYTE)(x + (y - x) * t + 0.5); };
    return Color(alpha, ch(a.GetR(), b.GetR()), ch(a.GetG(), b.GetG()), ch(a.GetB(), b.GetB()));
}
