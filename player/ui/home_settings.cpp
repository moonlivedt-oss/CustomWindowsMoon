#include "ui/home.h"
#include "ui/workspace.h"
#include <cmath>

namespace ui {
float Home::Hover(int key, bool over) {
    auto& value = hover_[key];
    const float target = over ? 1.f : 0.f;
    if (reducedMotion_) value = target;
    else value += (target - value) * (1 - std::exp(-frameDelta_ * 18));
    if (std::abs(value - target) > .005f) needsFrames_ = true;
    else value = target;
    return value;
}
void Home::PaintSettings(view::Rect content, const Input& in) {
    const auto white = D2D1::ColorF(0xf6f3f5), muted = D2D1::ColorF(0xaaa5b7);
    const auto accent = D2D1::ColorF(0xed647b), surface = D2D1::ColorF(0x191a23);
    Text(L"Студия плеера", {content.x, content.y, content.w, 38}, 28, white, 700);
    Text(L"Настраивайте слева — смотрите результат справа. Изменения сохраняются сразу.",
         {content.x, content.y + 43, content.w, 26}, 12, muted);
    const size_t firstContentHit = hits_.size();
    const wchar_t* sections[] = {L"Внешний вид", L"Звук и окно", L"Библиотека", L"Управление"};
    const float tabW = std::min(145.f, (content.w - 30) / 4);
    for (int i = 0; i < 4; ++i)
        Button({content.x + i * (tabW + 10), content.y + 84, tabW, 34}, sections[i],
               WorkspaceCommand, workspace::SettingsAppearance + i, in.settingsSection == i);
    const float top = content.y + 137;
    const float previewW = content.w >= 840 ? 300.f : 200.f;
    const view::Rect list{content.x, top, content.w - previewW - 20, content.Bottom() - top};
    const view::Rect stage{list.Right() + 20, top, previewW, list.h};
    Box(stage, D2D1::ColorF(0x211c29), 18);
    Text(L"ЖИВОЙ ПРЕДПРОСМОТР", {stage.x + 14, stage.y + 12, stage.w - 28, 20}, 9, muted, 600);
    Text(L"Демо без звука", {stage.x + 14, stage.y + 34, stage.w - 28, 20}, 11, accent);
    const float scale = std::min((stage.w - 28) / in.previewSize.w,
                                 std::max(10.f, stage.h - (in.canUndo ? 181 : 145)) / in.previewSize.h);
    view::Rect picture{stage.CenterX() - in.previewSize.w * scale / 2, stage.y + 63,
                       in.previewSize.w * scale, in.previewSize.h * scale};
    Image(in.settingsPreview, picture, 10);
    hits_.push_back({picture, WorkspaceCommand, workspace::PreviewLarge});
    const float controlsY = stage.Bottom() - 76;
    const float modeW = (stage.w - 38) / 2;
    Button({stage.x + 14, controlsY, modeW, 30}, L"Плеер", WorkspaceCommand, workspace::PreviewFull, !in.previewMini);
    Button({stage.x + 24 + modeW, controlsY, modeW, 30}, L"Мини", WorkspaceCommand, workspace::PreviewMini, in.previewMini);
    Button({stage.x + 14, stage.Bottom() - 39, stage.w - 28, 28}, L"Увеличить", WorkspaceCommand, workspace::PreviewLarge);
    if (in.canUndo)
        Button({stage.x + 14, stage.Bottom() - 112, stage.w - 28, 28}, L"Отменить изменение", WorkspaceCommand, workspace::UndoSettings);
    const int columns = std::min(2, std::max(1, (int)((list.w - 30) / 160)));
    const int themeColumns = std::min(3, std::max(1, (int)((list.w - 30) / 100)));
    auto heightOf = [&](const Card& card) {
        const int cols = card.themeGallery ? themeColumns : columns;
        return 99.f + ((card.controls.size() + cols - 1) / cols) * (card.themeGallery ? 91.f : 43.f);
    };
    float total = 0;
    for (const auto& card : in.cards) total += heightOf(card) + 14;
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, total - list.h));
    dc_->PushAxisAlignedClip(D2D1::RectF(list.x, list.y, list.Right(), list.Bottom()), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = top - scroll_;
    for (const auto& card : in.cards) {
        const float height = heightOf(card);
        if (y + height >= top && y < list.Bottom()) {
            Box({list.x, y, list.w, height}, surface, 14);
            Text(card.title, {list.x + 16, y + 14, list.w - 32, 26}, 16, white, 600);
            Text(card.subtitle, {list.x + 16, y + 44, list.w - 32, 45}, 11, muted);
            const int cols = card.themeGallery ? themeColumns : columns;
            const float bw = (list.w - 32 - (cols - 1) * 10) / cols;
            for (size_t i = 0; i < card.controls.size(); ++i) {
                const auto& control = card.controls[i];
                view::Rect r{list.x + 16 + (i % cols) * (bw + 10),
                             y + 96 + (i / cols) * (card.themeGallery ? 91.f : 43.f), bw,
                             card.themeGallery ? 81.f : 33.f};
                const bool over = r.Contains(mouse_.x, mouse_.y);
                const float hover = Hover(control.command + 20000, over);
                if (card.themeGallery) {
                    Box(r, control.background, 10);
                    const bool light = control.background.r + control.background.g + control.background.b > 1.8f;
                    const auto ink = light ? D2D1::ColorF(0x25252b) : white;
                    Box({r.x + 9, r.y + 10 - hover, 36, 36}, control.accent, 8);
                    Symbol({r.x + 18, r.y + 19 - hover, 18, 18}, IconId::Moon, light ? white : D2D1::ColorF(0x25212d));
                    Box({r.x + 53, r.y + 15, std::max(8.f, r.w - 65), 3}, control.accent, 2);
                    Box({r.x + 53, r.y + 26, std::max(8.f, r.w - 76), 2}, muted, 1);
                    Text(control.label, {r.x + 9, r.Bottom() - 27, r.w - 18, 23}, 11, ink, 600);
                    brush_->SetColor(control.active ? accent : D2D1::ColorF(1, 1, 1, hover * .35f));
                    dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + 1, r.y + 1, r.Right() - 1, r.Bottom() - 1), 10, 10), brush_.Get(), control.active ? 2.f : 1.f);
                } else if (control.toggle) {
                    Box(r, D2D1::ColorF(0x232330), 9);
                    Text(control.label, {r.x + 10, r.y + 4, r.w - 65, 25}, 11, white, 600);
                    const float on = Hover(control.command + 50000, control.active);
                    const view::Rect track{r.Right() - 47, r.y + 7, 36, 19};
                    Box(track, control.active ? accent : D2D1::ColorF(0x45424e), 10);
                    Box({track.x + 3 + on * 17, track.y + 3, 13, 13}, white, 7);
                } else {
                    Button(r, control.label, WorkspaceCommand, control.command, control.active);
                    if (r.y < list.y || r.Bottom() > list.Bottom()) hits_.pop_back();
                    continue;
                }
                if (r.y >= list.y && r.Bottom() <= list.Bottom()) hits_.push_back({r, WorkspaceCommand, control.command});
            }
        }
        y += height + 14;
    }
    dc_->PopAxisAlignedClip();
    if (total > list.h && list.h > 0) {
        const float thumb = std::max(24.f, list.h * list.h / total);
        Box({list.Right() + 6, list.y + (list.h - thumb) * scroll_ / (total - list.h), 3, thumb}, muted, 2);
    }
    if (in.previewLarge) {
        hits_.erase(hits_.begin() + firstContentHit, hits_.end());
        Box(content, D2D1::ColorF(0x101116, .98f), 16);
        Text(L"Так выглядит ваш плеер", {content.x + 20, content.y + 12, content.w - 160, 30}, 20, white, 600);
        Button({content.Right() - 120, content.y + 12, 100, 32}, L"Закрыть", WorkspaceCommand, workspace::PreviewLarge);
        const float bigScale = std::min((content.w - 40) / in.previewSize.w, (content.h - 84) / in.previewSize.h);
        Image(in.settingsPreview, {content.CenterX() - in.previewSize.w * bigScale / 2,
              content.y + 64, in.previewSize.w * bigScale, in.previewSize.h * bigScale}, 14);
    }
}
}
