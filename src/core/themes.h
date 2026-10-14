#pragma once
#include <string>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "core/config.h"
#include "gfx/color.h"

// Тема задаёт и цвета колец, и цвета студии. Палитры взяты из cpp-docs-panel, плюс свои.
struct ThemeInfo {
    const wchar_t* key;
    const wchar_t* name;
    const wchar_t* image;  // пейзаж для карточки в папке themes, пусто - своя картинка (обои, своя палитра)
    COLORREF bg, panel, fg, muted, ac, glow, gold;
};

struct RingColors {
    Gdiplus::Color base, baseSub, hotIn, hotOut, active, rim, border, text, pillBg, pillEdge, hubRing, dot, seam,
        shadow;
};

struct UiColors {
    Gdiplus::Color win, rail, card, cardHi, field, border, text, muted, faint, accent, onAccent, accent2, gold, warn,
        warnBg;
};

const std::vector<ThemeInfo>& Themes();
ThemeInfo CurrentTheme(const Config& c);  // с подставленными цветами обоев и своей палитры
RingColors RingPalette(const Config& c);
UiColors UiPalette(const Config& c);

// Плавный переход между палитрами студии и проверка, что переход закончен.
UiColors MixUi(const UiColors& a, const UiColors& b, float t);
bool SameUi(const UiColors& a, const UiColors& b);
