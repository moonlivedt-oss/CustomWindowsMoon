// Палитра команд поверх главной: затемнение, панель с полем ввода и списком действий и треков.
// Под полем лежит настоящий EDIT (его ставит App по PaletteField); выбор - стрелками или мышью.
#include "ui/home.h"
#include "ui/home_colors.h"
#include <cmath>

namespace ui {

view::Rect Home::PaletteField(view::Size window) {
    const float w = std::min(620.f, window.w - 80);
    return {window.w / 2 - w / 2 + 52, 128, w - 72, 24};
}

void Home::PaintPalette(const Input& in) {
    if (!in.paletteOpen) {
        paletteOpenedAt_ = -10;
        return;
    }
    if (paletteOpenedAt_ < 0) paletteOpenedAt_ = clock_;
    const auto& p = HomePalette();
    const float u = reducedMotion_ ? 1 : std::clamp((float)((clock_ - paletteOpenedAt_) / .22), 0.f, 1.f);
    const float e = 1 - (1 - u) * (1 - u) * (1 - u);
    if (u < 1) needsFrames_ = true;
    // Модальная: щелчки идут только в палитру, мимо панели - закрыть
    hits_.clear();
    hits_.push_back({{0, 0, in.size.w, in.size.h}, PaletteItem, -1});
    Box({0, 0, in.size.w, in.size.h}, D2D1::ColorF(0, 0, 0, .55f * e), 0);
    const view::Rect field = PaletteField(in.size);
    const int rows = std::min<int>(9, (int)in.palette.size());
    const view::Rect panel{field.x - 52, 108 - (1 - e) * 16, field.w + 72, 66.f + (rows ? rows * 48 + 14 : 46)};
    PushFade(e);
    Glow(panel, p.accent, 18, 22, .8f);
    Box(panel, MixColor(p.surface, p.card, .5f), 18);
    Path({panel.x + 20, panel.y + 22, 20, 20}, "M16 16L22 22M18 10A8 8 0 1 1 2 10A8 8 0 1 1 18 10", p.accent);
    Field({field.x, panel.y + 20, field.w, 24}, in.paletteQuery, in.paletteFocused, in.paletteStart, in.paletteEnd,
          L"Команда или название трека…", 16);
    Box({panel.x + 16, panel.y + 60, panel.w - 32, 1}, MixColor(p.card, p.white, .08f), 0);
    hits_.push_back({panel, PaletteItem, -2});  // щелчок по самой панели ничего не закрывает
    // Список прокручивается так, чтобы выбранная строка была видна
    const int first = std::clamp(in.paletteSelected - rows + 1, 0, std::max(0, (int)in.palette.size() - rows));
    for (int k = 0; k < rows; ++k) {
        const int i = first + k;
        const auto& item = in.palette[(size_t)i];
        const view::Rect row{panel.x + 10, panel.y + 70 + k * 48.f, panel.w - 20, 44};
        const bool selected = i == in.paletteSelected, over = row.Contains(mouse_.x, mouse_.y);
        const float glow = Hover(990000 + i, selected || over);
        if (glow > .01f) Box(row, MixColor(MixColor(p.card, p.accent, .22f), p.card, 1 - glow), 11);
        if (selected) Box({row.x, row.y + 10, 3, row.h - 20}, p.accent, 2);
        const bool track = item.id >= 1000;
        Path({row.x + 14, row.y + 12, 20, 20},
             track ? "M9 18V5L20 3V16M9 18A3 3 0 1 1 3 18A3 3 0 1 1 9 18M20 16A3 3 0 1 1 14 16A3 3 0 1 1 20 16"
                   : "M13 2L4 14H12L11 22L20 10H12Z",
             selected ? p.accent : p.muted);
        Text(item.title, {row.x + 46, row.y + 4, row.w - 210, 22}, 14, p.white, 600);
        Text(item.hint, {row.x + 46, row.y + 24, row.w - 210, 18}, 11, p.muted);
        if (selected) Text(L"Enter", {row.Right() - 64, row.y + 13, 54, 20}, 11, p.muted, 600);
        hits_.push_back({row, PaletteItem, i});
    }
    if (!rows)
        Text(L"Ничего не найдено - попробуйте другое слово", {panel.x + 24, panel.y + 76, panel.w - 48, 24}, 13,
             p.muted);
    PopFade();
}

}  // namespace ui
