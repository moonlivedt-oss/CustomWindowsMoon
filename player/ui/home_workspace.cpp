#include "ui/home.h"
#include "ui/workspace.h"
#include <d2d1helper.h>

namespace ui {
void Home::PaintWorkspace(view::Rect content, const Input& in) {
    if (model.page == view::HomeModel::Settings) { PaintSettings(content, in); return; }
    const auto white = D2D1::ColorF(0xf6f3f5), muted = D2D1::ColorF(0x9999aa);
    const auto accent = D2D1::ColorF(0xed647b), surface = D2D1::ColorF(0x191a23);
    const bool downloads = model.page == view::HomeModel::Downloads;
    const bool plugins = model.page == view::HomeModel::Plugins;
    Text(downloads ? L"Скачивание музыки" : plugins ? L"Плагины" : L"Настройки",
         {content.x, content.y, content.w, 38}, 28, white, 700);
    Text(downloads ? L"Ссылки, очередь загрузок и готовые треки в одном месте."
         : plugins ? L"Возможности плеера. Включайте только те, которыми пользуетесь."
                   : L"Оформление, воспроизведение и ваша музыкальная библиотека.",
         {content.x, content.y + 43, content.w, 26}, 12, muted);
    float listTop = content.y + 91;
    if (downloads) {
        const view::Rect field{content.x, content.y + 88, content.w - 164, 44};
        Box(field, surface, 12);
        if (in.downloadFocused) {
            brush_->SetColor(accent);
            dc_->DrawRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(field.x, field.y, field.Right(), field.Bottom()), 12, 12), brush_.Get(), 1);
        }
        const view::Rect input{field.x + 15, field.y + 10, field.w - 30, 24};
        dc_->PushAxisAlignedClip(D2D1::RectF(input.x, input.y, input.Right(), input.Bottom()), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        TextSpec spec;
        spec.size = 13;
        spec.ellipsis = false;
        const auto start = std::min<size_t>(in.downloadStart, in.downloadText.size());
        const auto end = std::min<size_t>(in.downloadEnd, in.downloadText.size());
        const float cursor = text_->Measure(in.downloadText.substr(0, end), spec);
        const float offset = in.downloadFocused ? std::max(0.f, cursor - input.w + 4) : 0;
        if (in.downloadFocused && start != end) {
            const float left = text_->Measure(in.downloadText.substr(0, start), spec);
            Box({input.x + left - offset, input.y + 2, cursor - left, 20}, D2D1::ColorF(0x713b4b), 2);
        }
        if (in.downloadText.empty()) Text(L"Вставьте ссылку на трек, альбом или плейлист", input, 13, muted);
        else {
            auto text = text_->Layout(in.downloadText, spec, std::max(input.w, cursor + 10000), input.h);
            if (text) {
                brush_->SetColor(white);
                dc_->DrawTextLayout(D2D1::Point2F(input.x - offset, input.y), text.Get(), brush_.Get());
            }
        }
        if (in.downloadFocused && start == end) Box({input.x + cursor - offset, input.y + 3, 1, 18}, accent, 0);
        dc_->PopAxisAlignedClip();
        Button({field.Right() + 12, field.y, 152, 44}, in.downloadEnabled ? L"Скачать" : L"Включить загрузчик",
               WorkspaceCommand, in.downloadEnabled ? workspace::SubmitDownload : workspace::EnableDownloader, true);
        Button({content.x, field.Bottom() + 10, 246, 28}, L"Мои «Мне нравится» в YouTube", WorkspaceCommand, workspace::LikedMusic);
        Text(L"YouTube  ·  YouTube Music  ·  Яндекс*",
             {content.x + 260, field.Bottom() + 10, content.w - 260, 26}, 10, muted);
        listTop = field.Bottom() + 54;
    }
    if (!in.notice.empty()) {
        Box({content.x, listTop, content.w, 44}, D2D1::ColorF(0x35232e), 10);
        Text(in.notice, {content.x + 14, listTop + 8, content.w - 28, 28}, 12, white);
        listTop += 58;
    }
    const float available = std::max(0.f, content.Bottom() - listTop);
    const int columns = std::min(4, std::max(1, (int)((content.w - 36) / 190)));
    auto cardHeight = [&](const Card& c) {
        const int rows = ((int)c.controls.size() + columns - 1) / columns;
        return 102.f + rows * 42 + (c.progress >= 0 ? 16 : 0);
    };
    float total = 0;
    for (const auto& c : in.cards) total += cardHeight(c) + 14;
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, total - available));
    dc_->PushAxisAlignedClip(D2D1::RectF(content.x, listTop, content.Right(), content.Bottom()),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = listTop - scroll_;
    for (const auto& c : in.cards) {
        const float height = cardHeight(c);
        if (y + height >= listTop && y < content.Bottom()) {
            Box({content.x, y, content.w, height}, surface, 14);
            Text(c.title, {content.x + 20, y + 15, content.w - 40, 27}, 16, white, 600);
            Text(c.subtitle, {content.x + 20, y + 47, content.w - 40, 43}, 12,
                 c.error ? D2D1::ColorF(0xffa6b5) : muted);
            if (!c.status.empty()) Text(c.status, {content.x + 20, y + height - 30, content.w - 40, 22}, 10, accent);
            const float bw = (content.w - 40 - (columns - 1) * 10) / columns;
            for (size_t i = 0; i < c.controls.size(); ++i) {
                const auto& control = c.controls[i];
                view::Rect r{content.x + 20 + (i % columns) * (bw + 10),
                             y + 94 + (i / columns) * 42, bw, 32};
                Button(r, control.label, WorkspaceCommand, control.command, control.active);
                // Cropped controls must never receive clicks above/below the viewport.
                if (r.y < listTop || r.Bottom() > content.Bottom()) hits_.pop_back();
            }
            if (c.progress >= 0) {
                const float py = y + height - 10;
                Box({content.x + 20, py, content.w - 40, 3}, D2D1::ColorF(0x383645), 2);
                Box({content.x + 20, py, (content.w - 40) * std::clamp(c.progress, 0.f, 1.f), 3}, accent, 2);
            }
        }
        y += height + 14;
    }
    dc_->PopAxisAlignedClip();
    if (total > available && available > 0) {
        const float thumb = std::max(28.f, available * available / total);
        Box({content.Right() + 9, listTop + (available - thumb) * scroll_ / (total - available), 3, thumb},
            D2D1::ColorF(0x5c4a5c), 2);
    }
}
}  // namespace ui
