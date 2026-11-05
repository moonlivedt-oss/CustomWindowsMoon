// Перетаскивание треков на главной: из коллекции в очередь «Далее» и внутри очереди.
// Home только рисует и говорит, куда бросили; плейлист меняет App.
#include "ui/home.h"
#include "ui/home_colors.h"
#include <cmath>

namespace ui {

void Home::Press(view::Point p) {
    drag_ = {};
    sliderCommand_ = -1;
    const auto hit = At(p);
    if (hit.action != None || (hit.index >= 10000 && hit.index < 10007))
        if (Focusable(hit)) AddRipple(p, hit.rect, std::min(14.f, hit.rect.h / 2));
    if (hit.action == Slider) {
        sliderCommand_ = hit.index;
        sliderRect_ = hit.rect;
        return;
    }
    if (hit.action != PlayTrack || nowOpen_) return;  // из карточки «Дальше» треки не перетаскиваются
    if (hit.index >= 0 && hit.index < (int)displayed_.size())
        drag_.track = displayed_[(size_t)hit.index];
    else if (hit.index <= -2 && -hit.index - 2 - queueStart_ < (int)queueVisible_.size() &&
             -hit.index - 2 >= queueStart_)
        drag_.track = queueVisible_[(size_t)(-hit.index - 2 - queueStart_)];
    else
        return;
    drag_.hit = hit.index;
    drag_.start = p;
}

int Home::DropSlot() const {
    const int slot = (int)std::lround((mouse_.y - queueTop_ + 7) / 68.f);
    return queueStart_ + std::clamp(slot, 0, queueShown_);
}

bool Home::ActiveSlider(int& command, float& fraction) const {
    if (sliderCommand_ < 0) return false;
    command = sliderCommand_;
    fraction = std::clamp((mouse_.x - sliderRect_.x - 12) / std::max(1.f, sliderRect_.w - 24 - 70), 0.f, 1.f);
    return true;
}

Home::Drop Home::Release(view::Point p) {
    Drop drop;
    mouse_ = p;
    sliderCommand_ = -1;
    if (drag_.active && queueRect_.Contains(p.x, p.y)) {
        drop.kind = drag_.hit >= 0 ? Drop::ToQueue : Drop::Reorder;
        drop.index = drag_.hit >= 0 ? drag_.hit : -drag_.hit - 2;
        drop.before = DropSlot();
        bursts_.push_back({p.x, p.y, clock_});
    }
    // Брошен мимо - перенос просто отменяется; обычный щелчок обрабатывает App
    if (drag_.active && drop.kind == Drop::None) drop.index = -2;
    drag_ = {};
    return drop;
}

// Обложка летит за курсором, чуть повёрнутая, с подписью и свечением
void Home::PaintDrag(const Input&) {
    if (!drag_.active) return;
    const auto& palette = HomePalette();
    const view::Rect r{mouse_.x - 30, mouse_.y - 30, 60, 60};
    D2D1_MATRIX_3X2_F previous;
    dc_->GetTransform(&previous);
    dc_->SetTransform(D2D1::Matrix3x2F::Rotation(-5, D2D1::Point2F(r.CenterX(), r.CenterY())) * previous);
    Glow(r, palette.accent, 12, 14, 1.2f);
    Art(drag_.track, r);
    dc_->SetTransform(previous);
    TextSpec spec;
    spec.size = 12;
    spec.weight = 600;
    const float w = std::min(240.f, text_->Measure(drag_.track.title, spec) + 24);
    const view::Rect pill{mouse_.x - w / 2, r.Bottom() + 8, w, 26};
    Box(pill, MixColor(palette.card, palette.accent, .3f), 13);
    spec.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    if (auto layout = text_->Layout(drag_.track.title, spec, pill.w - 16, pill.h)) {
        brush_->SetColor(palette.white);
        dc_->DrawTextLayout(D2D1::Point2F(pill.x + 8, pill.y + 5), layout.Get(), brush_.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    needsFrames_ = true;
}

}  // namespace ui

namespace ui {
// Ползунок настроек: дорожка с заливкой акцентом, ручка и значение справа
void Home::SliderControl(view::Rect r, const Control& control) {
    const auto& p = HomePalette();
    const bool over = r.Contains(mouse_.x, mouse_.y) || sliderCommand_ == control.command;
    const float hover = Hover(control.command + 70000, over);
    Box(r, MixColor(p.card, p.white, .02f + hover * .03f), 9);
    const view::Rect track{r.x + 12, r.CenterY() - 2, r.w - 24 - 70, 4};
    const float v = std::clamp(control.value, 0.f, 1.f);
    Box(track, MixColor(p.card, p.white, .12f), 2);
    Box({track.x, track.y, track.w * v, track.h}, p.accent, 2);
    const float knob = 14 + hover * 4;
    const float kx = track.x + track.w * v;
    Glow({kx - knob / 2, track.y + 2 - knob / 2, knob, knob}, p.accent, knob / 2, 4 + hover * 6, .6f + hover);
    Box({kx - knob / 2, track.y + 2 - knob / 2, knob, knob}, p.white, knob / 2);
    TextSpec spec;
    spec.size = 12;
    spec.weight = 700;
    spec.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
    if (auto layout = text_->Layout(control.valueText, spec, 62, 20)) {
        brush_->SetColor(p.white);
        dc_->DrawTextLayout(D2D1::Point2F(r.Right() - 74, r.CenterY() - 9), layout.Get(), brush_.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    hits_.push_back({r, Slider, control.command});
}

// Однострочное поле ввода, нарисованное поверх скрытого EDIT: текст, выделение и курсор
void Home::Field(view::Rect input, const std::wstring& value, bool focused, unsigned startAt, unsigned endAt,
                 const std::wstring& placeholder, float size) {
    const auto& p = HomePalette();
    dc_->PushAxisAlignedClip(D2D1::RectF(input.x, input.y, input.Right(), input.Bottom()),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    TextSpec spec;
    spec.size = size;
    spec.ellipsis = false;
    const auto start = std::min<size_t>(startAt, value.size());
    const auto end = std::min<size_t>(endAt, value.size());
    const float cursor = text_->Measure(value.substr(0, end), spec);
    const float offset = focused ? std::max(0.f, cursor - input.w + 4) : 0;
    if (focused && start != end) {
        const float left = text_->Measure(value.substr(0, start), spec);
        Box({input.x + left - offset, input.y + 2, cursor - left, 20}, p.selection, 2);
    }
    if (value.empty())
        Text(placeholder, input, size, p.muted);
    else if (auto layout = text_->Layout(value, spec, std::max(input.w, cursor + 10000), input.h)) {
        brush_->SetColor(p.white);
        dc_->DrawTextLayout(D2D1::Point2F(input.x - offset, input.y), layout.Get(), brush_.Get());
    }
    // Курсор мигает, как у системного поля
    if (focused && start == end && (reducedMotion_ || std::fmod(clock_, 1.06) < .6))
        Box({input.x + cursor - offset, input.y + 3, 1, 18}, p.accent, 0);
    // Между сменами «виден/не виден» картинка не меняется: кадр нужен только к следующей смене
    if (focused && !reducedMotion_ && start == end) {
        const double phase = std::fmod(clock_, 1.06), wake = clock_ + (phase < .6 ? .6 - phase : 1.06 - phase);
        nextWake_ = nextWake_ > 0 ? std::min(nextWake_, wake) : wake;
    }
    dc_->PopAxisAlignedClip();
}
}  // namespace ui

namespace ui {
void Home::GhostButton(view::Rect r, const std::wstring& label, int command) {
    const auto& p = HomePalette();
    const float hover = Hover(command * 7 + 610000, r.Contains(mouse_.x, mouse_.y));
    if (hover > .01f) Box(r, D2D1::ColorF(p.white.r, p.white.g, p.white.b, .06f * hover), 10);
    brush_->SetColor(MixColor(MixColor(p.card, p.white, .16f), p.accent, hover * .7f));
    dc_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(r.x + .5f, r.y + .5f, r.Right() - .5f, r.Bottom() - .5f), 10, 10), brush_.Get(),
        1);
    CenterText(label, {r.x + 6, r.CenterY() - 10, r.w - 12, 20}, 12, p.white, 600);
    hits_.push_back({r, WorkspaceCommand, command});
}

void Home::CenterText(const std::wstring& s, view::Rect r, float size, D2D1_COLOR_F color, float weight) {
    TextSpec spec;
    spec.size = size;
    spec.weight = weight;
    spec.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    spec.wrap = r.h > size * 2;
    if (auto layout = text_->Layout(s, spec, std::max(1.f, r.w), r.h)) {
        brush_->SetColor(color);
        dc_->DrawTextLayout(D2D1::Point2F(r.x, r.y), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void Home::PushEdgeFade(view::Rect area, float top, float bottom) {
    const auto bounds = D2D1::RectF(area.x - 8, area.y, area.Right() + 8, area.Bottom());
    edgeClip_ = true;
    if (top <= 0 && bottom <= 0) {
        dc_->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        return;
    }
    // Маска прозрачности: у краёв содержимое уходит в ноль
    const float h = std::max(1.f, area.h);
    D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0, 0, 0, top > 0 ? 0.f : 1.f)},
                                  {std::clamp(top / h, 0.f, .5f), D2D1::ColorF(0, 0, 0, 1)},
                                  {1 - std::clamp(bottom / h, 0.f, .5f), D2D1::ColorF(0, 0, 0, 1)},
                                  {1, D2D1::ColorF(0, 0, 0, bottom > 0 ? 0.f : 1.f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> mask;
    collection = gradients_.Get(dc_, stops, 4);
    if (collection)
        dc_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, area.y), D2D1::Point2F(0, area.Bottom())),
            collection.Get(), &mask);
    dc_->PushLayer(D2D1::LayerParameters1(bounds, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1,
                                          mask.Get()),
                   nullptr);
    edgeClip_ = false;  // слой, не клип
}

void Home::PopEdgeFade() {
    if (edgeClip_)
        dc_->PopAxisAlignedClip();
    else
        dc_->PopLayer();
    edgeClip_ = false;
}
}  // namespace ui
