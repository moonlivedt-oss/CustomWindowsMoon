#include "ui/home.h"
#include "ui/home_colors.h"
#include "plugins/sdk/playermoon_plugin.h"
#include "ui/workspace.h"
#include <d2d1helper.h>

namespace ui {
void Home::PaintWorkspace(view::Rect content, const Input& in) {
    if (model.page == view::HomeModel::Settings) { PaintSettings(content, in); return; }
    const auto& palette = HomePalette();
    const auto white = palette.white, muted = palette.muted, accent = palette.accent;
    const auto surface = MixColor(palette.surface, palette.card, .35f);
    const bool downloads = model.page == view::HomeModel::Downloads;
    const bool plugins = model.page == view::HomeModel::Plugins;
    if (downloads) {
        // Баннер: луна справа, тёмная левая часть под заголовок; поперечная вуаль держит контраст
        const view::Rect banner{content.x, content.y, content.w, kDownloadFieldTop - 14};
        Glow(banner, accent, 18, 14, .3f + in.energy * .5f);
        Box(banner, D2D1::ColorF(0x14131a), 18);
        // Луна - в верхней части картинки: кадр сдвинут вверх, чтобы она не обрезалась
        const float drift = in.ambient && !reducedMotion_ ? (float)std::sin(clock_ * .11) : 0;
        const auto depth = Parallax(2, banner);
        TintedImage(DownloadsBitmap(), banner, 18, .96f + depth.x * .04f, .22f + drift * .04f + depth.y * .06f, 1.08f + drift * .03f);
        D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x101117, .92f)},
                                      {.55f, D2D1::ColorF(0x101117, .55f)},
                                      {1, D2D1::ColorF(0x101117, 0)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        collection = gradients_.Get(dc_, stops, 3);
        if (collection) dc_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(banner.x, 0), D2D1::Point2F(banner.Right(), 0)),
            collection.Get(), &gradient);
        if (gradient) dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(banner.x, banner.y, banner.Right(), banner.Bottom()), 18, 18), gradient.Get());
        if (!reducedMotion_) {
            const float sweep = (float)((clock_ - pageEnteredAt_ - .2) / 1.4);
            Sweep(banner, 18, sweep);
            if (sweep < 1) needsFrames_ = true;
        }
        Text(L"ЗАГРУЗКИ", {content.x + 26, content.y + 22, content.w - 52, 20}, 9, accent, 700);
        Text(L"Скачивание музыки", {content.x + 26, content.y + 44, content.w - 52, 40}, 30, white, 700);
        Text(L"Скопируйте ссылку - и одна кнопка сделает остальное.",
             {content.x + 26, content.y + 90, std::min(content.w - 52, 420.f), 40}, 12, D2D1::ColorF(0xc6bfcc));
    } else {
        Text(plugins ? L"Плагины" : L"Настройки", {content.x, content.y, content.w, 38}, 28, white, 700);
        if (plugins && PictureBitmap(EmptyPlugins)) {
            // Шапка «Плагинов»: стеклянные пазлы вокруг луны справа от заголовка
            const view::Rect pic{content.Right() - 104, content.y - 4, 96, 96};  // ниже кнопок окна, выше карточек
            Glow({pic.x + 26, pic.y + 26, 44, 44}, accent, 22, 14, .5f);
            TintedImage(PictureBitmap(EmptyPlugins), pic, 0, .5f, .5f, 1);
        }
        Text(plugins ? L"Возможности плеера. Включайте только те, которыми пользуетесь."
                     : L"Оформление, воспроизведение и ваша музыкальная библиотека.",
             {content.x, content.y + 43, content.w, 26}, 12, muted);
    }
    float listTop = content.y + 91;
    if (downloads) {
        const view::Rect field{content.x, content.y + kDownloadFieldTop, content.w - 164, 44};
        Box(field, surface, 12);
        if (in.downloadFocused) {
            brush_->SetColor(accent);
            dc_->DrawRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(field.x, field.y, field.Right(), field.Bottom()), 12, 12), brush_.Get(), 1);
        }
        Field({field.x + 15, field.y + 10, field.w - 30, 24}, in.downloadText, in.downloadFocused, in.downloadStart, in.downloadEnd,
              L"Ссылка на трек, альбом или плейлист - или оставьте пустым, возьму из буфера");
        Button({field.Right() + 12, field.y, 152, 44}, in.downloadText.empty() ? L"Вставить и скачать" : L"Скачать",
               WorkspaceCommand, workspace::SubmitDownload, true);
        Button({content.x, field.Bottom() + 10, 246, 28}, L"Мои «Мне нравится» в YouTube", WorkspaceCommand, workspace::LikedMusic);
        Text(L"YouTube, YouTube Music, Яндекс Музыка  ·  можно несколько ссылок сразу",
             {content.x + 260, field.Bottom() + 10, content.w - 260, 26}, 10, muted);
        listTop = field.Bottom() + 54;
    }
    if (!in.notice.empty()) {
        Box({content.x, listTop, content.w, 44}, HomePalette().soft, 10);
        Text(in.notice, {content.x + 14, listTop + 8, content.w - 28, 28}, 12, white);
        listTop += 58;
    }
    const float available = std::max(0.f, content.Bottom() - listTop);
    const int columns = std::min(4, std::max(1, (int)((content.w - 36) / 190)));
    auto cardHeight = [&](const Card& c) {
        if (c.job >= 0) return 92.f;
        if (c.illustration >= 0) return 210.f;
        const int rows = ((int)c.controls.size() + columns - 1) / columns;
        return 102.f + rows * 42 + (c.progress >= 0 ? 16 : 0);
    };
    float total = 0;
    for (const auto& c : in.cards) total += cardHeight(c) + 14;
    ClampScroll(std::max(0.f, total - available));
    dc_->PushAxisAlignedClip(D2D1::RectF(content.x, listTop, content.Right(), content.Bottom()),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = listTop - scroll_;
    size_t order = 0;
    for (const auto& c : in.cards) {
        const float height = cardHeight(c);
        const size_t index = order++;
        if (y + height >= listTop && y < content.Bottom()) {
            const float enter = Enter((int)index);
            const float y0 = y;
            y += (1 - enter) * 20;
            if (enter < 1) PushFade(enter);
            if (c.job >= 0) {
                // Загрузка только что закончилась - «+1» летит к библиотеке
                auto known = jobStates_.find(c.title);
                if (known != jobStates_.end() && known->second != PM_ROW_DONE && c.job == PM_ROW_DONE && !reducedMotion_)
                {
                    // Строка загрузки - «Исполнитель - Название»: если трек уже в коллекции, летит его обложка
                    Flyer flyer{content.x + 41, y + height / 2, clock_, {}, false};
                    for (const auto& t : model.tracks)
                        if (!t.title.empty() && c.title.find(t.title) != std::wstring::npos &&
                            (t.artist.empty() || c.title.find(t.artist) != std::wstring::npos)) {
                            flyer.track = t;
                            flyer.known = true;
                            break;
                        }
                    flyers_.push_back(std::move(flyer));
                }
                jobStates_[c.title] = c.job;
                const size_t firstHit = hits_.size();
                JobCard({content.x, y, content.w, height}, c, index);
                // Кнопки обрезанной строки не должны нажиматься за краем списка
                for (size_t h = hits_.size(); h-- > firstHit;)
                    if (hits_[h].rect.y < listTop || hits_[h].rect.Bottom() > content.Bottom()) hits_.erase(hits_.begin() + (long)h);
                if (enter < 1) PopFade();
                y = y0 + height + 14;
                continue;
            }
            Box({content.x, y, content.w, height}, surface, 14);
            // Пустая страница: иллюстрация слева, текст рядом
            const float illustrationW = c.illustration >= 0 && PictureBitmap((Picture)c.illustration) ? height - 16 : 0.f;
            if (illustrationW > 0) {
                const view::Rect pic{content.x + 12, y + 8, illustrationW, illustrationW};
                Glow({pic.x + pic.w * .2f, pic.y + pic.h * .2f, pic.w * .6f, pic.h * .6f}, accent, pic.w * .3f, 18, .5f);
                TintedImage(PictureBitmap((Picture)c.illustration), pic, 0, .5f, .5f, 1);
            }
            const float tx = content.x + 20 + illustrationW + (illustrationW > 0 ? 8 : 0), tw = content.w - 40 - illustrationW;
            const float ty = illustrationW > 0 ? y + height / 2 - 40 : y;
            Text(c.title, {tx, ty + 15, tw, 27}, 16, white, 600);
            Text(c.subtitle, {tx, ty + 47, tw, 43}, 12,
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
            if (enter < 1) PopFade();
            y = y0;
        }
        y += height + 14;
    }
    dc_->PopAxisAlignedClip();
    if (total > available && available > 0) {
        const float thumb = std::max(28.f, available * available / total);
        Box({content.Right() + 9, listTop + (available - thumb) * std::clamp(scroll_ / (total - available), 0.f, 1.f), 3, thumb},
            D2D1::ColorF(0x5c4a5c), 2);
    }
}
}  // namespace ui
