#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// Цвета GDI+: из COLORREF, смешивание, прозрачность.
Gdiplus::Color ToColor(COLORREF c, BYTE a = 255);
Gdiplus::Color MixColor(Gdiplus::Color a, Gdiplus::Color b, double t, BYTE alpha = 255);
inline Gdiplus::Color WithAlpha(Gdiplus::Color c, BYTE a) { return Gdiplus::Color(a, c.GetR(), c.GetG(), c.GetB()); }
// тот же цвет с оттенком, повёрнутым на deg градусов (яркость и насыщенность те же)
Gdiplus::Color HueShift(Gdiplus::Color c, double deg);
