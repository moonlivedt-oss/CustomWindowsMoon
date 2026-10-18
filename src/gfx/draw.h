#pragma once
#include <cstdint>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// Общие мелочи рисования: плавность, углы, шрифт, размытие, прямоугольники.
constexpr double kPi = 3.14159265358979323846;
inline double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline double EaseOut(double t) {
    t = 1 - Clamp01(t);
    return 1 - t * t * t;
}
// с перелётом: доходит чуть дальше цели и возвращается, как пружина
inline double EaseOutBack(double t, double over = 1.4) {
    t = Clamp01(t) - 1;
    return 1 + (over + 1) * t * t * t + over * t * t;
}
inline double Rad(double deg) { return deg * kPi / 180.0; }

Gdiplus::FontFamily* Family();                          // Segoe UI Semibold, если есть, иначе Segoe UI
void BoxBlur(uint32_t* px, int w, int h, int radius);   // три прохода - почти гауссово размытие
// Уменьшение в factor раз усреднением блоков; результат непрозрачный (alpha 255).
void Downscale(const uint32_t* src, int w, int h, uint32_t* dst, int dw, int dh, int factor);
// Билинейное растяжение непрозрачной картинки; dstStride - в пикселях.
void UpscaleBilinear(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh, int dstStride);

Gdiplus::RectF Inset(const Gdiplus::RectF& r, float dx, float dy);
// рамка шире на d со всех сторон
inline Gdiplus::RectF Grow(const Gdiplus::RectF& r, float d) {
    return Gdiplus::RectF(r.X - d, r.Y - d, r.Width + 2 * d, r.Height + 2 * d);
}
Gdiplus::RectF Intersect(const Gdiplus::RectF& a, const Gdiplus::RectF& b);
bool Inside(const Gdiplus::RectF& in, const Gdiplus::RectF& out);  // in целиком внутри out
// Скруглённый прямоугольник в путь (радиус не больше половины меньшей стороны).
void RoundRectPath(Gdiplus::GraphicsPath& p, const Gdiplus::RectF& r, float radius);
