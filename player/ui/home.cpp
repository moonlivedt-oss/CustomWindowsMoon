#include "ui/home.h"
#include "core/tags.h"
#include <cmath>
#include <d2d1helper.h>

namespace ui {
namespace {
using view::Rect;
const auto bg = D2D1::ColorF(0x0b0d12), surface = D2D1::ColorF(0x11141c), card = D2D1::ColorF(0x191d28);
const auto white = D2D1::ColorF(0xf5f1f2), muted = D2D1::ColorF(0x8e96a8), accent = D2D1::ColorF(0xeb5266);
D2D1_RECT_F Native(Rect r) { return D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()); }
uint64_t Key(const std::wstring& s) { return std::hash<std::wstring>{}(s); }
std::wstring Time(double seconds) {
    int n = std::max(0, (int)seconds);
    return std::to_wstring(n / 60) + L":" + (n % 60 < 10 ? L"0" : L"") + std::to_wstring(n % 60);
}
std::wstring Count(size_t count) {
    const auto last = count % 10, teen = count % 100;
    const wchar_t* noun = teen >= 11 && teen <= 14 ? L"треков"
                          : last == 1              ? L"трек"
                          : last >= 2 && last <= 4 ? L"трека"
                                                   : L"треков";
    return std::to_wstring(count) + L" " + noun;
}
}  // namespace
Home::~Home() { Shutdown(); }
void Home::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        pending_.clear();
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}
void Home::Init(HWND hwnd, Renderer* renderer, TextSystem* text) {
    hwnd_ = hwnd;
    renderer_ = renderer;
    text_ = text;
    art_.Init(renderer->Wic());
    worker_ = std::thread([this] { ArtLoop(); });
}
void Home::ArtLoop() {
    for (;;) {
        view::HomeTrack t;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
            if (stop_) return;
            t = std::move(pending_.front());
            pending_.pop_front();
        }
        pm::TagInfo tags;
        pm::ReadTags(t.path, tags, true);
        {
            std::lock_guard lock(mutex_);
            ready_.push_back({t.path.wstring(), std::move(tags.cover), tags.title});
        }
        PostMessageW(hwnd_, kArtReady, 0, 0);
    }
}
void Home::RequestArt(const view::HomeTrack& t) {
    const auto key = t.path.wstring();
    std::lock_guard lock(mutex_);
    if (requested_.count(key) || pending_.size() >= 48) return;
    // Bound decoded images and failed lookups alike when browsing large libraries.
    if (requested_.size() >= 128 && pending_.empty() && ready_.empty()) {
        requested_.clear();
        art_.Keep({});
    }
    requested_.insert(key);
    pending_.push_back(t);
    cv_.notify_one();
}
void Home::DrainArt() {
    std::vector<Artwork> ready;
    {
        std::lock_guard lock(mutex_);
        ready.swap(ready_);
    }
    for (auto& a : ready) {
        theme::CoverPalette palette;
        art_.Load(Key(a.key), a.bytes, a.title, palette);
    }
}
void Home::Box(Rect r, D2D1_COLOR_F c, float radius) {
    brush_->SetColor(c);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(Native(r), radius, radius), brush_.Get());
}
void Home::Text(const std::wstring& s, Rect r, float size, D2D1_COLOR_F c, float weight) {
    TextSpec spec;
    spec.size = size;
    spec.weight = weight;
    spec.wrap = r.h > size * 2;
    auto layout = text_->Layout(s, spec, r.w, r.h);
    if (layout) {
        brush_->SetColor(c);
        dc_->DrawTextLayout(D2D1::Point2F(r.x, r.y), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}
void Home::Button(Rect r, const std::wstring& s, Action a, int index, bool primary) {
    const bool hover = r.Contains(mouse_.x, mouse_.y);
    Box(r, primary ? accent : hover ? D2D1::ColorF(0x292e3c) : card, 9);
    Text(s, {r.x + 14, r.y + 9, r.w - 24, r.h - 14}, 13, white, 600);
    hits_.push_back({r, a, index});
}
void Home::Glyph(Rect r, int kind, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    float x = r.CenterX(), y = r.CenterY(), s = r.w * .25f;
    if (kind == 1) {
        dc_->FillRectangle(D2D1::RectF(x - s, y - s, x - s / 3, y + s), brush_.Get());
        dc_->FillRectangle(D2D1::RectF(x + s / 3, y - s, x + s, y + s), brush_.Get());
        return;
    }
    // Explicit geometry avoids font-dependent play symbols.
    ComPtr<ID2D1PathGeometry> path;
    renderer_->Factory()->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(x - s * .7f, y - s), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x + s, y));
    sink->AddLine(D2D1::Point2F(x - s * .7f, y + s));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    dc_->FillGeometry(path.Get(), brush_.Get());
}
void Home::Art(const view::HomeTrack& t, Rect r) {
    RequestArt(t);
    auto bitmap = art_.Get(Key(t.path.wstring()), dc_, renderer_->DeviceGeneration());
    Box(r, card, 9);
    if (bitmap) {
        auto size = bitmap->GetSize();
        float side = std::min(size.width, size.height);
        auto src = D2D1::RectF((size.width - side) / 2, (size.height - side) / 2, (size.width + side) / 2,
                               (size.height + side) / 2);
        dc_->DrawBitmap(bitmap, Native(r), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    } else {
        auto hash = Key(t.path.wstring());
        const D2D1_COLOR_F color = D2D1::ColorF(.24f + float(hash % 40) / 160, .12f + float((hash >> 8) % 40) / 200,
                                                .22f + float((hash >> 16) % 40) / 160);
        Box(r, color, 9);
        dc_->PushAxisAlignedClip(Native(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, .12f));
        for (int i = 0; i < 5; i++)
            dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(r.CenterX(), r.CenterY()), r.w * (.18f + i * .09f),
                                           r.w * (.18f + i * .09f)),
                             brush_.Get(), 1.5f);
        dc_->PopAxisAlignedClip();
        Box({r.CenterX() - r.w * .17f, r.CenterY() - r.w * .17f, r.w * .34f, r.w * .34f}, D2D1::ColorF(0x11141c),
            r.w * .17f);
        if (r.w > 70) Text(L"♪", {r.CenterX() - 15, r.CenterY() - 23, 35, 45}, 32, white);
    }
}
Home::Hit Home::At(view::Point p) const {
    for (auto i = hits_.rbegin(); i != hits_.rend(); ++i)
        if (i->rect.Contains(p.x, p.y)) return *i;
    return {{}, None, -1};
}
float Home::Fraction(view::Point p, Action a) const {
    for (auto& h : hits_)
        if (h.action == a) return std::clamp((p.x - h.rect.x) / h.rect.w, 0.f, 1.f);
    return 0;
}
void Home::Paint(ID2D1DeviceContext* dc, const Input& in) {
    dc_ = dc;
    if (generation_ != renderer_->DeviceGeneration() || !brush_) {
        brush_.Reset();
        dc->CreateSolidColorBrush(white, &brush_);
        generation_ = renderer_->DeviceGeneration();
    }
    if (!brush_) return;
    DrainArt();
    hits_.clear();
    dc->SetTransform(D2D1::IdentityMatrix());
    dc->Clear(bg);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const float w = in.size.w, h = in.size.h, side = 204, bottom = h - 106;
    const bool right = w >= 1100;
    const float end = right ? w - 258 : w - 26, mainX = side + 28, mainW = end - mainX;
    Box({0, 0, side, bottom}, surface, 0);
    // A small broken orbit gives the music application a family resemblance to RadialMenu.
    brush_->SetColor(accent);
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(35, 35), 13, 13), brush_.Get(), 3);
    Box({28, 19, 20, 10}, surface, 0);
    Glyph({23, 23, 24, 24}, 0, white);
    Text(L"PlayerMoon", {58, 21, 142, 25}, 18, white, 700);
    Text(L"RADIALMENU · MUSIC", {24, 65, 174, 20}, 10, muted, 600);
    const wchar_t* labels[] = {L"Главная", L"Библиотека", L"Любимое", L"Очередь"};
    const wchar_t* symbols[] = {L"⌂", L"♫", L"♡", L"≡"};
    for (int i = 0; i < 4; ++i) {
        Rect r{14, 110.f + i * 48, side - 28, 40};
        bool active = model.page == i;
        if (active || r.Contains(mouse_.x, mouse_.y)) Box(r, active ? D2D1::ColorF(0x352029) : card, 9);
        if (active) Box({14, r.y + 10, 3, 20}, accent, 2);
        Text(symbols[i], {30, r.y + 6, 25, 29}, 21, active ? accent : muted);
        Text(labels[i], {65, r.y + 10, 120, 25}, 14, active ? white : muted, active ? 600 : 400);
        hits_.push_back({r, None, 10000 + i});
    }
    Text(L"ВАША МУЗЫКА", {25, 329, 155, 22}, 10, muted, 600);
    Text(Count(model.tracks.size()) + L" в библиотеке", {25, 357, 165, 22}, 12, white);
    if (bottom > 570)
        Text(in.scanning ? L"Обновляю библиотеку…" : L"Локально. Без лишнего.", {25, 382, 165, 40}, 11, muted);
    Button({16, bottom - 146, 172, 38}, L"+  Добавить музыку", OpenFiles);
    Button({16, bottom - 100, 172, 38}, L"Компактный плеер", Compact);
    Button({16, bottom - 54, 172, 38}, L"Настройки", Menu);
    Text(L"МУЗЫКА В ВАШЕМ РИТМЕ", {mainX, 24, mainW, 18}, 10, muted, 600);
    Button({w - 126, 7, 36, 30}, L"−", Minimize);
    Button({w - 86, 7, 36, 30}, L"□", Maximize);
    Button({w - 46, 7, 36, 30}, L"×", Close);
    // The real Win32 search edit sits on this surface.
    Box({mainX, 57, mainW, 42}, surface, 10);
    // The accessible native edit handles Unicode, selection, IME and clipboard.
    // DirectComposition is above child HWND painting, so draw its text on this surface.
    const Rect search{mainX + 15, 68, mainW - 30, 25};
    if (model.query.empty())
        Text(L"Поиск по названию, исполнителю или альбому", search, 14, muted);
    else {
        TextSpec spec;
        spec.size = 14;
        const auto a = std::min<size_t>(in.selectionStart, model.query.size());
        const auto b = std::min<size_t>(in.selectionEnd, model.query.size());
        float x = text_->Measure(model.query.substr(0, a), spec);
        if (in.searchFocused && a != b) {
            float width = text_->Measure(model.query.substr(a, b - a), spec);
            Box({search.x + x, search.y, std::min(width, std::max(0.f, search.w - x)), 21}, D2D1::ColorF(0x713b4b), 1);
        }
        Text(model.query, search, 14, white);
        if (in.searchFocused && a == b && x < search.w) Box({search.x + x, search.y + 1, 1, 19}, accent, 0);
    }
    if (in.searchFocused && model.query.empty()) Box({search.x, search.y + 1, 1, 19}, accent, 0);
    const bool hero = model.page == view::HomeModel::Home && model.query.empty();
    float top = 119;
    if (hero) {
        Box({mainX, top, mainW, 158}, D2D1::ColorF(0x271a26), 15);
        dc->PushAxisAlignedClip(D2D1::RectF(mainX, top, end, top + 158), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (int i = 0; i < 6; ++i) {
            brush_->SetColor(D2D1::ColorF(.92f, .32f, .40f, .10f + i * .025f));
            float radius = 48.f + i * 19;
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(end - 58, top + 83), radius, radius), brush_.Get(),
                            i == 3 ? 9.f : 2.f);
        }
        dc->PopAxisAlignedClip();
        Text(L"ВАША ЛИЧНАЯ ОРБИТА", {mainX + 22, top + 17, mainW - 44, 18}, 10, accent, 600);
        Text(L"Включить свой ритм", {mainX + 22, top + 39, mainW - 40, 40}, 28, white, 700);
        Text(L"Знакомые треки. Новая последовательность.", {mainX + 22, top + 82, mainW - 44, 23}, 12, muted);
        Button({mainX + 22, top + 112, 155, 33}, L"▶  Моя волна", Wave, -1, true);
        Button({mainX + 188, top + 112, 147, 33}, L"Открыть папку", OpenFolder);
        top += 183;
    }
    Text(!model.query.empty() ? L"Результаты поиска"
         : hero               ? L"Вернуться к музыке"
                              : labels[(int)model.page],
         {mainX, top, mainW, 34}, 23, white, 650);
    if (renderedRevision_ != revision_ || renderedPage_ != (int)model.page || renderedQuery_ != model.query) {
        displayed_.clear();
        if (model.page == view::HomeModel::Queue) {
            for (const auto& t : in.queue)
                if (model.Match(t)) displayed_.push_back(t);
        } else
            for (auto index : model.Visible()) displayed_.push_back(model.tracks[index]);
        renderedRevision_ = revision_;
        renderedPage_ = (int)model.page;
        renderedQuery_ = model.query;
    }
    Text(Count(displayed_.size()), {mainX, top + 36, mainW, 22}, 11, muted);
    top += 67;
    const int columns = std::max(2, (int)((mainW + 16) / 156));
    const float tile = (mainW - (columns - 1) * 16) / columns, row = tile + 69;
    const float available = std::max(0.f, bottom - 18 - top);
    const int rowCount = ((int)displayed_.size() + columns - 1) / columns;
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, rowCount * row - available));
    if (displayed_.empty()) {
        Text(model.query.empty() ? (model.page == view::HomeModel::Favorites ? L"Здесь будет музыка, которую вы любите"
                                                                             : L"Ваша музыка начинается здесь")
                                 : L"Ничего не найдено",
             {mainX, top + 24, mainW, 38}, 20, white, 600);
        Text(model.query.empty() ? L"Откройте файлы или перетащите папку в окно."
                                 : L"Попробуйте название трека, исполнителя или альбома.",
             {mainX, top + 69, mainW, 48}, 13, muted);
        if (model.query.empty()) Button({mainX, top + 125, 168, 39}, L"Добавить музыку", OpenFiles, -1, true);
    }
    dc->PushAxisAlignedClip(D2D1::RectF(mainX, top, end, bottom - 16), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    int first = std::max(0, (int)(scroll_ / row)) * columns;
    int last = std::min((int)displayed_.size(), first + ((int)(available / row) + 2) * columns);
    for (int i = first; i < last; i++) {
        float x = mainX + (i % columns) * (tile + 16), y = top + (i / columns) * row - scroll_;
        Rect r{x, y, tile, tile};
        const auto& t = displayed_[i];
        Art(t, r);
        Rect hit{x, std::max(top, y), tile, std::max(0.f, std::min(bottom - 16, y + tile + 58) - std::max(top, y))};
        if (hit.h > 0) hits_.push_back({hit, PlayTrack, i});
        Text(t.title, {x, y + tile + 10, tile, 22}, 13, white, 600);
        Text(t.artist.empty() ? L"Неизвестный исполнитель" : t.artist, {x, y + tile + 33, tile, 20}, 11, muted);
        const bool favorite = model.favorites.count(view::HomePathKey(t.path));
        {
            Rect heart{x + tile - 34, y + 6, 28, 28};
            Box(heart, D2D1::ColorF(0x11141c), 14);
            Text(favorite ? L"♥" : L"♡", {heart.x + 6, heart.y + 3, 24, 24}, 17, favorite ? accent : white);
            if (heart.y >= top && heart.Bottom() < bottom - 16) hits_.push_back({heart, Favorite, i});
        }
    }
    dc->PopAxisAlignedClip();
    if (rowCount * row > available && available > 0) {
        float thumb = std::max(24.f, available * available / (rowCount * row));
        float y = top + (available - thumb) * (scroll_ / (rowCount * row - available));
        Box({end + 9, y, 3, thumb}, D2D1::ColorF(0x444a5b), 2);
    }
    if (right) {
        const float x = w - 235;
        Box({x - 13, 49, 247, bottom - 49}, surface, 0);
        Text(L"Далее в очереди", {x, 65, 218, 26}, 16, white, 600);
        Text(Count(in.queue.size()), {x, 96, 218, 20}, 11, muted);
        int start = std::max(0, in.playback.index + 1);
        int shown = 0;
        for (int i = start; i < (int)in.queue.size() && 121 + shown * 67 < bottom - 56; ++i, ++shown) {
            float y = 125.f + shown * 67;
            auto& t = in.queue[i];
            Rect r{x - 4, y - 5, 223, 61};
            if (r.Contains(mouse_.x, mouse_.y)) Box(r, card, 8);
            Art(t, {x, y + 1, 45, 45});
            Text(t.title, {x + 57, y + 4, 157, 22}, 12, white, 600);
            Text(t.artist.empty() ? L"Локальный трек" : t.artist, {x + 57, y + 28, 157, 20}, 10, muted);
            hits_.push_back({r, PlayTrack, -i - 2});
        }
        if (!shown)
            Text(L"Выберите музыку из библиотеки. Здесь появятся следующие треки.", {x, 140, 210, 100}, 12, muted);
    }
    // The transport never scrolls with the collection.
    Box({0, bottom, w, 106}, D2D1::ColorF(0x141720), 0);
    Box({0, bottom, w, 1}, D2D1::ColorF(0x292e3b), 0);
    view::HomeTrack current;
    current.path = in.current;
    current.title = in.title;
    current.artist = in.artist;
    if (!current.path.empty()) Art(current, {20, bottom + 22, 60, 60});
    Text(in.title.empty() ? L"Выберите трек" : in.title, {94, bottom + 28, w * .27f, 24}, 14, white, 600);
    Text(in.artist.empty() ? L"PlayerMoon · RadialMenu" : in.artist, {94, bottom + 54, w * .27f, 22}, 11, muted);
    const float center = w * .56f;
    Button({center - 130, bottom + 13, 38, 34}, L"⇄", Shuffle, -1, in.playback.shuffle);
    Button({center - 79, bottom + 13, 40, 34}, L"‹", Previous);
    Rect play{center - 22, bottom + 8, 44, 44};
    Box(play, accent, 22);
    Glyph(play, in.playback.playing ? 1 : 0, white);
    hits_.push_back({play, Toggle});
    Button({center + 39, bottom + 13, 40, 34}, L"›", Next);
    Button({center + 92, bottom + 13, 38, 34}, in.playback.repeat == pm::Repeat::One ? L"1" : L"↻", Repeat, -1,
           in.playback.repeat != pm::Repeat::Off);
    const float line = std::min(410.f, w * .36f), lx = center - line / 2;
    Rect seek{lx, bottom + 66, line, 16};
    Box({lx, bottom + 72, line, 3}, D2D1::ColorF(0x3c4353), 2);
    float position =
        in.playback.duration > 0 ? std::clamp((float)(in.playback.position / in.playback.duration), 0.f, 1.f) : 0;
    Box({lx, bottom + 72, line * position, 3}, accent, 2);
    hits_.push_back({seek, Seek});
    Text(Time(in.playback.position), {lx - 45, bottom + 65, 40, 21}, 10, muted);
    Text(Time(in.playback.duration), {lx + line + 12, bottom + 65, 45, 21}, 10, muted);
    Text(L"♪", {w - 143, bottom + 37, 28, 25}, 19, muted);
    Rect volume{w - 111, bottom + 36, 86, 24};
    Box({volume.x, volume.y + 11, volume.w, 3}, D2D1::ColorF(0x3c4353), 2);
    Box({volume.x, volume.y + 11, volume.w * (in.playback.muted ? 0 : in.playback.volume), 3}, accent, 2);
    hits_.push_back({volume, Volume});
}
}  // namespace ui
