// Пипетка и линейка: тексты цвета и замера (под тестами).
#include <cmath>
#include <cstdlib>

#include "helpers/picker.h"

namespace picker {

std::wstring Hex(BYTE r, BYTE g, BYTE b) {
    wchar_t s[16];
    swprintf(s, 16, L"#%02X%02X%02X", r, g, b);
    return s;
}

std::wstring RgbText(BYTE r, BYTE g, BYTE b) {
    return L"rgb(" + std::to_wstring(r) + L", " + std::to_wstring(g) + L", " + std::to_wstring(b) + L")";
}

std::wstring Measure(int dx, int dy) {
    int w = std::abs(dx), h = std::abs(dy);
    double len = std::sqrt((double)w * w + (double)h * h);
    wchar_t s[64];
    if (w == 0 || h == 0)
        swprintf(s, 64, L"%d px", w + h);
    else
        swprintf(s, 64, L"%d x %d, %.1f px", w, h, len);
    std::wstring t = s;
    for (wchar_t& c : t)
        if (c == L'.') c = L',';
    return t;
}

POINT Constrain(POINT from, POINT to) {
    if (std::abs(to.x - from.x) >= std::abs(to.y - from.y)) return {to.x, from.y};
    return {from.x, to.y};
}

}  // namespace picker
