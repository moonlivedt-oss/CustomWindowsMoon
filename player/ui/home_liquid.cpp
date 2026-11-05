// Рисование жидкого индикатора выбора; движение - в view/liquid.h. Подпись под каплей перекрашивается
// ровно по её границе: текст двух цветов, второй - в клипе по индикатору.
#include "ui/home.h"
#include "ui/home_colors.h"
#include <cmath>

namespace ui {
view::Rect Home::LiquidRect(int key, view::Rect target, int slot) {
    auto& liquid = liquids_[key];
    const auto now = liquid.Update(target, slot, clock_, paintFrame_, reducedMotion_);
    if (liquid.Animating(clock_)) needsFrames_ = true;
    return now;
}

void Home::LiquidFill(view::Rect r, D2D1_COLOR_F color, float radius, float glow) {
    if (glow > 0) Glow(r, color, radius, 8, glow);
    Box(r, color, std::min(radius, std::min(r.w, r.h) / 2));
}

void Home::LiquidLabel(const std::wstring& text, view::Rect r, view::Rect liquid, D2D1_COLOR_F normal,
                       D2D1_COLOR_F onLiquid, float size, float weight) {
    const view::Rect line{r.x, r.CenterY() - size * .7f, r.w, size * 1.5f};
    CenterText(text, line, size, normal, weight);
    const float l = std::max(r.x, liquid.x), t = std::max(r.y, liquid.y);
    const float rr = std::min(r.Right(), liquid.Right()), b = std::min(r.Bottom(), liquid.Bottom());
    if (rr <= l || b <= t) return;
    // Вторая краска - только там, где под текстом сейчас капля
    dc_->PushAxisAlignedClip(D2D1::RectF(l, t, rr, b), D2D1_ANTIALIAS_MODE_ALIASED);
    CenterText(text, line, size, onLiquid, weight);
    dc_->PopAxisAlignedClip();
}
}  // namespace ui
