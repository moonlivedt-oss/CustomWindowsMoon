// «Итоги» историей на весь экран, как у больших сервисов: пять слайдов с автопереходом,
// полоски прогресса сверху, щелчок справа - дальше, слева - назад. Последний слайд - карточка,
// которую можно сохранить картинкой (App рисует её же в отдельный bitmap через RenderStoryCard).
#include "ui/home.h"
#include "ui/home_colors.h"
#include <cmath>

namespace ui {
namespace {
constexpr int kSlides = 5;
constexpr double kSlideTime = 6.5;
float Rise(double t, double delay) {
    const float u = std::clamp((float)((t - delay) / .5), 0.f, 1.f);
    return 1 - (1 - u) * (1 - u) * (1 - u);
}
}  // namespace

void Home::OpenStory() {
    storyOpen_ = true;
    storySlide_ = 0;
    storySlideAt_ = clock_;
}

void Home::StoryStep(int delta) {
    const int next = storySlide_ + delta;
    if (next < 0) return;
    if (next >= kSlides) return;
    storySlide_ = next;
    storySlideAt_ = clock_;
}

void Home::ShowToast(const std::wstring& text) {
    toast_ = text;
    toastAt_ = clock_;
    spokenToast_ = text;  // App отдаст фразу диктору
}

bool Home::Focusable(const Hit& h) const {
    switch (h.action) {
        case Seek:
        case Volume:
            return false;  // у них свои клавиши: стрелки
        case None:
            return h.index >= 10000 && h.index < 10007;
        case StoryNav:
            return h.rect.w < 320;  // половины экрана «назад/дальше» - не элементы
        case PaletteItem:
            return h.index >= 0;
        default:
            return true;
    }
}

std::wstring Home::Describe(const Hit& h) const {
    if (!h.label.empty()) return h.label;
    const wchar_t* pages[] = {L"Главная", L"Библиотека", L"Любимое",  L"Очередь",
                              L"Плагины", L"Скачивание", L"Настройки"};
    auto track = [&](int i) -> std::wstring {
        if (i >= 0 && i < (int)displayed_.size())
            return displayed_[(size_t)i].title + L", " + displayed_[(size_t)i].artist;
        const int q = -i - 2 - queueStart_;
        if (i <= -2 && q >= 0 && q < (int)queueVisible_.size()) return L"Далее: " + queueVisible_[(size_t)q].title;
        return L"Трек";
    };
    switch (h.action) {
        case None:
            return std::wstring(L"Раздел ") + pages[h.index - 10000];
        case PlayTrack:
            return L"Играть: " + track(h.index);
        case Favorite:
            return L"В любимое: " + track(h.index);
        case Toggle:
            return L"Пауза или воспроизведение";
        case Next:
            return L"Следующий трек";
        case Previous:
            return L"Предыдущий трек";
        case Shuffle:
            return L"Перемешать";
        case Repeat:
            return L"Повтор";
        case Wave:
            return L"Слушать Волну";
        case OpenFiles:
            return L"Добавить музыку";
        case OpenFolder:
            return L"Открыть папку";
        case Compact:
            return L"Компактный плеер";
        case Minimize:
            return L"Свернуть окно";
        case Maximize:
            return L"Развернуть окно";
        case Close:
            return L"Закрыть";
        case NowPlaying:
            return L"Сейчас играет";
        case Karaoke:
            return L"Караоке";
        case Slider:
            return L"Ползунок, стрелки меняют значение";
        default:
            return L"Кнопка";
    }
}

std::wstring Home::MoveFocus(int direction) {
    std::vector<size_t> order;
    for (size_t i = 0; i < hits_.size(); ++i)
        if (Focusable(hits_[i])) order.push_back(i);
    if (order.empty()) return {};
    // Порядок - как на экране: сверху вниз, слева направо
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto& ra = hits_[a].rect;
        const auto& rb = hits_[b].rect;
        if (std::abs(ra.y - rb.y) > 8) return ra.y < rb.y;
        return ra.x < rb.x;
    });
    int at = -1;
    for (size_t k = 0; k < order.size(); ++k)
        if (focusActive_ && hits_[order[k]].action == focusAction_ && hits_[order[k]].index == focusIndex_) at = (int)k;
    const int next =
        at < 0 ? (direction > 0 ? 0 : (int)order.size() - 1) : (at + direction + (int)order.size()) % (int)order.size();
    const auto& h = hits_[order[(size_t)next]];
    focusActive_ = true;
    focusAction_ = h.action;
    focusIndex_ = h.index;
    return Describe(h);
}

bool Home::FocusedPoint(view::Point& center) const {
    if (!focusActive_) return false;
    for (const auto& h : hits_)
        if (h.action == focusAction_ && h.index == focusIndex_) {
            center = {h.rect.CenterX(), h.rect.CenterY()};
            return true;
        }
    return false;
}

// Контур фокуса: заметный, в акценте, с мягким свечением - видно и в высоком контрасте
void Home::PaintFocus() {
    if (!focusActive_) return;
    for (const auto& h : hits_) {
        if (h.action != focusAction_ || h.index != focusIndex_) continue;
        const auto& p = HomePalette();
        const view::Rect r{h.rect.x - 3, h.rect.y - 3, h.rect.w + 6, h.rect.h + 6};
        Glow(r, p.accent, 12, 6, 1);
        brush_->SetColor(p.accent);
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), 12, 12), brush_.Get(),
                                  2.5f);
        return;
    }
}

void Home::PaintToast() {
    const float age = (float)(clock_ - toastAt_);
    if (toast_.empty() || age > 3.2f) return;
    const auto& p = HomePalette();
    const float in = std::clamp(age / .25f, 0.f, 1.f), out = std::clamp((3.2f - age) / .4f, 0.f, 1.f);
    TextSpec spec;
    spec.size = 13;
    spec.weight = 600;
    const float w = std::min(560.f, text_->Measure(toast_, spec) + 40);
    const auto size = dc_->GetSize();
    const view::Rect r{size.width / 2 - w / 2, size.height - 160 + (1 - in) * 12, w, 40};
    PushFade(std::min(in, out));
    Glow(r, p.accent, 20, 10, .6f);
    Box(r, MixColor(p.card, p.accent, .25f), 20);
    CenterText(toast_, {r.x, r.y + 10, r.w, 20}, 13, p.white, 600);
    PopFade();
    needsFrames_ = true;
}

// Карточка итогов: тот же рисунок на последнем слайде и в сохранённой картинке
void Home::PaintStoryCard(view::Rect r, const Input::Story& s) {
    const auto& p = HomePalette();
    Glow(r, p.accent, 28, 24, 1.1f);
    D2D1_GRADIENT_STOP stops[] = {
        {0, MixColor(p.card, p.accent, .32f)}, {.55f, MixColor(p.surface, p.accent, .08f)}, {1, p.bg}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> fill;
    collection = gradients_.Get(dc_, stops, 3);
    if (collection)
        dc_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.Right(), r.Bottom())),
            collection.Get(), &fill);
    if (fill)
        dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), 28, 28), fill.Get());
    if (auto* picture = PictureBitmap(s.background == 1 ? RecapRecords : RecapNight)) {
        // Фон-картина в оттенке акцента и вуаль сверху вниз: текст читается на любом фоне
        TintedImage(picture, r, 28, .5f, .5f, 1);
        D2D1_GRADIENT_STOP veil[] = {
            {0, D2D1::ColorF(0, 0, 0, .15f)}, {.45f, D2D1::ColorF(0, 0, 0, .45f)}, {1, D2D1::ColorF(0, 0, 0, .7f)}};
        ComPtr<ID2D1GradientStopCollection> veilStops;
        ComPtr<ID2D1LinearGradientBrush> shade;
        veilStops = gradients_.Get(dc_, veil, 3);
        if (veilStops)
            dc_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.y), D2D1::Point2F(0, r.Bottom())),
                veilStops.Get(), &shade);
        if (shade)
            dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()), 28, 28),
                                      shade.Get());
    }
    const float k = r.w / 420;  // всё в карточке масштабируется от ширины
    const float x = r.x + 32 * k, w = r.w - 64 * k;
    Symbol({x, r.y + 30 * k, 22 * k, 22 * k}, IconId::Moon, p.accent);
    Text(L"PlayerMoon", {x + 30 * k, r.y + 30 * k, w, 24 * k}, 14 * k, p.white, 700);
    TextSpec right;
    right.size = 12 * k;
    right.weight = 600;
    right.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
    if (auto layout = text_->Layout(s.period, right, w, 24 * k)) {
        brush_->SetColor(p.accent);
        dc_->DrawTextLayout(D2D1::Point2F(x, r.y + 33 * k), layout.Get(), brush_.Get());
    }
    float y = r.y + 92 * k;
    Text(L"МУЗЫКИ ЗА ПЕРИОД", {x, y, w, 18 * k}, 10 * k, p.muted, 700);
    Text(s.hours, {x, y + 18 * k, w, 54 * k}, 40 * k, p.white, 800);
    Text(s.playsLine, {x, y + 70 * k, w, 20 * k}, 12 * k, p.muted);
    y += 118 * k;
    auto block = [&](const wchar_t* label, const std::wstring& title, const std::wstring& sub) {
        Box({x, y, w, 1}, MixColor(p.card, p.white, .1f), 0);
        Text(label, {x, y + 14 * k, w, 18 * k}, 10 * k, p.accent, 700);
        Text(title, {x, y + 32 * k, w, 32 * k}, 22 * k, p.white, 700);
        Text(sub, {x, y + 62 * k, w, 20 * k}, 12 * k, p.muted);
        y += 98 * k;
    };
    block(L"ИСПОЛНИТЕЛЬ №1", s.artists.empty() ? L"-" : s.artists[0].title,
          s.artists.empty() ? L"" : s.artists[0].detail);
    block(L"ТРЕК ПЕРИОДА", s.tracks.empty() ? L"-" : s.tracks[0].title, s.tracks.empty() ? L"" : s.tracks[0].sub);
    block(L"ЛЮБИМОЕ ВРЕМЯ", s.topHour >= 0 ? std::to_wstring(s.topHour) + L":00" : L"-",
          L"Дней с музыкой: " + std::to_wstring(s.activeDays));
    // Внизу - сутки столбиками: видно, когда музыка звучит чаще
    const float chartTop = y + 8 * k, chartH = std::max(20.f, r.Bottom() - 34 * k - chartTop);
    const float bar = w / 24;
    for (int h = 0; h < 24; ++h) {
        const float v = std::max(.06f, s.hourShare[h]);
        Box({x + h * bar + bar * .2f, chartTop + chartH * (1 - v), bar * .6f, chartH * v},
            h == s.topHour ? p.accent : MixColor(p.card, p.white, .22f), bar * .3f);
    }
    Text(L"0:00", {x, r.Bottom() - 28 * k, 60 * k, 16 * k}, 9 * k, p.muted);
    CenterText(L"12:00", {x + w / 2 - 30 * k, r.Bottom() - 28 * k, 60 * k, 16 * k}, 9 * k, p.muted);
}

void Home::RenderStoryCard(ID2D1DeviceContext* dc, D2D1_SIZE_F size, const Input& in) {
    dc_ = dc;
    if (!brush_) dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush_);
    SetHomePalette(in.colors);
    dc->Clear(HomePalette().bg);
    Aurora({0, 0, size.width, size.height}, in);
    PaintStoryCard({40, 40, size.width - 80, size.height - 80}, in.story);
}

void Home::PaintStory(const Input& in) {
    if (!storyOpen_) return;
    const auto& p = HomePalette();
    const auto& s = in.story;
    const float w = in.size.w, h = in.size.h;
    const double t = clock_ - storySlideAt_;
    // Автопереход: слайд держится 6,5 с; последний ждёт решения
    if (!reducedMotion_ && storySlide_ < kSlides - 1 && t > kSlideTime && s.seconds > 0) StoryStep(1);
    needsFrames_ = true;
    hits_.clear();
    Box({0, 0, w, h}, p.bg, 0);
    {
        // У слайдов про время, исполнителя, трек и часы - своя картина за текстом, приглушённая
        const Picture backdrop = storySlide_ == 0   ? RecapTotal
                                 : storySlide_ == 1 ? RecapArtist
                                 : storySlide_ == 2 ? RecapTrack
                                 : storySlide_ == 3 ? RecapTime
                                                    : kPictures;
        if (auto* picture = backdrop != kPictures && s.seconds > 0 ? PictureBitmap(backdrop) : nullptr) {
            // Картина вертикальная: встаёт по высоте окна в центре, бока растворяются в фоне,
            // иначе широкое окно оставило бы от неё только тёмную середину
            const float pw = std::min(w, h * .75f), px = (w - pw) / 2;
            D2D1_GRADIENT_STOP edge[] = {{0, D2D1::ColorF(0, 0, 0, 0)},
                                         {.22f, D2D1::ColorF(0, 0, 0, 1)},
                                         {.78f, D2D1::ColorF(0, 0, 0, 1)},
                                         {1, D2D1::ColorF(0, 0, 0, 0)}};
            ComPtr<ID2D1GradientStopCollection> edgeStops;
            ComPtr<ID2D1LinearGradientBrush> mask;
            edgeStops = gradients_.Get(dc_, edge, 4);
            if (edgeStops)
                dc_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(px, 0), D2D1::Point2F(px + pw, 0)),
                    edgeStops.Get(), &mask);
            dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), .8f * Rise(t, 0), mask.Get()),
                           nullptr);
            TintedImage(picture, {px, 0, pw, h}, 0, .5f, .5f, 1.04f - .04f * Rise(t, 0));
            dc_->PopLayer();
        }
    }
    Aurora({0, 0, w, h}, in);
    // Полоски прогресса слайдов
    const float barW = (std::min(w - 80, 720.f) - (kSlides - 1) * 6) / kSlides,
                barX = w / 2 - (barW * kSlides + 6 * (kSlides - 1)) / 2;
    for (int i = 0; i < kSlides; ++i) {
        const view::Rect bar{barX + i * (barW + 6), 26, barW, 4};
        Box(bar, MixColor(p.card, p.white, .15f), 2);
        const float done = i < storySlide_ ? 1 : i > storySlide_ ? 0 : std::clamp((float)(t / kSlideTime), 0.f, 1.f);
        if (i == kSlides - 1 && i == storySlide_)
            Box(bar, p.accent, 2);
        else if (done > 0)
            Box({bar.x, bar.y, bar.w * done, bar.h}, p.white, 2);
    }
    // Переключение периода и закрыть
    const wchar_t* periods[] = {L"Месяц", L"Год", L"Всё время"};
    view::Rect chips[3];
    for (int i = 0; i < 3; ++i) {
        chips[i] = {24.f + i * 104, 48, 96, 30};
        Box(chips[i], MixColor(p.card, p.white, .06f), 15);
        hits_.push_back({chips[i], StoryNav, 100 + i, periods[i]});
    }
    const int period = std::clamp(s.periodIndex, 0, 2);
    const auto chipLiquid = LiquidRect(103, chips[period], period);
    LiquidFill(chipLiquid, p.accent, 15, .5f);
    for (int i = 0; i < 3; ++i) LiquidLabel(periods[i], chips[i], chipLiquid, p.white, p.onAccent, 12, 600);
    const view::Rect close{w - 64, 44, 40, 36};
    Path({close.x + 10, close.y + 8, 20, 20}, "M6 6L18 18M18 6L6 18", p.white);
    // Щелчок по левой трети - назад, по остальному - дальше
    hits_.push_back({{0, 90, w * .33f, h - 90}, StoryNav, -1});
    hits_.push_back({{w * .33f, 90, w * .67f, h - 90}, StoryNav, 1});
    hits_.push_back({close, StoryNav, 202});

    const float cx = w / 2, colW = std::min(w - 120, 760.f), x = cx - colW / 2;
    auto label = [&](const std::wstring& text, float y, double delay) {
        const float e = Rise(t, delay);
        PushFade(e);
        CenterText(text, {x, y + (1 - e) * 14, colW, 22}, 12, p.accent, 700);
        PopFade();
    };
    auto big = [&](const std::wstring& text, float y, float size, double delay, D2D1_COLOR_F color) {
        const float e = Rise(t, delay);
        PushFade(e);
        CenterText(text, {x - 100, y + (1 - e) * 24, colW + 200, size * 1.4f}, size, color, 800);
        PopFade();
    };
    const float mid = h * .42f;
    if (s.seconds <= 0) {
        big(L"Пока нечего считать", mid - 40, 40, .1, p.white);
        label(L"История копится, когда трек звучит дольше 30 секунд", mid + 30, .3);
        return;
    }
    switch (storySlide_) {
        case 0:
            label(L"ИТОГИ · " + s.period, mid - 110, .05);
            big(s.hours, mid - 70, 76, .2, p.white);
            big(L"музыки", mid + 40, 26, .45, MixColor(p.white, p.muted, .4f));
            label(s.playsLine, mid + 110, .7);
            break;
        case 1: {
            label(L"ВАШ ИСПОЛНИТЕЛЬ №1", mid - 150, .05);
            big(s.artists.empty() ? L"-" : s.artists[0].title, mid - 110, 64, .2, p.white);
            if (!s.artists.empty()) label(s.artists[0].detail, mid - 10, .45);
            // Остальная пятёрка - строки с полосой доли
            for (size_t i = 1; i < s.artists.size() && i < 5; ++i) {
                const float e = Rise(t, .6 + i * .12);
                const float ry = mid + 40 + i * 46.f + (1 - e) * 16;
                PushFade(e);
                Text(std::to_wstring(i + 1) + L"   " + s.artists[i].title, {x + 80, ry, colW - 300, 26}, 17, p.white,
                     600);
                const view::Rect track{x + colW - 220, ry + 10, 140, 6};
                Box(track, MixColor(p.card, p.white, .1f), 3);
                Box({track.x, track.y, track.w * s.artists[i].share * e, track.h}, p.accent, 3);
                PopFade();
            }
            break;
        }
        case 2: {
            label(L"ТРЕК ПЕРИОДА", mid - 200, .05);
            const float e = Rise(t, .15);
            const float side = std::min(240.f, h * .3f) * (.9f + .1f * e);
            const view::Rect art{cx - side / 2, mid - 165 + (1 - e) * 30, side, side};
            PushFade(e);
            Glow(art, p.accent, 18, 26, 1.2f + in.beat);
            view::HomeTrack top;
            top.path = s.topTrackPath;
            top.title = s.tracks.empty() ? L"" : s.tracks[0].title;
            Art(top, art);
            PopFade();
            big(s.tracks.empty() ? L"-" : s.tracks[0].title, art.Bottom() + 22, 40, .35, p.white);
            if (!s.tracks.empty()) label(s.tracks[0].sub + L"  ·  " + s.tracks[0].detail, art.Bottom() + 84, .55);
            break;
        }
        case 3: {
            label(L"ВАШЕ ВРЕМЯ ДЛЯ МУЗЫКИ", mid - 260, .05);
            // Часы: 24 луча, длина - доля прослушанного в этот час
            const float r0 = std::min(110.f, h * .14f), e = Rise(t, .15);
            const float ccy = mid - 10;
            for (int hour = 0; hour < 24; ++hour) {
                const float a = (hour / 24.f) * 6.2831853f - 1.5707963f;
                const float len = 10 + s.hourShare[hour] * 70 * e;
                brush_->SetColor(hour == s.topHour ? p.accent : MixColor(p.card, p.white, .35f));
                dc_->DrawLine(D2D1::Point2F(cx + std::cos(a) * r0, ccy + std::sin(a) * r0),
                              D2D1::Point2F(cx + std::cos(a) * (r0 + len), ccy + std::sin(a) * (r0 + len)),
                              brush_.Get(), hour == s.topHour ? 6.f : 4.f, nullptr);
            }
            CenterText(s.topHour >= 0 ? std::to_wstring(s.topHour) + L":00" : L"-", {cx - 100, ccy - 26, 200, 50}, 38,
                       p.white, 800);
            big(L"чаще всего музыка звучит в это время", ccy + r0 + 96, 22, .45, MixColor(p.white, p.muted, .3f));
            label(L"Дней с музыкой: " + std::to_wstring(s.activeDays), ccy + r0 + 140, .65);
            break;
        }
        default: {
            const float e = Rise(t, .05);
            const float cardH = std::min(h - 220, 600.f), cardW = cardH * .7f;
            const view::Rect card{cx - cardW / 2, 100 + (1 - e) * 30, cardW, cardH};
            PushFade(e);
            PaintStoryCard(card, s);
            PopFade();
            // Кнопки под карточкой; щелчки по ним не листают историю
            const float by = card.Bottom() + 24;
            hits_.erase(hits_.begin() + 3, hits_.begin() + 5);
            Button({cx - 390, by, 180, 40}, L"Сохранить картинку", StoryNav, 200, true);
            Button({cx - 190, by, 180, 40}, s.background == 1 ? L"Фон: пластинки" : L"Фон: ночь", StoryNav, 204);
            Button({cx + 10, by, 180, 40}, L"Слушать «Волну»", StoryNav, 201);
            Button({cx + 210, by, 180, 40}, L"Сначала", StoryNav, 203);
            break;
        }
    }
}
}  // namespace ui
