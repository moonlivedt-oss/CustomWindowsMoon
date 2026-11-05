// «Сейчас играет» на весь экран: размытая обложка вместо фона, большая обложка, синхронный текст
// с подсветкой строки, карточка «Дальше» и волна по низу. Открывается щелчком по обложке в нижней панели.
#include "ui/home.h"
#include "ui/home_colors.h"
#include <cmath>
#include <d2d1effects.h>

namespace ui {
namespace {
float Smooth(float u) {
    u = std::clamp(u, 0.f, 1.f);
    return u * u * (3 - 2 * u);
}
}  // namespace

void Home::ToggleNowPlaying() {
    nowOpen_ = !nowOpen_;
    nowToggledAt_ = clock_;
}

float Home::NowPlayingAmount() {
    const float u = reducedMotion_ ? 1 : std::clamp((float)((clock_ - nowToggledAt_) / .55), 0.f, 1.f);
    if (u < 1) needsFrames_ = true;
    return nowOpen_ ? Smooth(u) : 1 - Smooth(u);
}

// Обложка, растянутая на всё окно и размытая, - цвет альбома становится фоном
void Home::BlurredCover(ID2D1Bitmap1* bitmap, view::Rect area, float opacity) {
    if (!bitmap) return;
    const auto size = bitmap->GetSize();
    const float scale = std::max(area.w / size.width, area.h / size.height) * 1.15f;
    const auto place = D2D1::Matrix3x2F::Translation(-size.width / 2, -size.height / 2) * D2D1::Matrix3x2F::Scale(scale, scale) *
                       D2D1::Matrix3x2F::Translation(area.CenterX(), area.CenterY());
    D2D1_MATRIX_3X2_F previous;
    dc_->GetTransform(&previous);
    const auto drawLive = [&] {
        // Запасной путь: размытие прямо в кадре
        if (!blur_ && FAILED(dc_->CreateEffect(CLSID_D2D1GaussianBlur, &blur_))) return;
        blur_->SetInput(0, bitmap);
        blur_->SetValue(0u, std::max(size.width, size.height) / 18);
        blur_->SetValue(2u, (UINT32)1);
        dc_->SetTransform(place * previous);
        PushFade(opacity);
        dc_->DrawImage(blur_.Get());
        PopFade();
        dc_->SetTransform(previous);
    };
    // Размытие на весь экран - самая дорогая операция кадра, а результат зависит только от обложки и размера окна:
    // считаем один раз на отдельной поверхности, дальше рисуем готовую картинку
    if (blurOf_.Get() != bitmap || blurW_ != area.w || blurH_ != area.h || !blurred_) {
        blurred_.Reset();
        blurOf_ = bitmap;
        blurW_ = area.w;
        blurH_ = area.h;
        ComPtr<ID2D1DeviceContext> aux;
        float dpiX = 96, dpiY = 96;
        dc_->GetDpi(&dpiX, &dpiY);
        if (renderer_->Device()) renderer_->Device()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &aux);
        ComPtr<ID2D1Effect> effect;
        if (aux && SUCCEEDED(aux->CreateEffect(CLSID_D2D1GaussianBlur, &effect))) {
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY);
            const D2D1_SIZE_U pixels = D2D1::SizeU((UINT32)std::ceil(area.w * dpiX / 96), (UINT32)std::ceil(area.h * dpiY / 96));
            if (SUCCEEDED(aux->CreateBitmap(pixels, nullptr, 0, props, &blurred_))) {
                effect->SetInput(0, bitmap);
                effect->SetValue(0u, std::max(size.width, size.height) / 18);  // D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION
                effect->SetValue(2u, (UINT32)1);                               // D2D1_GAUSSIANBLUR_PROP_BORDER_MODE: HARD
                aux->SetDpi(dpiX, dpiY);
                aux->SetTarget(blurred_.Get());
                aux->BeginDraw();
                aux->Clear(D2D1::ColorF(0, 0, 0, 0));
                aux->SetTransform(place * D2D1::Matrix3x2F::Translation(-area.x, -area.y));
                aux->DrawImage(effect.Get());
                if (FAILED(aux->EndDraw())) blurred_.Reset();
            }
        }
    }
    if (!blurred_) {
        drawLive();
        return;
    }
    dc_->DrawBitmap(blurred_.Get(), D2D1::RectF(area.x, area.y, area.x + blurred_->GetSize().width, area.y + blurred_->GetSize().height), opacity,
                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

// Цвет трека: из настоящей обложки, а у трека без неё - из его картины-заглушки
D2D1_COLOR_F Home::CoverMood(const Input& in, bool hasCover) {
    if (in.highContrast || in.current.empty()) return in.colors.accent;
    const uint64_t key = std::hash<std::wstring>{}(in.current.native());
    if (hasCover) {
        const auto known = coverMood_.find(key);
        if (known != coverMood_.end()) return known->second;
    }
    const size_t slot = key % 12;
    if (placeholderMoodReady_[slot]) return placeholderMood_[slot];
    const auto which = (Picture)(Placeholder1 + slot);
    PictureBitmap(which);
    placeholderMoodReady_[slot] = true;
    placeholderMood_[slot] = in.colors.accent;
    if (auto* source = pictureSources_[which].Get()) {
        UINT pw = 0, ph = 0;
        source->GetSize(&pw, &ph);
        std::vector<uint8_t> pixels((size_t)pw * ph * 4);
        if (pw && SUCCEEDED(source->CopyPixels(nullptr, pw * 4, (UINT)pixels.size(), pixels.data()))) {
            const auto mood = theme::CoverAccent(theme::PaletteFromPixels(pixels.data(), (int)(pw * ph), ""), true);
            placeholderMood_[slot] = D2D1::ColorF(mood.r, mood.g, mood.b);
        }
    }
    return placeholderMood_[slot];
}

// Карточка справа: что заиграет дальше; при смене трека строки поднимаются на одну
void Home::PaintNextUp(const Input& in, view::Rect r, float fade) {
    const auto& p = HomePalette();
    PushFade(fade);
    Box(r, D2D1::ColorF(p.surface.r, p.surface.g, p.surface.b, .62f), 16);
    brush_->SetColor(D2D1::ColorF(p.white.r, p.white.g, p.white.b, .07f));
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), 16, 16), brush_.Get(), 1);
    Text(L"ДАЛЬШЕ", {r.x + 18, r.y + 15, r.w - 36, 18}, 10, p.accent, 700);
    const float rowH = 66, listTop = r.y + 42, rx = r.x + 14, rw = r.w - 28;
    const float shift = reducedMotion_ ? 1 : std::clamp((float)((clock_ - trackChangedAt_) / .45), 0.f, 1.f);
    const float rising = (1 - shift) * (1 - shift) * (1 - shift) * rowH;
    if (shift < 1) needsFrames_ = true;
    dc_->PushAxisAlignedClip(D2D1::RectF(r.x, listTop - 4, r.Right(), r.Bottom() - 6), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const int first = in.playback.index + 1;
    const int rows = (int)((r.h - 52) / rowH);
    for (int k = 0; k < rows && first + k < (int)in.queue.size(); ++k) {
        const auto& t = in.queue[(size_t)first + k];
        const view::Rect row{rx, listTop + k * rowH + rising, rw, rowH - 6};
        const bool over = row.Contains(mouse_.x, mouse_.y);
        if (over) Box(row, MixColor(p.card, p.white, .05f), 10);
        Art(t, {row.x + 6, row.y + 6, 48, 48}, 8);
        Text(t.title, {row.x + 66, row.y + 8, row.w - 74, 22}, 13, p.white, 600);
        Text(t.artist.empty() ? L"Локальный трек" : t.artist, {row.x + 66, row.y + 31, row.w - 74, 20}, 11, p.muted);
        if (fade > .5f) hits_.push_back({row, PlayTrack, -(first + k) - 2, t.title});
    }
    dc_->PopAxisAlignedClip();
    PopFade();
}

// Караоке: прожектор на сцене дышит в такт, в луче медленно плывут пылинки
void Home::PaintStage(view::Rect area, const Input& in, float stage) {
    const auto warm = D2D1::ColorF(1.f, .72f, .78f);
    const float lamp = (.05f + std::min(1.f, in.beat * .8f + in.energy * .35f) * .2f) * stage;
    D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(warm.r, warm.g, warm.b, lamp)},
                                  {.55f, D2D1::ColorF(warm.r, warm.g, warm.b, lamp * .3f)},
                                  {1, D2D1::ColorF(warm.r, warm.g, warm.b, 0)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1RadialGradientBrush> beam;
    collection = gradients_.Get(dc_, stops, 3);
    const float lx = area.x + area.w * .85f, ly = area.y + area.h * .2f;
    if (collection)
        dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(lx, ly), D2D1::Point2F(0, 0), area.w * .2f, area.h * .42f),
                                       collection.Get(), &beam);
    if (beam) {
        dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
        dc_->FillRectangle(D2D1::RectF(area.x, area.y, area.Right(), area.Bottom()), beam.Get());
        dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
    if (reducedMotion_) return;
    const float t = (float)clock_;
    for (int i = 0; i < 34; ++i) {
        const float seed = i * 12.9898f;
        const float a = std::fmod(std::abs(std::sin(seed) * 43758.5f), 1.f), b = std::fmod(std::abs(std::sin(seed * 1.7f) * 24634.6f), 1.f);
        const float climb = std::fmod(t * (.012f + a * .02f) + b, 1.f);
        const float x = area.x + area.w * (.72f + a * .24f + .012f * std::sin(t * .4f + seed));
        const float y = area.y + area.h * (.66f - climb * .64f);
        const float size = 1.2f + b * 1.8f;
        // Появляются и исчезают по краям пути; на долю вспыхивают
        const float alpha = std::sin(climb * 3.14159f) * (.18f + .4f * std::abs(std::sin(t * .9f + seed))) * (1 + in.beat * .9f) * stage;
        brush_->SetColor(D2D1::ColorF(1, .88f, .84f, std::min(1.f, alpha)));
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), size, size), brush_.Get());
    }
    needsFrames_ = true;
}

void Home::PaintNowPlaying(const Input& in, float amount, float w, float bottom) {
    const auto& p = HomePalette();
    const view::Rect area{0, 0, w, bottom};
    PushFade(amount);
    Box(area, p.bg, 0);
    const view::HomeTrack& current = currentTrack_;
    RequestArt(current);
    ID2D1Bitmap1* cover = in.current.empty() ? nullptr
                                             : art_.Get(std::hash<std::wstring>{}(in.current.native()), dc_,
                                                        renderer_->DeviceGeneration());
    // Весь экран живёт в цвете обложки: свечение, пятна фона, текущая строка и волна.
    // Смена цвета плавная; нижняя панель и остальной плеер остаются в цветах темы
    {
        const auto target = CoverMood(in, cover != nullptr);
        if (reducedMotion_ || clock_ - moodPaintedAt_ > .2) {
            moodNow_ = target;
        } else {
            moodNow_ = MixColor(moodNow_, target, 1 - std::exp(-frameDelta_ * 3.5f));
            if (std::abs(moodNow_.r - target.r) + std::abs(moodNow_.g - target.g) + std::abs(moodNow_.b - target.b) > .01f) needsFrames_ = true;
        }
        moodPaintedAt_ = clock_;
        HomeColors mood = in.colors;
        if (!in.highContrast) {
            mood.accent = moodNow_;
            mood.accent2 = MixColor(moodNow_, D2D1::ColorF(0xa78bfa), .45f);
        }
        SetHomePalette(mood);
        auroraMood_ = !in.highContrast;
    }
    if (auto* plain = cover ? nullptr : PictureBitmap(NowNoCover)) {
        // Трек без обложки: размывать нечего, свой спокойный фон
        PushFade(.9f);
        Image(plain, area, 0);
        PopFade();
    } else {
        BlurredCover(cover ? cover : HeroBitmap(), area, .75f);
    }
    // Караоке: фон сцены проявляется поверх
    const float stage = Hover(880002, in.karaoke);
    if (auto* club = stage > .01f ? PictureBitmap(KaraokeBackdrop) : nullptr) {
        // Зеркально: микрофон на картине слева спрятался бы за обложкой, справа ему свободно
        D2D1_MATRIX_3X2_F previous;
        dc_->GetTransform(&previous);
        dc_->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1, D2D1::Point2F(area.CenterX(), area.CenterY())) * previous);
        PushFade(stage * .85f);
        Image(club, area, 0);
        PopFade();
        dc_->SetTransform(previous);
    }
    Aurora(area, in);
    auroraMood_ = false;
    D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(p.bg.r, p.bg.g, p.bg.b, .35f)},
                                  {1, D2D1::ColorF(p.bg.r, p.bg.g, p.bg.b, .92f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> scrim;
    collection = gradients_.Get(dc_, stops, 2);
    if (collection)
        dc_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, bottom)), collection.Get(),
            &scrim);
    if (scrim) dc_->FillRectangle(D2D1::RectF(0, 0, w, bottom), scrim.Get());
    if (stage > .01f) PaintStage(area, in, stage);

    // Обложка слева «дышит» с басом; при открытии поднимается снизу
    const float rise = (1 - amount) * 40;
    float side = std::min(bottom * .56f, w * .34f);
    float artCx = w * .09f + side / 2, artCy = bottom / 2 - 24;
    if (HasFrame(in.coverFrame)) {
        // Рамка шире обложки: обложка уменьшается так, чтобы вся рамка уместилась над волной,
        // и по центру стоит рамка, а не обложка - у полароида и кассеты окно смещено
        const auto& f = WindowOfFrame(in.coverFrame - 1);
        const float minSide = std::max(.2f, std::min(f.r - f.l, f.b - f.t));
        side = std::min(side, (bottom - 112) * minSide / .97f);
        const float size = side * .97f / minSide;
        artCy = (bottom - 88) / 2 + size * ((f.t + f.b) / 2 - .5f);
        artCx = std::max(w * .09f + side / 2, 14 + size * (f.l + f.r) / 2);
    }
    const float breathe = 1 + (in.ambient ? in.energy * .02f + in.beat * .025f : 0);
    const float s = side * breathe;
    const view::Rect art{artCx - s / 2, artCy - s / 2, s, s};
    // Обложка вылетает из нижней панели дугой - сначала вверх, потом в сторону - и с лёгким
    // перелётом занимает своё место; рисуется позже, поверх панели
    const view::Rect from{22, bottom + 26, 56, 56};
    auto flight = [&](float a) {
        float grow = a;
        if (nowOpen_ && !reducedMotion_) {
            const float u = std::clamp((float)((clock_ - nowToggledAt_) / .55), 0.f, 1.f);
            if (u < 1 && a >= amount) {
                // Рост догоняет полёт и на подлёте чуть перелетает размер
                const float e = u * u - 1;
                grow = 1 + 2.4f * e * e * e + 1.4f * e * e;
            }
        }
        const float size = from.w + (art.w - from.w) * grow;
        const float cx = std::max(size / 2 + 14, from.CenterX() + (art.CenterX() - from.CenterX()) * std::pow(a, 1.6f));
        const float cy = from.CenterY() + (art.CenterY() - from.CenterY()) * (1 - std::pow(1 - a, 1.6f));
        return view::Rect{cx - size / 2, cy - size / 2, size, size};
    };
    morphArt_ = flight(amount);
    morphGhost_[0] = morphGhost_[1] = {};
    if (amount > .04f && amount < .96f && !reducedMotion_) {
        morphGhost_[0] = flight(std::max(0.f, amount - .16f));
        morphGhost_[1] = flight(std::max(0.f, amount - .08f));
    }
    morphGlow_ = (.9f + in.energy) * amount;
    if (amount > .5f) hits_.push_back({art, Toggle});

    // Рамка шире обложки (у винилового кольца - на треть): текст начинается за её краем
    const float overhang =
        HasFrame(in.coverFrame) ? std::max(0.f, FrameDest(art, in.coverFrame).right - art.Right()) : 0.f;
    const float x = artCx + side / 2 + overhang + 56;
    // Широкое окно: справа карточка «Дальше»; в караоке там сцена, и карточка уступает ей место
    constexpr float kNextW = 290, kNextGap = 44;
    const int nextFirst = in.playback.index + 1;
    const bool nextFits = w - x - 70 >= kNextW + kNextGap + 420 && nextFirst < (int)in.queue.size();
    const float tw = std::max(120.f, w - x - 70 - (nextFits ? kNextW + kNextGap : 0));
    if (nextFits && (1 - stage) * amount > .01f) {
        const int rows = std::min(3, (int)in.queue.size() - nextFirst);
        const float nextH = 52 + rows * 66;
        PaintNextUp(in, {w - 70 - kNextW + (1 - amount) * 46, std::max(56.f, artCy - nextH / 2), kNextW, nextH}, (1 - stage) * amount);
    }
    float y = (bottom - side) / 2 - 14 + rise * 1.3f;
    Text(L"СЕЙЧАС ИГРАЕТ", {x, y, tw, 18}, 10, p.accent, 700);
    Text(in.title.empty() ? L"Тишина" : Scramble(in.title, trackChangedAt_), {x, y + 22, tw, 48}, 36, p.white, 700);
    Text(in.artist.empty() ? L"Неизвестный исполнитель" : in.artist, {x, y + 74, tw, 26}, 16,
         MixColor(p.white, p.muted, .4f), 500);
    // Караоке: голос тише, текст крупнее, звучащая строка заливается слева направо
    {
        const view::Rect k{x, y + 108, 132, 30};
        const bool over = k.Contains(mouse_.x, mouse_.y);
        if (in.karaoke) Glow(k, p.accent, 15, 8, .8f + in.beat);
        Box(k, in.karaoke ? p.accent : MixColor(p.card, p.white, over ? .12f : .06f), 15);
        Path({k.x + 12, k.y + 7, 16, 16},
             "M12 2A3 3 0 0 0 9 5V11A3 3 0 0 0 15 11V5A3 3 0 0 0 12 2ZM19 10V11A7 7 0 0 1 5 11V10M12 18V22M8 22H16",
             in.karaoke ? p.onAccent : p.white);
        Text(in.karaoke ? L"Караоке вкл." : L"Караоке", {k.x + 34, k.y + 6, k.w - 40, 20}, 12,
             in.karaoke ? p.onAccent : p.white, 600);
        hits_.push_back({k, Karaoke});
    }
    y += 122 + 44;

    // Текст песни: звучащая строка крупнее и в акценте, остальные гаснут по мере удаления
    const float lyricsH = std::max(80.f, art.Bottom() - y);
    const view::Rect lyrics{x, y, tw, lyricsH};
    dc_->PushAxisAlignedClip(D2D1::RectF(lyrics.x - 4, lyrics.y, lyrics.Right(), lyrics.Bottom()),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if ((in.lyricsState == 2 || in.lyricsState == 3) && !in.lyrics.empty()) {
        const bool synced = in.lyricsState == 2 && in.lyricTimes.size() == in.lyrics.size();
        int line = 0;
        if (synced)
            for (size_t i = 0; i < in.lyricTimes.size(); ++i)
                if (in.lyricTimes[i] <= in.playback.position + .25) line = (int)i;
        const float target = synced ? (float)line : 0;
        lyricPos_ += (target - lyricPos_) * (reducedMotion_ ? 1 : 1 - std::exp(-frameDelta_ * 8));
        if (std::abs(target - lyricPos_) > .01f)
            needsFrames_ = true;
        else
            lyricPos_ = target;
        // В караоке строки крупнее, а звучащая заливается акцентом по мере пения
        // Когда колонка широкая, текст - главное на экране и набирается крупнее
        const bool roomy = tw >= 520;
        const float kLine = in.karaoke ? (roomy ? 64.f : 54.f) : (roomy ? 48.f : 38.f);
        const float bigSize = in.karaoke ? (roomy ? 42.f : 32.f) : (roomy ? 31.f : 22.f);
        const float smallSize = in.karaoke ? (roomy ? 24.f : 20.f) : (roomy ? 20.f : 17.f);
        // Длинная строка не обрезается многоточием, а ужимается по ширине колонки
        auto fitted = [&](const std::wstring& phrase, float size, float weight) {
            TextSpec spec;
            spec.size = size;
            spec.weight = weight;
            const float width = text_->Measure(phrase, spec);
            return width > lyrics.w - 4 ? std::max(12.f, size * (lyrics.w - 4) / width) : size;
        };
        const float center = synced ? lyrics.y + lyricsH * .38f : lyrics.y;
        for (int i = 0; i < (int)in.lyrics.size(); ++i) {
            const float ly = center + (i - lyricPos_) * kLine;
            if (ly < lyrics.y - kLine || ly > lyrics.Bottom()) continue;
            const float distance = std::abs(i - lyricPos_);
            const bool now = synced && i == line;
            const std::wstring& words = in.lyrics[(size_t)i].empty() ? std::wstring(L"♪") : in.lyrics[(size_t)i];
            const view::Rect row{lyrics.x, ly, lyrics.w, kLine};
            if (now && in.karaoke) {
                // Заливка: доля времени от начала строки до следующей
                const double lineStart = in.lyricTimes[(size_t)i];
                const double to = (size_t)i + 1 < in.lyricTimes.size() ? in.lyricTimes[(size_t)i + 1] : lineStart + 4;
                const float fill =
                    std::clamp((float)((in.playback.position - lineStart) / std::max(.5, to - lineStart)), 0.f, 1.f);
                const float karaokeSize = fitted(words, bigSize, 800);
                TextSpec spec;
                spec.size = karaokeSize;
                spec.weight = 800;
                const float width = std::min(row.w, text_->Measure(words, spec));
                Text(words, row, karaokeSize, D2D1::ColorF(p.white.r, p.white.g, p.white.b, .9f), 800);
                dc_->PushAxisAlignedClip(D2D1::RectF(row.x, row.y, row.x + width * fill, row.Bottom()),
                                         D2D1_ANTIALIAS_MODE_ALIASED);
                Text(words, row, karaokeSize, p.accent, 800);
                dc_->PopAxisAlignedClip();
                if (in.playback.playing) needsFrames_ = true;
            } else {
                const auto color =
                    now ? p.accent
                        : D2D1::ColorF(p.white.r, p.white.g, p.white.b, std::max(.18f, .85f - distance * .2f));
                Text(words, row, fitted(words, now ? bigSize : smallSize, now ? 700.f : 500.f), color, now ? 700.f : 500.f);
            }
            if (synced && ly >= lyrics.y && ly + kLine <= lyrics.Bottom()) hits_.push_back({row, SeekLyric, i});
        }
    } else {
        const wchar_t* message = in.lyricsState == 1   ? L"Ищу текст песни…"
                                 : in.lyricsState == 5 ? L"Инструментальная композиция - просто слушай"
                                 : in.lyricsState == 6 ? L"Текст появится, когда будет интернет"
                                 : in.current.empty()  ? L"Выберите трек в коллекции"
                                                       : L"Текста для этой песни нет";
        // Иллюстрация состояния над сообщением, если хватает места
        const Picture state = in.current.empty() || in.lyricsState == 1 ? kPictures
                              : in.lyricsState == 5                     ? EmptyInstrumental
                              : in.lyricsState == 6                     ? EmptyOffline
                                                                        : EmptyLyrics;
        float messageY = lyrics.y + 6;
        if (auto* picture = state != kPictures && lyricsH > 170 ? PictureBitmap(state) : nullptr) {
            const float pic = std::min(170.f, lyricsH - 50);
            Glow({lyrics.x + pic * .25f, lyrics.y + pic * .25f, pic * .5f, pic * .5f}, p.accent, pic * .25f, 14, .5f);
            TintedImage(picture, {lyrics.x, lyrics.y, pic, pic}, 0, .5f, .5f, 1);
            messageY = lyrics.y + pic + 6;
        }
        Text(message, {lyrics.x, messageY, lyrics.w, 30}, 17, MixColor(p.white, p.muted, .5f), 500);
    }
    dc_->PopAxisAlignedClip();

    // Волна по низу: частые тонкие штрихи вместо крупных капель. Сыгранная часть трека яркая,
    // остаток приглушён, поэтому волна заодно показывает, где мы сейчас, и не спорит с текстом
    if (!in.spectrum.empty()) {
        const int source = (int)in.spectrum.size();
        const float left = 60, span = w - 120, barW = 3, base = bottom - 34;
        const int n = std::max(2, (int)(span / 9));
        const float played = in.playback.duration > 0 ? std::clamp((float)(in.playback.position / in.playback.duration), 0.f, 1.f) : 0;
        for (int i = 0; i < n; ++i) {
            const float f = (float)i / (n - 1);
            const float at = f * (source - 1);
            const int low = std::min((int)at, source - 2 < 0 ? 0 : source - 2);
            const int high = std::min(low + 1, source - 1);
            const float mix = Smooth(at - low);
            const float v = std::clamp(in.spectrum[(size_t)low] * (1 - mix) + in.spectrum[(size_t)high] * mix, 0.f, 1.f);
            const float h = 3 + v * 46;
            const float bx = left + f * (span - barW);
            const bool past = f <= played;
            const float alpha = past ? std::min(1.f, .72f + v * .2f + in.beat * .12f) : .2f + v * .16f;
            auto color = MixColor(p.accent2, p.accent, f);
            color.a = alpha;
            brush_->SetColor(color);
            dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, base - h, bx + barW, base), barW / 2, barW / 2), brush_.Get());
            if (past) {
                color.a = .1f;
                brush_->SetColor(color);
                dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, base + 3, bx + barW, base + 3 + h * .22f), barW / 2, barW / 2), brush_.Get());
            }
        }
        if (in.playback.playing) needsFrames_ = true;
    }

    // Свернуть - стрелка вниз справа сверху; окно по-прежнему можно свернуть и закрыть
    const view::Rect collapse{w - 176, 7, 40, 30};
    if (collapse.Contains(mouse_.x, mouse_.y)) Box(collapse, p.card, 10);
    Path({collapse.x + 10, collapse.y + 5, 20, 20}, "M6 9L12 15L18 9", p.white);
    hits_.push_back({collapse, NowPlaying});
    PopFade();
    SetHomePalette(in.colors);
}
}  // namespace ui
