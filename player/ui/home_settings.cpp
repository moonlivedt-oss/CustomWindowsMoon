#include "ui/home.h"
#include "ui/home_colors.h"
#include "ui/workspace.h"
#include <cmath>

namespace ui {
float Home::Hover(int key, bool over) {
    auto& value = hover_[key];
    const float target = over ? 1.f : 0.f;
    if (reducedMotion_)
        value = target;
    else
        value += (target - value) * (1 - std::exp(-frameDelta_ * 18));
    if (std::abs(value - target) > .005f)
        needsFrames_ = true;
    else
        value = target;
    return value;
}
void Home::PaintSettings(view::Rect content, const Input& in) {
    const auto& palette = HomePalette();
    const auto white = palette.white, muted = palette.muted, accent = palette.accent;
    const auto surface = MixColor(palette.surface, palette.card, .35f);
    const bool searching = !in.settingsQuery.empty();
    if (auto* console = PictureBitmap(BannerSettings))
        PageBanner({content.x - 10, content.y - 6, content.w - 290, 78}, console, 3, 14);
    Text(L"Студия плеера", {content.x, content.y, content.w - 300, 38}, 28, white, 700);
    Text(searching ? L"Результаты поиска по всем разделам. Esc - очистить."
                   : L"Настраивайте слева - смотрите результат справа. Изменения сохраняются сразу.",
         {content.x, content.y + 43, content.w - 300, 26}, 12, muted);
    // Поиск по настройкам: под нарисованным полем лежит настоящий EDIT
    settingsSearch_ = {content.Right() - 280, content.y + 4, 280, 40};
    Box(settingsSearch_, palette.card, 12);
    if (in.settingsSearchFocused) {
        brush_->SetColor(accent);
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(settingsSearch_.x, settingsSearch_.y,
                                                                settingsSearch_.Right(), settingsSearch_.Bottom()),
                                                    12, 12),
                                  brush_.Get(), 1);
    }
    Path({settingsSearch_.x + 13, settingsSearch_.y + 12, 16, 16}, "M16 16L22 22M18 10A8 8 0 1 1 2 10A8 8 0 1 1 18 10",
         muted);
    Field({settingsSearch_.x + 38, settingsSearch_.y + 9, settingsSearch_.w - 50, 24}, in.settingsQuery,
          in.settingsSearchFocused, in.settingsStart, in.settingsEnd, L"Найти настройку");
    const size_t firstContentHit = hits_.size();
    const wchar_t* sections[] = {L"Вид", L"Движение", L"Звук", L"Библиотека", L"Общее"};
    // Справа в том же ряду - «Сбросить раздел» (168): вкладки не должны на него наезжать в узком окне
    const float tabW = std::min(124.f, (content.w - 168 - 16 - 4 * 8) / 5);
    // Вкладки: подложка перетекает к выбранной, подпись перекрашивается по её краю
    view::Rect tabs[5];
    for (int i = 0; i < 5; ++i) {
        tabs[i] = {content.x + i * (tabW + 8), content.y + 84, tabW, 34};
        const float over = Hover(workspace::SettingsTabFirst + i + 40000, tabs[i].Contains(mouse_.x, mouse_.y));
        Box(tabs[i], MixColor(palette.card, white, over * .05f), 9);
        hits_.push_back({tabs[i], WorkspaceCommand, workspace::SettingsTabFirst + i, sections[i]});
    }
    view::Rect tabLiquid{};
    if (!searching) {
        const int section = std::clamp(in.settingsSection, 0, 4);
        tabLiquid = LiquidRect(100, tabs[section], section);
        LiquidFill(tabLiquid, accent, 9, .5f);
    }
    for (int i = 0; i < 5; ++i)
        LiquidLabel(sections[i], tabs[i], tabLiquid, searching ? muted : white, palette.onAccent, 13, 600);
    if (!searching) {
        // Сброс - не вкладка: тихая обведённая кнопка в конце ряда
        GhostButton({content.Right() - 168, content.y + 84, 168, 34}, L"↺  Сбросить раздел", workspace::ResetSection);
    }
    const float top = content.y + 137;
    const float previewW = content.w >= 840 ? 300.f : 220.f;
    const view::Rect list{content.x, top, content.w - previewW - 24, content.Bottom() - top};

    // Образец: высота по содержимому, кнопки сразу под картинкой
    const float controlsH = 32 + 12 + (in.canUndo ? 32 + 10 : 0) + 28 + 14;
    const float scale =
        std::min((previewW - 28) / in.previewSize.w, std::max(10.f, list.h - 63 - controlsH - 14) / in.previewSize.h);
    const float stageX = list.Right() + 24;
    const view::Rect picture{stageX + previewW / 2 - in.previewSize.w * scale / 2, top + 63, in.previewSize.w * scale,
                             in.previewSize.h * scale};
    const view::Rect stage{stageX, top, previewW, 63 + picture.h + 14 + controlsH};
    Box(stage, palette.surface, 18);
    brush_->SetColor(MixColor(palette.surface, white, .06f));
    dc_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(stage.x + .5f, stage.y + .5f, stage.Right() - .5f, stage.Bottom() - .5f), 18, 18),
        brush_.Get(), 1);
    Text(L"ЖИВОЙ ПРЕДПРОСМОТР", {stage.x + 16, stage.y + 14, stage.w - 32, 20}, 9, muted, 600);
    Text(L"Демо без звука", {stage.x + 16, stage.y + 34, stage.w - 32, 20}, 11, accent);
    Glow(picture, accent, 10, 10, .35f);
    Image(in.settingsPreview, picture, 10);
    hits_.push_back({picture, WorkspaceCommand, workspace::PreviewLarge});
    float cy = picture.Bottom() + 14;
    // «Плеер / Мини» - одна дорожка с бегущей подложкой
    const view::Rect segment{stage.x + 14, cy, stage.w - 28, 32};
    Box(segment, palette.card, 10);
    const float half = segment.w / 2;
    const view::Rect modeSlot{segment.x + 3 + (in.previewMini ? half - 3 : 0), segment.y + 3, half - 3, segment.h - 6};
    const auto modeLiquid = LiquidRect(102, modeSlot, in.previewMini ? 1 : 0);
    LiquidFill(modeLiquid, accent, 8, .35f);
    const wchar_t* modes[] = {L"Плеер", L"Мини"};
    for (int i = 0; i < 2; ++i) {
        const view::Rect r{segment.x + i * half, segment.y, half, segment.h};
        LiquidLabel(modes[i], r, modeLiquid, white, palette.onAccent, 12, 600);
        hits_.push_back({r, WorkspaceCommand, i ? workspace::PreviewMini : workspace::PreviewFull});
    }
    cy += 32 + 12;
    if (in.canUndo) {
        const float w2 = (stage.w - 38) / 2;
        if (in.undoCount > 1) {
            GhostButton({stage.x + 14, cy, w2, 32}, L"Отменить", workspace::UndoSettings);
            GhostButton({stage.x + 24 + w2, cy, w2, 32}, L"Отменить всё · " + std::to_wstring(in.undoCount),
                        workspace::UndoAll);
        } else {
            GhostButton({stage.x + 14, cy, stage.w - 28, 32}, L"Отменить изменение", workspace::UndoSettings);
        }
        cy += 42;
    }
    const view::Rect zoom{stage.x + 14, cy, stage.w - 28, 28};
    const bool zoomOver = zoom.Contains(mouse_.x, mouse_.y);
    Path({zoom.CenterX() - 52, zoom.y + 6, 16, 16}, "M14 4h6v6M20 4l-6.5 6.5M10 20H4v-6M4 20l6.5-6.5",
         zoomOver ? white : muted);
    Text(L"Увеличить", {zoom.CenterX() - 30, zoom.y + 5, 100, 20}, 12, zoomOver ? white : muted, 600);
    hits_.push_back({zoom, WorkspaceCommand, workspace::PreviewLarge});

    // Карточки: заголовок и пояснение, ниже - элементы ровными рядами
    const float inner = list.w - 32;
    auto balanced = [&](size_t n, float minWidth) {
        if (n == 0) return 1;
        const int fit = std::max(1, (int)((inner + 10) / (minWidth + 10)));
        const int rows = ((int)n + fit - 1) / fit;
        return ((int)n + rows - 1) / rows;  // ряды почти поровну: 3 кнопки - в один ряд, 7 - как 4 и 3
    };
    auto isToggle = [](const Card& c) { return c.controls.size() == 1 && c.controls[0].toggle; };
    auto isSlider = [](const Card& c) { return c.controls.size() == 1 && c.controls[0].value >= 0; };
    auto colsOf = [&](const Card& c) {
        if (c.swatches) return (int)std::max<size_t>(1, c.controls.size());
        if (c.themeGallery) return balanced(c.controls.size(), 170.f);
        // Ширина колонки - по самой длинной подписи: длинные названия не обрезаются многоточием
        TextSpec spec;
        spec.size = 13;
        spec.weight = 600;
        float widest = 130;
        for (const auto& control : c.controls) widest = std::max(widest, text_->Measure(control.label, spec) + 36);
        return balanced(c.controls.size(), std::min(widest, inner));
    };
    auto rowHeight = [&](const Card& c) {
        return isSlider(c) ? 40.f : c.themeGallery ? 91.f : c.swatches ? 84.f : 42.f;
    };
    auto heightOf = [&](const Card& c) {
        if (isToggle(c)) return 78.f;
        const int cols = colsOf(c);
        return 82.f + ((int)c.controls.size() + cols - 1) / cols * rowHeight(c) + (c.controls.empty() ? 0.f : 6.f);
    };
    hoveredCommand_ = -1;
    float total = 0;
    for (const auto& card : in.cards) total += heightOf(card) + 12;
    ClampScroll(std::max(0.f, total - list.h));
    PushEdgeFade(list, scroll_ > 1 ? 22.f : 0.f, total - scroll_ > list.h + 1 ? 28.f : 0.f);
    float y = top - scroll_;
    int order = 0;
    auto visible = [&](const view::Rect& r) { return r.y >= list.y && r.Bottom() <= list.Bottom(); };
    for (const auto& card : in.cards) {
        const float height = heightOf(card);
        if (y + height >= top && y < list.Bottom()) {
            const float enter = Enter(order++);
            if (enter < 1) PushFade(enter);
            const view::Rect box{list.x, y, list.w, height};
            if (searching) Glow(box, accent, 14, 6, .4f);
            if (isToggle(card)) {
                // Переключатель: вся строка - кнопка, сам переключатель справа
                const auto& control = card.controls[0];
                const bool over = box.Contains(mouse_.x, mouse_.y);
                const float hover = Hover(control.command + 20000, over);
                Box(box, MixColor(surface, white, hover * .025f), 14);
                Spotlight(box, 14, hover);
                Text(card.title, {box.x + 18, y + 14, box.w - 120, 26}, 15, white, 600);
                Text(card.subtitle, {box.x + 18, y + 42, box.w - 120, 24}, 11, muted);
                const float on = Hover(control.command + 50000, control.active);
                const view::Rect track{box.Right() - 64, box.CenterY() - 12, 46, 24};
                if (control.active) Glow(track, accent, 12, 6, .5f);
                Box(track, MixColor(MixColor(palette.card, white, .14f), accent, on), 12);
                Box({track.x + 3 + on * 22, track.y + 3, 18, 18}, white, 9);
                if (visible(box)) hits_.push_back({box, WorkspaceCommand, control.command});
                if (over && visible(box)) hoveredCommand_ = control.command;
            } else {
                Box(box, surface, 14);
                Spotlight(box, 14, 1);
                Text(card.title, {box.x + 18, y + 14, box.w - 36, 26}, 15, white, 600);
                Text(card.subtitle, {box.x + 18, y + 42, box.w - 36, 34}, 11, muted);
                const int cols = colsOf(card);
                const float bw = (inner - (cols - 1) * 10.f) / cols;
                const float rowH = rowHeight(card);
                // Ряд вариантов с одним выбранным - выбор перетекает жидкой подложкой
                int chosen = -1, actives = 0;
                for (size_t i = 0; i < card.controls.size(); ++i)
                    if (card.controls[i].active && !card.controls[i].toggle && card.controls[i].value < 0) {
                        chosen = (int)i;
                        ++actives;
                    }
                const bool liquid = actives == 1 && !card.swatches && !card.themeGallery && !isSlider(card);
                std::vector<view::Rect> cells;
                for (size_t i = 0; i < card.controls.size(); ++i) {
                    const auto& control = card.controls[i];
                    const view::Rect r{box.x + 16 + (i % cols) * (bw + 10), y + 78 + (i / cols) * rowH, bw, rowH - 10};
                    const bool over = r.Contains(mouse_.x, mouse_.y);
                    const float hover = Hover(control.command + 20000, over);
                    if (over && visible(r)) hoveredCommand_ = control.command;
                    if (control.value >= 0) {
                        SliderControl(r, control);
                        if (!visible(r)) hits_.pop_back();
                        continue;
                    }
                    if (card.swatches) {
                        // Цвет - кружок с подписью; выбранный - в кольце со свечением
                        const float d = 36 + hover * 4;
                        const view::Rect dot{r.CenterX() - d / 2, r.y + 4 + (40 - d) / 2, d, d};
                        if (control.active) Glow(dot, control.accent, d / 2, 8, 1.2f);
                        Box(dot, control.accent, d / 2);
                        if (control.active) {
                            brush_->SetColor(white);
                            dc_->DrawEllipse(
                                D2D1::Ellipse(D2D1::Point2F(dot.CenterX(), dot.CenterY()), d / 2 + 4, d / 2 + 4),
                                brush_.Get(), 2);
                        }
                        if (control.command == workspace::CustomAccent)
                            Path({dot.CenterX() - 8, dot.CenterY() - 8, 16, 16}, "M12 5V19M5 12H19", palette.onAccent);
                        CenterText(control.label, {r.x, r.y + 50, r.w, 20}, 11, control.active ? white : muted,
                                   control.active ? 700.f : 500.f);
                    } else if (card.themeGallery) {
                        Box(r, control.background, 10);
                        const bool light = control.background.r + control.background.g + control.background.b > 1.8f;
                        const auto ink = light ? D2D1::ColorF(0x25252b) : white;
                        Box({r.x + 10, r.y + 10 - hover, 36, 36}, control.accent, 9);
                        Symbol({r.x + 19, r.y + 19 - hover, 18, 18}, IconId::Moon,
                               light ? white : D2D1::ColorF(0x25212d));
                        Box({r.x + 56, r.y + 16, std::max(8.f, r.w - 70), 3}, control.accent, 2);
                        Box({r.x + 56, r.y + 27, std::max(8.f, r.w - 92), 2}, light ? D2D1::ColorF(0x9a98a8) : muted,
                            1);
                        Text(control.label, {r.x + 10, r.Bottom() - 27, r.w - 20, 23}, 11, ink, 600);
                        if (control.active) Glow(r, accent, 10, 6, .6f);
                        brush_->SetColor(control.active ? accent : D2D1::ColorF(1, 1, 1, hover * .35f));
                        dc_->DrawRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(r.x + 1, r.y + 1, r.Right() - 1, r.Bottom() - 1), 10, 10),
                            brush_.Get(), control.active ? 2.f : 1.f);
                    } else if (liquid) {
                        // Подложка ячейки; подпись - после капли, чтобы перекраситься по её краю
                        Box(r, MixColor(palette.card, white, hover * .05f), 9);
                        cells.push_back(r);
                    } else {
                        Button(r, control.label, WorkspaceCommand, control.command, control.active);
                        if (!visible(r)) hits_.pop_back();
                        continue;
                    }
                    if (visible(r)) hits_.push_back({r, WorkspaceCommand, control.command, control.label});
                }
                if (liquid && chosen >= 0 && (size_t)chosen < cells.size()) {
                    const int key = 200 + (int)(std::hash<std::wstring>{}(card.title) % 100000);
                    const auto drop = LiquidRect(key, cells[(size_t)chosen], chosen);
                    LiquidFill(drop, accent, 9, .45f);
                    for (size_t i = 0; i < cells.size(); ++i)
                        LiquidLabel(card.controls[i].label, cells[i], drop, white, palette.onAccent, 13, 600);
                }
            }
            if (enter < 1) PopFade();
        }
        y += height + 12;
    }
    PopEdgeFade();
    if (total > list.h && list.h > 0) {
        const float thumb = std::max(24.f, list.h * list.h / total);
        Box({list.Right() + 8, list.y + (list.h - thumb) * std::clamp(scroll_ / (total - list.h), 0.f, 1.f), 3, thumb},
            muted, 2);
    }
    if (in.previewLarge) {
        hits_.erase(hits_.begin() + firstContentHit, hits_.end());
        Box(content, D2D1::ColorF(0x101116, .98f), 16);
        Text(L"Так выглядит ваш плеер", {content.x + 20, content.y + 12, content.w - 160, 30}, 20, white, 600);
        Button({content.Right() - 120, content.y + 12, 100, 32}, L"Закрыть", WorkspaceCommand, workspace::PreviewLarge);
        const float bigScale = std::min((content.w - 40) / in.previewSize.w, (content.h - 84) / in.previewSize.h);
        Image(in.settingsPreview,
              {content.CenterX() - in.previewSize.w * bigScale / 2, content.y + 64, in.previewSize.w * bigScale,
               in.previewSize.h * bigScale},
              14);
    }
}
}  // namespace ui
