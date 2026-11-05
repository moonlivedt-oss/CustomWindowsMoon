// Мелкие живые эффекты главной: волна от щелчка, прожектор под курсором, магнитные кнопки,
// параллакс баннера, «пересыпание» букв названия, живые иконки меню и рамки обложки.
// При «меньше движения» всё, что двигается само, отключается; отклик на курсор остаётся статичным.
#include "ui/home.h"
#include <d2d1effects.h>
#include "ui/home_colors.h"
#include <cmath>

namespace ui {
namespace {
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return {c.r, c.g, c.b, a}; }
}  // namespace

// ---------------- волна от щелчка ----------------

void Home::AddRipple(view::Point p, view::Rect r, float radius) {
    if (reducedMotion_ || r.w > 700 || r.Empty()) return;
    if (ripples_.size() > 6) ripples_.erase(ripples_.begin());
    ripples_.push_back({p, r, radius, clock_});
}

void Home::PaintRipples() {
    std::erase_if(ripples_, [&](const Ripple& r) { return clock_ - r.at > .6; });
    for (const auto& r : ripples_) {
        const float u = (float)((clock_ - r.at) / .6);
        const float ease = 1 - (1 - u) * (1 - u) * (1 - u);
        const float reach = std::hypot(std::max(r.p.x - r.rect.x, r.rect.Right() - r.p.x),
                                       std::max(r.p.y - r.rect.y, r.rect.Bottom() - r.p.y));
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        const auto rect = D2D1::RectF(r.rect.x, r.rect.y, r.rect.Right(), r.rect.Bottom());
        renderer_->Factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, r.radius, r.radius), &clip);
        if (!clip) continue;
        dc_->PushLayer(D2D1::LayerParameters(rect, clip.Get()), nullptr);
        brush_->SetColor(WithAlpha(HomePalette().white, .16f * (1 - u)));
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.p.x, r.p.y), reach * ease, reach * ease), brush_.Get());
        dc_->PopLayer();
        needsFrames_ = true;
    }
}

// ---------------- прожектор ----------------

// Пятно света под курсором и подсвеченная ближняя к курсору часть рамки
void Home::Spotlight(view::Rect r, float radius, float strength) {
    if (!r.Contains(mouse_.x, mouse_.y) || strength <= 0) return;
    const auto& p = HomePalette();
    const float size = std::max(r.w, r.h) * .9f;
    D2D1_GRADIENT_STOP stops[] = {{0, WithAlpha(p.white, .10f * strength)}, {1, WithAlpha(p.white, 0)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1RadialGradientBrush> light;
    collection = gradients_.Get(dc_, stops, 2);
    if (!collection) return;
    dc_->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(mouse_.x, mouse_.y), D2D1::Point2F(0, 0), size, size),
        collection.Get(), &light);
    const auto rect = D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), radius, radius);
    if (light) dc_->FillRoundedRectangle(rect, light.Get());
    D2D1_GRADIENT_STOP edge[] = {{0, WithAlpha(p.accent, .9f * strength)}, {1, WithAlpha(p.accent, 0)}};
    ComPtr<ID2D1GradientStopCollection> edgeStops;
    ComPtr<ID2D1RadialGradientBrush> rim;
    edgeStops = gradients_.Get(dc_, edge, 2);
    if (edgeStops)
        dc_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(mouse_.x, mouse_.y), D2D1::Point2F(0, 0), size * .55f,
                                                size * .55f),
            edgeStops.Get(), &rim);
    if (rim) dc_->DrawRoundedRectangle(rect, rim.Get(), 1.4f);
}

// ---------------- магнит ----------------

// Смещение кнопки к курсору: чем ближе, тем сильнее; плавно догоняет и плавно возвращается
view::Point Home::Magnet(int key, view::Rect r, float reach, float pull) {
    auto& m = magnets_[key];
    view::Point target{0, 0};
    const float dx = mouse_.x - r.CenterX(), dy = mouse_.y - r.CenterY();
    const float d = std::hypot(dx, dy), limit = std::max(r.w, r.h) / 2 + reach;
    if (!reducedMotion_ && mouse_.x >= 0 && d < limit) {
        const float k = (1 - d / limit) * pull;
        target = {dx * k, dy * k};
    }
    const float follow = 1 - std::exp(-frameDelta_ * 14);
    m.x += (target.x - m.x) * follow;
    m.y += (target.y - m.y) * follow;
    if (std::abs(target.x - m.x) + std::abs(target.y - m.y) > .05f) needsFrames_ = true;
    return m;
}

// ---------------- параллакс ----------------

view::Point Home::Parallax(int key, view::Rect r) {
    view::Point target{0, 0};
    if (!reducedMotion_ && r.Contains(mouse_.x, mouse_.y))
        target = {(mouse_.x - r.CenterX()) / (r.w / 2), (mouse_.y - r.CenterY()) / (r.h / 2)};
    // Плавно, медленнее магнита: картинка «плывёт» за курсором
    auto& m = magnets_[key + 8000000];
    const float follow = 1 - std::exp(-frameDelta_ * 6);
    m.x += (target.x - m.x) * follow;
    m.y += (target.y - m.y) * follow;
    if (std::abs(target.x - m.x) + std::abs(target.y - m.y) > .002f) needsFrames_ = true;
    return m;
}

// ---------------- пересыпание букв ----------------

// Новое название проявляется слева направо, ещё не «осевшие» буквы перебирают случайные знаки
std::wstring Home::Scramble(const std::wstring& text, double since) {
    if (reducedMotion_) return text;
    const float u = (float)((clock_ - since) / .55);
    if (u >= 1 || u < 0) return text;
    needsFrames_ = true;
    static const wchar_t glyphs[] =
        L"АБВГДЕЖЗИКЛМНОПРСТУФХЦЧШЭЮЯабвгдежзиклмнопрстуфхцчшэюяABCDEFGHKMNPRSTUVWXYZ0123456789";
    const size_t count = sizeof(glyphs) / sizeof(glyphs[0]) - 1;
    const size_t settled = (size_t)(u * u * (text.size() + 4));
    std::wstring out = text;
    const unsigned frame = (unsigned)(clock_ * 30);
    for (size_t i = settled; i < out.size(); ++i) {
        if (out[i] == L' ') continue;
        out[i] = glyphs[(i * 7919u + frame * 104729u) % count];
    }
    return out;
}

// ---------------- живые иконки меню ----------------

// Каждая иконка при наведении делает своё: дом подпрыгивает, сердце бьётся, стрелка опускается,
// шестерёнка поворачивается. Возвращает преобразование вокруг центра иконки
D2D1_MATRIX_3X2_F Home::NavIconMotion(int index, view::Rect icon, float over) {
    const auto c = D2D1::Point2F(icon.CenterX(), icon.CenterY());
    if (reducedMotion_ || over <= .01f) return D2D1::IdentityMatrix();
    needsFrames_ = true;
    const float t = (float)clock_;
    switch (index) {
        case 0:
            return D2D1::Matrix3x2F::Translation(0, -2.5f * over * std::abs(std::sin(t * 6)));
        case 1:
            return D2D1::Matrix3x2F::Rotation(-8 * over, c);
        case 2: {
            const float beat = 1 + .14f * over * std::pow(std::max(0.f, std::sin(t * 7)), 6.f);
            return D2D1::Matrix3x2F::Scale(beat, beat, c);
        }
        case 3:
            return D2D1::Matrix3x2F::Translation(2.5f * over * std::sin(t * 5), 0);
        case 4:
            return D2D1::Matrix3x2F::Rotation(45 * over, c);
        case 5:
            return D2D1::Matrix3x2F::Translation(0, 2.5f * over * std::abs(std::sin(t * 5)));
        case 6:
            return D2D1::Matrix3x2F::Rotation(90 * over + t * 40 * over, c);
        default:
            return D2D1::IdentityMatrix();
    }
}

// ---------------- рамки обложки ----------------

// Широкая шапка страницы: картина с лёгким параллаксом, вуаль слева под заголовок и блик при входе
void Home::PageBanner(view::Rect r, ID2D1Bitmap1* picture, int key, float radius) {
    const auto& p = HomePalette();
    Glow(r, p.accent, radius, 12, .3f);
    Box(r, p.surface, radius);
    const auto depth = Parallax(key, r);
    Image(picture, r, radius, 1.f - .06f + depth.x * .05f, .5f + depth.y * .1f, 1.09f);
    D2D1_GRADIENT_STOP stops[] = {{0, WithAlpha(p.bg, .95f)}, {.55f, WithAlpha(p.bg, .55f)}, {1, WithAlpha(p.bg, 0)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> veil;
    collection = gradients_.Get(dc_, stops, 3);
    if (collection)
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, 0), D2D1::Point2F(r.Right(), 0)),
                                       collection.Get(), &veil);
    if (veil) dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), radius, radius), veil.Get());
    if (!reducedMotion_) {
        const float sweep = (float)((clock_ - pageEnteredAt_ - .2) / 1.4);
        Sweep(r, radius, sweep);
        if (sweep < 1) needsFrames_ = true;
    }
}

// PNG из assets/frames с прозрачным окном; окно меряется при загрузке
D2D1_RECT_F Home::FrameDest(view::Rect art, int style, view::Rect* window) const {
    const auto& f = WindowOfFrame(style - 1);
    // Масштаб: меньшая сторона окна чуть меньше обложки - край картинки уходит под рамку, без щели
    const float side = std::min(f.r - f.l, f.b - f.t);
    const float size = art.w * .97f / std::max(.2f, side);
    const float cx = (f.l + f.r) / 2, cy = (f.t + f.b) / 2;
    const float x = art.CenterX() - size * cx, y = art.CenterY() - size * cy;
    if (window) *window = {x + size * f.l, y + size * f.t, size * (f.r - f.l), size * (f.b - f.t)};
    return D2D1::RectF(x, y, x + size, y + size);
}

void Home::CoverFrame(view::Rect art, int style, const Input* live) {
    if (!HasFrame(style)) return;
    auto& bitmap = frameBitmaps_[(size_t)style - 1];
    if (!bitmap) {
        // Картинка в памяти не нужна, как только она на видеокарте; после смены устройства читается заново
        auto& source = frameSources_[(size_t)style - 1];
        if (!source) source = DecodeFrame(style - 1);
        if (source) dc_->CreateBitmapFromWicBitmap(source.Get(), &bitmap);
        source.Reset();
    }
    if (!bitmap) return;
    const auto dest = FrameDest(art, style);
    const bool alive = live && live->ambient && !reducedMotion_;
    enum { Lunar = 1, Neon, Vinyl, Sunset, Frost, Cassette, Gold, Polaroid, Film, AuroraFrame };
    // «Сияние» переливается: оттенок плавно гуляет и подскакивает на долю
    bool drawn = false;
    if (alive && style == AuroraFrame && (hue_ || SUCCEEDED(dc_->CreateEffect(CLSID_D2D1HueRotation, &hue_)))) {
        hue_->SetInput(0, bitmap.Get());
        hue_->SetValue(0u, 28 * std::sin((float)clock_ * .7f) + live->beat * 34);
        const auto size = bitmap->GetSize();
        D2D1_MATRIX_3X2_F previous;
        dc_->GetTransform(&previous);
        dc_->SetTransform(D2D1::Matrix3x2F::Scale((dest.right - dest.left) / size.width, (dest.bottom - dest.top) / size.height) *
                          D2D1::Matrix3x2F::Translation(dest.left, dest.top) * previous);
        dc_->DrawImage(hue_.Get());
        dc_->SetTransform(previous);
        drawn = true;
    }
    if (!drawn) dc_->DrawBitmap(bitmap.Get(), dest, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (!alive) return;

    // Подъём на долю: сама рамка вспыхивает, свечение не вылезает за её контур. Бумага полароида не светится
    static const float lift[] = {.25f, .6f, .2f, .3f, .25f, .15f, .22f, 0, .2f, .4f};
    const float bright = lift[style - 1] * std::min(1.f, live->beat * .8f + live->energy * .3f);
    if (bright > .01f) {
        dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
        dc_->DrawBitmap(bitmap.Get(), dest, bright, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
    const float W = dest.right - dest.left, H = dest.bottom - dest.top;
    const auto pixels = bitmap->GetSize();
    const auto oldMode = dc_->GetAntialiasMode();
    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // FillOpacityMask работает только так
    if (style == Film) {
        // По кромкам плёнки бегут полосы света, будто лента тянется через кадр
        const auto& f = WindowOfFrame(style - 1);
        const float period = W * .0727f, phase = std::fmod((float)clock_ * 42, period);
        D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(1, .78f, .84f, 0)},
                                      {.42f, D2D1::ColorF(1, .78f, .84f, 0)},
                                      {.5f, D2D1::ColorF(1, .78f, .84f, .13f)},
                                      {.58f, D2D1::ColorF(1, .78f, .84f, 0)},
                                      {1, D2D1::ColorF(1, .78f, .84f, 0)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        collection = gradients_.Get(dc_, stops, 5, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_WRAP);
        if (collection) {
            // Две стороны - полосы идут по горизонтали, две - по вертикали, ленту обегает по кругу
            struct Side {
                float l, t, r, b;
                float dx, dy;
            };
            const Side sides[] = {{0, 0, 1, f.t, 1, 0}, {f.r, f.t, 1, f.b, 0, 1}, {0, f.b, 1, 1, -1, 0}, {0, f.t, f.l, f.b, 0, -1}};
            for (const auto& side : sides) {
                ComPtr<ID2D1LinearGradientBrush> stripes;
                const auto from = D2D1::Point2F(dest.left + side.dx * phase, dest.top + side.dy * phase);
                dc_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(from, D2D1::Point2F(from.x + std::abs(side.dx) * period, from.y + std::abs(side.dy) * period)),
                    collection.Get(), &stripes);
                if (!stripes) continue;
                const auto where = D2D1::RectF(dest.left + W * side.l, dest.top + H * side.t, dest.left + W * side.r, dest.top + H * side.b);
                const auto source = D2D1::RectF(pixels.width * side.l, pixels.height * side.t, pixels.width * side.r, pixels.height * side.b);
                dc_->FillOpacityMask(bitmap.Get(), stripes.Get(), where, source);
            }
        }
    } else {
        // Блик наискось проходит по рамке раз в несколько секунд; у золота - ярче и чаще
        static const float strength[] = {.3f, .16f, .28f, .28f, .4f, .22f, .75f, .14f, 0, .2f};
        static const float every[] = {7, 7, 8, 7, 6, 8, 4.5f, 10, 7, 8};
        const float u = std::fmod((float)clock_, every[style - 1]) / every[style - 1] / .38f;
        if (u < 1) {
            const float c = -.25f + 1.5f * u, band = .14f;
            auto at = [&](float k) { return D2D1::Point2F(dest.left + W * k, dest.top + H * k); };
            const auto tint = style == Gold ? D2D1::ColorF(1, .9f, .62f, strength[style - 1]) : D2D1::ColorF(1, 1, 1, strength[style - 1]);
            D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(tint.r, tint.g, tint.b, 0)}, {.5f, tint}, {1, D2D1::ColorF(tint.r, tint.g, tint.b, 0)}};
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> sheen;
            collection = gradients_.Get(dc_, stops, 3);
            if (collection)
                dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(at(c - band), at(c + band)), collection.Get(), &sheen);
            if (sheen) dc_->FillOpacityMask(bitmap.Get(), sheen.Get(), dest);
        }
    }
    dc_->SetAntialiasMode(oldMode);
}

// Едва заметное зерно убирает полосы в тёмных градиентах; вспышка света от правого края - при смене вида
void Home::PaintFilm(const Input& in, float w, float bottom) {
    if (in.highContrast) return;
    D2D1_MATRIX_3X2_F previous;
    dc_->GetTransform(&previous);
    dc_->SetTransform(D2D1::IdentityMatrix());
    const double since = !reducedMotion_ ? std::min(clock_ - pageSwitchedAt_, nowOpen_ ? clock_ - nowToggledAt_ : 10.) : 10.;
    if (since < .75) {
        const float u = (float)(since / .75), a = std::sin(3.14159265f * std::sqrt(u)) * .17f;
        const auto warm = MixColor(HomePalette().accent, D2D1::ColorF(0xffb27a), .55f);
        D2D1_GRADIENT_STOP stops[] = {{0, WithAlpha(warm, a)}, {.5f, WithAlpha(warm, a * .3f)}, {1, WithAlpha(warm, 0)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1RadialGradientBrush> leak;
        collection = gradients_.Get(dc_, stops, 3);
        if (collection)
            dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(w * (.98f - u * .08f), bottom * .3f), D2D1::Point2F(0, 0), w * .55f, bottom * .95f),
                                           collection.Get(), &leak);
        if (leak) {
            dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
            dc_->FillRectangle(D2D1::RectF(0, 0, w, bottom), leak.Get());
            dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        }
        needsFrames_ = true;
    }
    if (!grain_) {
        constexpr UINT32 kSide = 256;
        std::vector<uint32_t> noise(kSide * kSide);
        uint32_t state = 0x2f6e2b1u;
        for (auto& pixel : noise) {
            state = state * 1664525u + 1013904223u;
            const uint32_t a = (state >> 24) % 5;
            // Предумноженный BGRA: половина точек светлит, половина темнит
            pixel = (state >> 16 & 1) ? (a << 24 | a << 16 | a << 8 | a) : a << 24;
        }
        ComPtr<ID2D1Bitmap1> bitmap;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(kSide, kSide), noise.data(), kSide * 4, props, &bitmap)))
            dc_->CreateBitmapBrush(bitmap.Get(), &grain_);
        if (grain_) {
            grain_->SetExtendModeX(D2D1_EXTEND_MODE_WRAP);
            grain_->SetExtendModeY(D2D1_EXTEND_MODE_WRAP);
            grain_->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }
    }
    if (grain_) {
        // Пока играет музыка, зерно живёт: 12 смен в секунду, как на плёнке
        const int tick = in.ambient && !reducedMotion_ ? (int)(clock_ * 12) : 0;
        grain_->SetTransform(D2D1::Matrix3x2F::Translation((float)(tick * 73 % 256), (float)(tick * 151 % 256)));
        dc_->FillRectangle(D2D1::RectF(0, 0, in.size.w, in.size.h), grain_.Get());
    }
    dc_->SetTransform(previous);
}

ID2D1Bitmap1* Home::PictureBitmap(Picture which) {
    // Путь и наибольшая нужная сторона: обложки показываются не крупнее 512, баннеры - во всю ширину окна
    struct Entry {
        const wchar_t* file;
        UINT side;
    };
    static const Entry table[kPictures] = {
        {L"recap/night.png", 1200},     {L"recap/records.png", 1200},   {L"empty/downloads.png", 512},
        {L"empty/plugins.png", 512},    {L"icons/playermoon.png", 128},
        {L"placeholders/1.png", 512},   {L"placeholders/2.png", 512},   {L"placeholders/3.png", 512},
        {L"placeholders/4.png", 512},   {L"placeholders/5.png", 512},   {L"placeholders/6.png", 512},
        {L"placeholders/7.png", 512},   {L"placeholders/8.png", 512},   {L"placeholders/9.png", 512},
        {L"placeholders/10.png", 512},  {L"placeholders/11.png", 512},  {L"placeholders/12.png", 512},
        {L"mixes/wave.png", 384},       {L"mixes/favorites.png", 384},  {L"mixes/forgotten.png", 384},
        {L"mixes/fresh.png", 384},      {L"mixes/morning.png", 384},    {L"mixes/day.png", 384},
        {L"mixes/evening.png", 384},    {L"mixes/night.png", 384},
        {L"empty/favorites.png", 512},  {L"empty/library.png", 512},    {L"empty/queue.png", 512},
        {L"empty/search.png", 512},     {L"empty/lyrics.png", 512},     {L"empty/offline.png", 512},
        {L"empty/error.png", 256},      {L"empty/instrumental.png", 512},
        {L"banners/neon.png", 2172},    {L"banners/frost.png", 2172},   {L"banners/sunset.png", 2172},
        {L"banners/minimal.png", 2172}, {L"banners/glass.png", 2172},
        {L"banners/library.png", 2172}, {L"banners/favorites.png", 2172}, {L"banners/queue.png", 2172},
        {L"banners/settings.png", 2172},
        {L"recap/total.png", 1448},     {L"recap/artist.png", 1448},    {L"recap/track.png", 1448},
        {L"recap/time.png", 1448},      {L"intro/backdrop.png", 1600},  {L"karaoke/backdrop.png", 1600},
        {L"now/nocover.png", 1600},
    };
    if (pictureGeneration_ != renderer_->DeviceGeneration()) {
        for (auto& b : pictureBitmaps_) b.Reset();
        pictureGeneration_ = renderer_->DeviceGeneration();
    }
    if (!pictureTried_[which]) {
        pictureTried_[which] = true;
        pictureSources_[which] = LoadAsset(assetsDir_ / table[which].file, table[which].side);
    }
    auto& bitmap = pictureBitmaps_[which];
    if (!bitmap && !pictureSources_[which] && pictureUploaded_[which])
        pictureSources_[which] = LoadAsset(assetsDir_ / table[which].file, table[which].side);  // устройство сменилось
    if (!bitmap && pictureSources_[which] && dc_) {
        dc_->CreateBitmapFromWicBitmap(pictureSources_[which].Get(), &bitmap);
        pictureUploaded_[which] = true;
        // На видеокарте копия уже есть. Заглушкам пиксели ещё нужны: по ним считается настроение (CoverMood)
        if (bitmap && (which < Placeholder1 || which >= Placeholder1 + 12)) pictureSources_[which].Reset();
    }
    return bitmap.Get();
}

bool Home::HasFrame(int style) const { return style > 0 && style <= kFrames && !frameFiles_[(size_t)style - 1].empty(); }
}  // namespace ui
