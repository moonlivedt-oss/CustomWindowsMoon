// Края анимируются по отдельности: тот, что смотрит по ходу движения, убегает вперёд быстро и с
// лёгким перелётом, противоположный догоняет позже - капля вытягивается и снова собирается.
// Посреди пути она чуть сплющивается поперёк движения.
#include "view/liquid.h"

#include <cmath>
#include <utility>

namespace view {
namespace {
// Выход с перелётом (back-out): быстро к цели, чуть дальше и обратно
float BackOut(float u) {
    u = Clamp01(u);
    const float c = 1.15f, v = u - 1;  // перелёт ~6%: заметно, но не вылезает за карточку
    return 1 + (c + 1) * v * v * v + c * v * v;
}
float EaseInOut(float u) {
    u = Clamp01(u);
    return u < .5f ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3.f) / 2;
}
}  // namespace

Rect Liquid::Update(Rect target, int slot, double clock, uint64_t frame, bool reducedMotion) {
    // Группу не рисовали в прошлом кадре (карточка уходила за край, открывался другой раздел):
    // старое положение ничего не значит, тянуться от него через весь экран нельзя
    const bool absent = seenFrame_ + 1 != frame && seenFrame_ != frame;
    seenFrame_ = frame;
    if (!ready_ || reducedMotion || absent) {
        from_ = to_ = target;
        start_ = clock - kSettleSeconds;
        slot_ = slot;
        ready_ = true;
        return target;
    }
    if (slot != slot_) {
        // Новая цель посреди перетекания - стартуем с того места, где капля сейчас
        from_ = Now(clock);
        start_ = clock;
        slot_ = slot;
    } else {
        // Та же кнопка переехала: капля едет вместе с ней как одно целое
        from_ = {from_.x + target.x - to_.x, from_.y + target.y - to_.y, from_.w + target.w - to_.w,
                 from_.h + target.h - to_.h};
    }
    to_ = target;
    return Now(clock);
}

Rect Liquid::Now(double clock) const {
    const float t = (float)(clock - start_);
    const Rect& a = from_;
    const Rect& b = to_;
    const float distance = std::hypot(b.CenterX() - a.CenterX(), b.CenterY() - a.CenterY());
    // Дальше ехать - чуть дольше, но не больше полусекунды
    const float span = std::clamp(.22f + distance / 2400.f, .22f, .36f);
    const float lead = BackOut(t / span), trail = EaseInOut((t - span * .35f) / (span * 1.15f));
    auto edge = [&](float from, float to, bool forward) { return from + (to - from) * (forward ? lead : trail); };
    const bool right = b.CenterX() > a.CenterX(), down = b.CenterY() > a.CenterY();
    float left = edge(a.x, b.x, !right), rightEdge = edge(a.Right(), b.Right(), right);
    float top = edge(a.y, b.y, !down), bottom = edge(a.Bottom(), b.Bottom(), down);
    // Сплющивание поперёк движения в середине пути
    const float travel = Clamp01(t / (span * 1.5f));
    const float squash = std::sin(travel * 3.14159265f) * std::min(1.f, distance / 160.f);
    const bool horizontal = std::abs(b.CenterX() - a.CenterX()) >= std::abs(b.CenterY() - a.CenterY());
    if (horizontal) {
        const float c = (top + bottom) / 2, h = (bottom - top) * (1 - .16f * squash);
        top = c - h / 2;
        bottom = c + h / 2;
    } else {
        const float c = (left + rightEdge) / 2, w = (rightEdge - left) * (1 - .1f * squash);
        left = c - w / 2;
        rightEdge = c + w / 2;
    }
    if (rightEdge < left) std::swap(left, rightEdge);
    if (bottom < top) std::swap(top, bottom);
    return {left, top, rightEdge - left, bottom - top};
}

}  // namespace view
