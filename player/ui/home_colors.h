#pragma once
// Цвета главной. Раньше были зашиты (графит и малиновый акцент); теперь их каждый кадр задаёт App
// из темы и обложки: акцент - живой акцент плеера, фон - тёмная основа с оттенком окна темы.
// Главная всегда тёмная, светлые темы дают ей только акцент.
#include <d2d1.h>
#include <algorithm>

namespace ui {
struct HomeColors {
    D2D1_COLOR_F bg{0.063f, 0.067f, 0.086f, 1}, surface{0.082f, 0.086f, 0.114f, 1}, card{0.118f, 0.125f, 0.161f, 1};
    D2D1_COLOR_F white{0.965f, 0.953f, 0.961f, 1}, muted{0.6f, 0.6f, 0.667f, 1};
    D2D1_COLOR_F accent{0.929f, 0.392f, 0.482f, 1}, accent2{0.655f, 0.545f, 0.98f, 1};
    // Производные от акцента: подложка активного, выбранного, выделения текста, наведения на акцент
    D2D1_COLOR_F soft{}, softer{}, selection{}, accentHover{};
    D2D1_COLOR_F onAccent{1, 1, 1, 1};  // текст и значки на заливке акцентом: на светлом акценте - тёмные
};
inline D2D1_COLOR_F MixColor(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
inline HomeColors& HomePalette() {
    static HomeColors colors;
    return colors;
}
// Задать основу; производные цвета пересчитываются
inline void SetHomePalette(const HomeColors& base) {
    auto& c = HomePalette();
    c = base;
    c.soft = MixColor(c.card, c.accent, .2f);
    c.softer = MixColor(c.card, c.accent, .1f);
    c.selection = MixColor(c.bg, c.accent, .45f);
    c.accentHover = MixColor(c.accent, c.white, .2f);
    // Белый акцент (тема «Стекло») или светлый свой цвет: белая надпись на нём не читается
    const float light = .2126f * c.accent.r + .7152f * c.accent.g + .0722f * c.accent.b;
    c.onAccent = light > .62f ? D2D1_COLOR_F{.07f, .07f, .09f, 1} : D2D1_COLOR_F{1, 1, 1, 1};
    if (light > .62f) c.accentHover = MixColor(c.accent, c.bg, .12f);
}
}  // namespace ui
