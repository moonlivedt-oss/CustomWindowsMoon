// Эффекты главной: живой фон, свечение, эквалайзер, пластинка загрузок, блик и вход страницы.
// Всё рисуется обычным Direct2D без промежуточных картинок: градиенты и полупрозрачные слои.
// Движение - только при ambient (музыка играет, окно впереди) и без «Меньше движения».
#include "ui/home.h"
#include "ui/home_colors.h"
#include "ui/workspace.h"
#include "plugins/sdk/playermoon_plugin.h"
#include <d2d1helper.h>
#include <cmath>
#include <d2d1effects.h>

namespace ui {
namespace {
const D2D1_COLOR_F &kWhite = HomePalette().white, &kMuted = HomePalette().muted, &kAccent = HomePalette().accent;
const D2D1_COLOR_F& kLavender = HomePalette().accent2;
constexpr float kPi = 3.14159265f;
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return {c.r, c.g, c.b, c.a * a}; }
float EaseOut(float u) { return 1 - std::pow(1 - std::clamp(u, 0.f, 1.f), 3.f); }
}  // namespace

float Home::Enter(int order) {
    if (reducedMotion_) return 1;
    const float u = (float)((clock_ - pageEnteredAt_ - std::min(order, 14) * .035) / .42);
    if (u < 1) needsFrames_ = true;
    return EaseOut(u);
}

void Home::PushFade(float opacity) {
    dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), opacity),
                   nullptr);
}
void Home::PopFade() { dc_->PopLayer(); }

// Три мягких пятна цвета медленно плывут по фону; низкие частоты слегка раздувают их и делают ярче
void Home::Aurora(view::Rect area, const Input& in) {
    if (in.highContrast) return;  // в высоком контрасте фон ровный
    const float t = in.ambient && !reducedMotion_ ? (float)clock_ : 0;
    const float e = in.ambient ? std::min(1.f, in.energy + in.beat * .5f) : 0;
    struct Blob {
        D2D1_COLOR_F color;
        float x, y, r, speed, phase, alpha;
    };
    // В «Сейчас играет» все пятна из цвета обложки, иначе чужой оттенок спорит с ней
    const D2D1_COLOR_F second = auroraMood_ ? HomePalette().accent2 : kLavender;
    const D2D1_COLOR_F third = auroraMood_ ? MixColor(kAccent, HomePalette().white, .2f) : D2D1::ColorF(0x57c9c1);
    const Blob blobs[] = {{kAccent, .80f, .12f, .62f, .050f, 0.f, .28f},
                          {second, .30f, .90f, .62f, .037f, 2.1f, .20f},
                          {third, .58f, .62f, .42f, .029f, 4.2f, .08f}};
    for (const auto& b : blobs) {
        const float cx = area.x + area.w * (b.x + .10f * std::sin(t * b.speed * 2 * kPi + b.phase));
        const float cy = area.y + area.h * (b.y + .12f * std::cos(t * b.speed * 1.6f * kPi + b.phase));
        const float r = std::max(area.w, area.h) * b.r * (1 + e * .10f);
        // Яркость - прозрачностью кисти: бас меняет её каждый кадр, а коллекция цветов остаётся одна
        D2D1_GRADIENT_STOP stops[] = {{0, Alpha(b.color, 1)}, {.45f, Alpha(b.color, .35f)}, {1, Alpha(b.color, 0)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1RadialGradientBrush> brush;
        collection = gradients_.Get(dc_, stops, 3);
        if (!collection) continue;
        dc_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), r, r * .72f),
            D2D1::BrushProperties(b.alpha + e * .10f), collection.Get(), &brush);
        if (brush) dc_->FillRectangle(D2D1::RectF(area.x, area.y, area.Right(), area.Bottom()), brush.Get());
    }
    if (in.ambient && !reducedMotion_) needsFrames_ = true;
}

// Мягкое свечение без размытия: кольца с убывающей прозрачностью
void Home::Glow(view::Rect r, D2D1_COLOR_F color, float radius, float spread, float strength) {
    constexpr int kRings = 7;
    for (int i = kRings; i >= 1; --i) {
        const float k = (float)i / kRings, grow = spread * k;
        brush_->SetColor(Alpha(color, .055f * strength * (1 - k * .8f)));
        dc_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.x - grow, r.y - grow, r.Right() + grow, r.Bottom() + grow), radius + grow,
                              radius + grow),
            brush_.Get());
    }
}

// Четыре полоски в такт: из полос спектра, а без звука - тихое дыхание
void Home::Equalizer(view::Rect r, const Input& in, D2D1_COLOR_F color) {
    const float gap = r.w * .12f, bar = (r.w - gap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        float level = in.bandsLive ? std::clamp(in.bands[i * 2] * 1.25f + .12f, .15f, 1.f)
                                 : .25f + .2f * std::sin((float)clock_ * 3 + i * 1.7f) * (reducedMotion_ ? 0 : 1);
        if (!in.playback.playing) level = .22f;
        const float h = r.h * level;
        brush_->SetColor(color);
        dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + i * (bar + gap), r.Bottom() - h,
                                                                r.x + i * (bar + gap) + bar, r.Bottom()),
                                                    bar / 2, bar / 2),
                                  brush_.Get());
    }
    if (in.playback.playing && !reducedMotion_) needsFrames_ = true;
}

// Пластинка загрузки: крутится, пока идёт работа; готово - отметка, ошибка - восклицательный знак
void Home::Disc(view::Rect r, int state, bool tools) {
    const float cx = r.CenterX(), cy = r.CenterY(), rad = r.w / 2;
    const bool active = state == PM_ROW_ACTIVE, done = state == PM_ROW_DONE, error = state == PM_ROW_ERROR;
    if (active) Glow({cx - rad, cy - rad, rad * 2, rad * 2}, kAccent, rad, 10, 1.3f);
    brush_->SetColor(D2D1::ColorF(0x0c0c11));
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), brush_.Get());
    brush_->SetColor(D2D1::ColorF(0x2c2b38));
    for (float k : {.92f, .78f, .64f})
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad * k, rad * k), brush_.Get(), .8f);
    // Блик на дорожках показывает вращение
    const float angle = active && !reducedMotion_ ? (float)clock_ * 2.4f : -.8f;
    if (active && !reducedMotion_) needsFrames_ = true;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, .22f));
    for (float side : {0.f, kPi}) {
        const float a = angle + side;
        dc_->DrawLine(D2D1::Point2F(cx + std::cos(a) * rad * .42f, cy + std::sin(a) * rad * .42f),
                      D2D1::Point2F(cx + std::cos(a) * rad * .9f, cy + std::sin(a) * rad * .9f), brush_.Get(), 1.4f);
    }
    const auto label = error                     ? D2D1::ColorF(0xd9536b)
                       : done                    ? kAccent
                       : state == PM_ROW_WAITING ? D2D1::ColorF(0x4a4458)
                                                 : kAccent;
    brush_->SetColor(label);
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad * .42f, rad * .42f), brush_.Get());
    const view::Rect mark{cx - rad * .3f, cy - rad * .3f, rad * .6f, rad * .6f};
    if (tools && !done && !error)
        Symbol(mark, IconId::Settings, kWhite);
    else if (done)
        Path(mark, "M5 12.5L10 17L19 7", kWhite);
    else if (error)
        Path(mark, "M12 5V14M12 18.5V19", kWhite);
    else {
        brush_->SetColor(D2D1::ColorF(0x0c0c11));
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad * .08f, rad * .08f), brush_.Get());
    }
}

// Блик по картинке: светлая полоса проходит слева направо один раз (progress 0..1)
void Home::Sweep(view::Rect r, float radius, float progress) {
    if (progress <= 0 || progress >= 1) return;
    const float x = r.x - r.w * .3f + (r.w * 1.6f) * EaseOut(progress);
    D2D1_GRADIENT_STOP stops[] = {
        {0, D2D1::ColorF(1, 1, 1, 0)}, {.5f, D2D1::ColorF(1, .92f, .95f, .13f)}, {1, D2D1::ColorF(1, 1, 1, 0)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> brush;
    collection = gradients_.Get(dc_, stops, 3);
    if (!collection) return;
    dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x - r.w * .14f, r.y),
                                                                       D2D1::Point2F(x + r.w * .14f, r.Bottom())),
                                   collection.Get(), &brush);
    if (brush)
        dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), radius, radius),
                                  brush.Get());
}

// Строка загрузки: пластинка, название, состояние пилюлей, маленькие кнопки справа, полоса с бегущим бликом
void Home::JobCard(view::Rect r, const Card& c, size_t index) {
    const bool hover = r.Contains(mouse_.x, mouse_.y);
    const float lift = Hover(900000 + (int)index, hover);
    const auto base = MixColor(HomePalette().surface, HomePalette().card, .35f);
    if (c.job == PM_ROW_ACTIVE) Glow(r, kAccent, 16, 8, .6f);
    Box(r, D2D1::ColorF(base.r + lift * .025f, base.g + lift * .02f, base.b + lift * .03f), 16);
    Spotlight(r, 16, lift);
    if (c.job == PM_ROW_ERROR) {
        brush_->SetColor(D2D1::ColorF(0xd9536b, .35f));
        dc_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.x + .5f, r.y + .5f, r.Right() - .5f, r.Bottom() - .5f), 16, 16),
            brush_.Get(), 1);
    }
    if (auto* failed = c.job == PM_ROW_ERROR ? PictureBitmap(EmptyError) : nullptr)
        Image(failed, {r.x + 10, r.CenterY() - 31, 62, 62}, 0);  // треснувшая пластинка вместо обычной
    else
        Disc({r.x + 18, r.CenterY() - 23, 46, 46}, c.job, c.tools);

    // Кнопки справа по ширине подписи
    TextSpec spec;
    spec.size = 12;
    spec.weight = 600;
    float right = r.Right() - 16;
    for (size_t i = c.controls.size(); i-- > 0;) {
        const auto& control = c.controls[i];
        const float bw = std::clamp(text_->Measure(control.label, spec) + 34, 74.f, 220.f);
        right -= bw;
        Button({right, r.CenterY() - 16, bw, 32}, control.label, WorkspaceCommand, control.command,
               i == 0 && c.job != PM_ROW_ACTIVE);
        right -= 8;
    }
    const float textX = r.x + 80, textW = std::max(40.f, right - textX - 12);
    float statusW = 0;
    if (!c.status.empty()) {
        TextSpec pill;
        pill.size = 10;
        pill.weight = 700;
        statusW = text_->Measure(c.status, pill) + 20;
    }
    Text(c.title, {textX, r.y + 17, std::max(20.f, textW - statusW - 10), 24}, 15, kWhite, 600);
    if (statusW > 0) {
        const view::Rect pill{textX + std::min(textW - statusW, text_->Measure(c.title, TextSpec{15, 600}) + 12),
                              r.y + 19, statusW, 20};
        const auto tone = c.job == PM_ROW_ERROR  ? D2D1::ColorF(0xd9536b)
                          : c.job == PM_ROW_DONE ? D2D1::ColorF(0x83b87d)
                                                 : kAccent;
        Box(pill, Alpha(tone, .18f), 10);
        TextSpec ps;
        ps.size = 10;
        ps.weight = 700;
        ps.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        if (auto layout = text_->Layout(c.status, ps, pill.w, pill.h)) {
            brush_->SetColor(tone);
            dc_->DrawTextLayout(D2D1::Point2F(pill.x, pill.y + 3), layout.Get(), brush_.Get(),
                                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    Text(c.subtitle, {textX, r.y + 45, textW, 34}, 11, c.error ? D2D1::ColorF(0xffa6b5) : kMuted);
    if (c.progress >= 0) {
        const view::Rect track{textX, r.Bottom() - 13, r.Right() - textX - 18, 4};
        Box(track, D2D1::ColorF(0x2e2c3a), 2);
        const float fill = track.w * std::clamp(c.progress, 0.f, 1.f);
        if (fill > 1) {
            Glow({track.x, track.y, fill, track.h}, kAccent, 2, 5, .9f);
            D2D1_GRADIENT_STOP stops[] = {{0, kAccent}, {1, HomePalette().accentHover}};
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> gradient;
            collection = gradients_.Get(dc_, stops, 2);
            if (collection)
                dc_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(track.x, 0), D2D1::Point2F(track.x + fill, 0)),
                    collection.Get(), &gradient);
            if (gradient)
                dc_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track.x, track.y, track.x + fill, track.Bottom()), 2, 2),
                    gradient.Get());
            if (!reducedMotion_) {
                // Бегущий блик внутри заполненной части - работа идёт, даже если проценты стоят
                const float u = (float)std::fmod(clock_ * .7, 1.0);
                dc_->PushAxisAlignedClip(D2D1::RectF(track.x, track.y, track.x + fill, track.Bottom()),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                const float sx = track.x - 40 + (fill + 80) * u;
                brush_->SetColor(D2D1::ColorF(1, 1, 1, .45f));
                dc_->FillRectangle(D2D1::RectF(sx, track.y, sx + 26, track.Bottom()), brush_.Get());
                dc_->PopAxisAlignedClip();
                needsFrames_ = true;
            }
        }
    }
}
}  // namespace ui

namespace ui {
void Home::StepScroll() {
    const float k = reducedMotion_ ? 1 : 1 - std::exp(-frameDelta_ * 14);
    const float back = reducedMotion_ ? 1 : 1 - std::exp(-frameDelta_ * 9);
    // За краем цель возвращается к границе - получается мягкий отскок
    if (scrollTarget_ < 0) scrollTarget_ += (0 - scrollTarget_) * back;
    if (scrollTarget_ > scrollMax_) scrollTarget_ += (scrollMax_ - scrollTarget_) * back;
    scroll_ += (scrollTarget_ - scroll_) * k;
    if (std::abs(scrollTarget_ - scroll_) < .3f && scrollTarget_ >= -.3f && scrollTarget_ <= scrollMax_ + .3f)
        scroll_ = scrollTarget_ = std::clamp(scrollTarget_, 0.f, std::max(0.f, scrollMax_));
    else
        needsFrames_ = true;
}

void Home::ClampScroll(float maximum) {
    scrollMax_ = maximum;
    scrollTarget_ = std::clamp(scrollTarget_, -60.f, maximum + 60);
    scroll_ = std::clamp(scroll_, -60.f, maximum + 60);
}

void Home::PaintParticles() {
    const auto& accent = HomePalette().accent;
    const char* heart =
        "M20.8 4.6C18.5 2.5 14.8 3 12 6C9.2 3 5.5 2.5 3.2 4.6C.4 7.2 2.3 11.6 "
        "5.1 14.4L12 21L18.9 14.4C21.7 11.6 23.6 7.2 20.8 4.6Z";
    // Сердечки: восемь штук разлетаются по кругу, растут и гаснут
    std::erase_if(bursts_, [&](const Particle& p) { return clock_ - p.t > .7; });
    for (const auto& p : bursts_) {
        const float u = (float)((clock_ - p.t) / .7);
        const float ease = 1 - (1 - u) * (1 - u);
        for (int i = 0; i < 8; ++i) {
            const float a = i * kPi / 4 + .4f;
            const float dist = 8 + ease * 30, size = 7.f + (i % 2) * 3;
            const view::Rect r{p.x + std::cos(a) * dist - size / 2, p.y + std::sin(a) * dist - size / 2 - ease * 6,
                               size, size};
            PushFade(1 - u);
            Path(r, heart, i % 3 == 0 ? HomePalette().accent2 : accent, true);
            PopFade();
        }
        brush_->SetColor(Alpha(accent, (1 - u) * .7f));
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y), 6 + ease * 22, 6 + ease * 22), brush_.Get(),
                         2 * (1 - u));
        needsFrames_ = true;
    }
    // Скачанный трек дугой летит к пункту «Библиотека», оставляя искры; там вспыхивает «+1»
    const float tx = 37, ty = 116 + 42 + 19;
    auto where = [&](const Flyer& f, float e, float& x, float& y) {
        const float cx = (f.x + tx) / 2, cy = std::min(f.y, ty) - 120;
        x = (1 - e) * (1 - e) * f.x + 2 * (1 - e) * e * cx + e * e * tx;
        y = (1 - e) * (1 - e) * f.y + 2 * (1 - e) * e * cy + e * e * ty;
    };
    for (auto it = flyers_.begin(); it != flyers_.end();) {
        const float u = (float)((clock_ - it->t) / 1.);
        if (u >= 1) {
            libraryPulseAt_ = clock_;
            it = flyers_.erase(it);
            continue;
        }
        const float e = u * u * (3 - 2 * u);
        for (int spark = 1; spark <= 6; ++spark) {
            const float back = std::max(0.f, u - spark * .028f);
            float sx, sy;
            where(*it, back * back * (3 - 2 * back), sx, sy);
            const float dot = 5.f - spark * .55f;
            Box({sx - dot / 2, sy - dot / 2, dot, dot}, Alpha(accent, .55f - spark * .07f), dot / 2);
        }
        float x, y;
        where(*it, e, x, y);
        // Вылетает с небольшим подскоком и уменьшается к цели
        const float size = 52 * (1 - e) + 22 * e + std::sin(std::min(1.f, u * 4) * kPi) * 8;
        const view::Rect r{x - size / 2, y - size / 2, size, size};
        Glow(r, accent, size * .25f, 12, 1.5f);
        if (it->known)
            Art(it->track, r, size * .2f);
        else
            Disc(r, PM_ROW_DONE, false);
        needsFrames_ = true;
        ++it;
    }
}

// Иллюстрации нарисованы в малиновом: поворачиваем оттенок к акценту темы
void Home::TintedImage(ID2D1Bitmap1* bitmap, view::Rect r, float radius, float anchor, float anchorY, float zoom) {
    if (!bitmap || r.Empty()) return;
    const auto& c = HomePalette().accent;
    const float hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
    float hue = 0;
    if (hi - lo > .05f) {
        if (hi == c.r)
            hue = 60 * std::fmod((c.g - c.b) / (hi - lo) + 6, 6.f);
        else if (hi == c.g)
            hue = 60 * ((c.b - c.r) / (hi - lo) + 2);
        else
            hue = 60 * ((c.r - c.g) / (hi - lo) + 4);
    }
    float angle = std::fmod(hue - 350 + 720, 360.f);
    if (angle > 180) angle -= 360;
    if (hi - lo <= .05f || std::abs(angle) < 6 || (!hue_ && FAILED(dc_->CreateEffect(CLSID_D2D1HueRotation, &hue_)))) {
        Image(bitmap, r, radius, anchor, anchorY, zoom);
        return;
    }
    const auto size = bitmap->GetSize();
    float sw = size.width, sh = size.height;
    if (sw / sh > r.w / r.h)
        sw = sh * r.w / r.h;
    else
        sh = sw * r.h / r.w;
    sw /= zoom;
    sh /= zoom;
    const auto source = D2D1::RectF((size.width - sw) * anchor, (size.height - sh) * anchorY,
                                    (size.width - sw) * anchor + sw, (size.height - sh) * anchorY + sh);
    hue_->SetInput(0, bitmap);
    hue_->SetValue(0u, angle);  // D2D1_HUEROTATION_PROP_ANGLE (градусы): у MinGW нет перечисления
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    renderer_->Factory()->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), radius, radius), &clip);
    if (clip) dc_->PushLayer(D2D1::LayerParameters(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), clip.Get()), nullptr);
    D2D1_MATRIX_3X2_F previous;
    dc_->GetTransform(&previous);
    // Участок source ложится в r: сдвиг к началу участка, масштаб, перенос в r
    dc_->SetTransform(D2D1::Matrix3x2F::Translation(-source.left, -source.top) *
                      D2D1::Matrix3x2F::Scale(r.w / sw, r.h / sh) * D2D1::Matrix3x2F::Translation(r.x, r.y) * previous);
    dc_->DrawImage(hue_.Get(), D2D1::Point2F(source.left, source.top), source, D2D1_INTERPOLATION_MODE_LINEAR);
    dc_->SetTransform(previous);
    if (clip) dc_->PopLayer();
}
}  // namespace ui

namespace ui {
void Home::MixCard(view::Rect r, const std::wstring& title, const std::wstring& subtitle, const std::vector<view::HomeTrack>& tracks,
                   Action action, int index, int key, Picture cover) {
    const auto& p = HomePalette();
    const bool over = r.Contains(mouse_.x, mouse_.y);
    const float lift = Hover(760000 + key, over);
    r.y -= lift * 3;
    const float enter = Enter(key);
    if (enter < 1) PushFade(enter);
    if (lift > .01f) Glow(r, p.accent, 16, 10, lift * .8f);
    Box(r, MixColor(p.surface, p.card, .6f + lift * .3f), 16);
    Spotlight(r, 16, lift);
    // Коллаж: квадрат слева из четырёх обложек, в стыках - тонкий зазор
    const view::Rect art{r.x + 10, r.y + 10, r.h - 20, r.h - 20};
    if (auto* picture = PictureBitmap(cover)) {
        // Своя обложка подборки; коллаж - запасной вариант, когда картинки нет
        Glow(art, p.accent, 10, 6, .4f + lift * .5f);
        Image(picture, art, 10);
        Text(title, {art.Right() + 14, r.CenterY() - 26, r.Right() - art.Right() - 24, 24}, 15, p.white, 650);
        Text(subtitle, {art.Right() + 14, r.CenterY(), r.Right() - art.Right() - 24, 32}, 11, p.muted);
        hits_.push_back({r, action, index, title});
        if (enter < 1) PopFade();
        return;
    }
    const float half = (art.w - 3) / 2;
    for (int i = 0; i < 4; ++i) {
        const view::Rect cell{art.x + (i % 2) * (half + 3), art.y + (i / 2) * (half + 3), half, half};
        if (!tracks.empty()) Art(tracks[(size_t)i % tracks.size()], cell);
        else Box(cell, MixColor(p.card, p.accent, .15f), 6);
    }
    // Вуаль акцента снизу: коллаж читается как одна обложка подборки
    D2D1_GRADIENT_STOP stops[] = {{0, Alpha(p.accent, 0)}, {1, Alpha(p.accent, .55f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> veil;
    collection = gradients_.Get(dc_, stops, 2);
    if (collection)
        dc_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, art.y), D2D1::Point2F(0, art.Bottom())), collection.Get(), &veil);
    if (veil) dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(art.x, art.y, art.Right(), art.Bottom()), 10, 10), veil.Get());
    Text(title, {art.Right() + 14, r.CenterY() - 26, r.Right() - art.Right() - 24, 24}, 15, p.white, 650);
    Text(subtitle, {art.Right() + 14, r.CenterY(), r.Right() - art.Right() - 24, 32}, 11, p.muted);
    hits_.push_back({r, action, index});
    if (enter < 1) PopFade();
}
}  // namespace ui
