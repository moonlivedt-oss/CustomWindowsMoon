#include "ui/home.h"
#include "core/tags.h"
#include <cmath>
#include <d2d1helper.h>

namespace ui {
namespace {
using view::Rect;
const auto bg = D2D1::ColorF(0x101116), surface = D2D1::ColorF(0x15161d), card = D2D1::ColorF(0x1e2029);
const auto white = D2D1::ColorF(0xf6f3f5), muted = D2D1::ColorF(0x9999aa), accent = D2D1::ColorF(0xed647b);
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
    icons_.Init(renderer->Factory());
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    const auto file = std::filesystem::path(exe).parent_path() / L"assets" / L"art" / L"moon-session.png";
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    auto* wic = renderer->Wic();
    if (SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        ComPtr<IWICBitmap> image;
        if (SUCCEEDED(wic->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &image)))
            heroSource_ = image;
    }
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
    if (r.Empty()) return;
    auto layout = text_->Layout(s, spec, r.w, r.h);
    if (layout) {
        brush_->SetColor(c);
        dc_->DrawTextLayout(D2D1::Point2F(r.x, r.y), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}
void Home::Button(Rect r, const std::wstring& s, Action a, int index, bool primary) {
    const bool hover = r.Contains(mouse_.x, mouse_.y);
    const float amount = Hover((int)a * 100000 + index, hover);
    r.y -= amount * 1.2f;
    Box(r, primary ? accent : D2D1::ColorF(card.r + amount * .06f, card.g + amount * .05f, card.b + amount * .06f), 9);
    TextSpec spec;
    spec.size = 13;
    spec.weight = 600;
    spec.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    auto layout = text_->Layout(s, spec, std::max(1.f, r.w - 20), r.h);
    if (layout) {
        brush_->SetColor(white);
        dc_->DrawTextLayout(D2D1::Point2F(r.x + 10, r.y), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
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
void Home::Symbol(Rect r, IconId id, D2D1_COLOR_F color) {
    const auto& icon = icons_.Get(id);
    D2D1_MATRIX_3X2_F previous;
    dc_->GetTransform(&previous);
    const float scale = r.w / icon.viewBox;
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(r.x, r.y));
    brush_->SetColor(color);
    for (const auto& layer : icon.layers) {
        if (layer.part == IconPart::Mute || layer.part == IconPart::RepeatOne) continue;
        if (layer.solid) dc_->FillGeometry(layer.geometry.Get(), brush_.Get());
        else dc_->DrawGeometry(layer.geometry.Get(), brush_.Get(), 1.7f);
    }
    dc_->SetTransform(previous);
}
void Home::Path(Rect r, const std::string& path, D2D1_COLOR_F color, bool fill) {
    auto& geo = paths_[path];
    if (!geo) geo = ParseSvgPath(renderer_->Factory(), path);
    if (!geo) return;
    D2D1_MATRIX_3X2_F previous;
    dc_->GetTransform(&previous);
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(r.w / 24, r.h / 24) * D2D1::Matrix3x2F::Translation(r.x, r.y));
    brush_->SetColor(color);
    if (fill) dc_->FillGeometry(geo.Get(), brush_.Get());
    else dc_->DrawGeometry(geo.Get(), brush_.Get(), 1.6f);
    dc_->SetTransform(previous);
}
void Home::Image(ID2D1Bitmap1* bitmap, Rect r, float radius, float anchor) {
    if (!bitmap || r.Empty()) return;
    auto size = bitmap->GetSize();
    float sw = size.width, sh = size.height;
    if (sw / sh > r.w / r.h) sw = sh * r.w / r.h;
    else sh = sw * r.h / r.w;
    auto source = D2D1::RectF((size.width - sw) * anchor, (size.height - sh) * .5f,
                              (size.width - sw) * anchor + sw, (size.height + sh) * .5f);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    renderer_->Factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(Native(r), radius, radius), &clip);
    if (clip) dc_->PushLayer(D2D1::LayerParameters(Native(r), clip.Get()), nullptr);
    dc_->DrawBitmap(bitmap, Native(r), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
    if (clip) dc_->PopLayer();
}
void Home::IconButton(Rect r, IconId icon, Action action, bool active) {
    const bool hover = r.Contains(mouse_.x, mouse_.y);
    if (hover || active) Box(r, active ? D2D1::ColorF(0x39232f) : card, 10);
    Symbol({r.CenterX() - 10, r.CenterY() - 10, 20, 20}, icon, active ? accent : hover ? white : muted);
    hits_.push_back({r, action});
}
void Home::Art(const view::HomeTrack& t, Rect r) {
    RequestArt(t);
    auto bitmap = art_.Get(Key(t.path.wstring()), dc_, renderer_->DeviceGeneration());
    Box(r, card, 9);
    if (bitmap) Image(bitmap, r);
    else if (heroBitmap_) {
        const auto hash = Key(t.path.wstring());
        Image(heroBitmap_.Get(), r, 12, .45f + (hash % 5) * .12f);
        Box(r, D2D1::ColorF(.08f + (hash % 7) * .025f, .06f, .14f + ((hash >> 8) % 5) * .03f, .76f), 12);
        if (r.w >= 90) {
            TextSpec spec;
            spec.size = r.w * .23f;
            spec.weight = 600;
            spec.align = DWRITE_TEXT_ALIGNMENT_CENTER;
            std::wstring initials;
            for (const auto c : t.title) {
                if (std::iswalnum(c)) initials += (wchar_t)std::towupper(c);
                if (initials.size() == 2) break;
            }
            if (initials.empty()) initials = L"PM";
            auto label = text_->Layout(initials, spec, r.w, r.h);
            if (label) {
                brush_->SetColor(D2D1::ColorF(1, 1, 1, .78f));
                dc_->DrawTextLayout(D2D1::Point2F(r.x, r.y), label.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    } else {
        Box(r, D2D1::ColorF(0x332735), 12);
        Symbol({r.CenterX() - 18, r.CenterY() - 18, 36, 36}, IconId::Moon, muted);
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
    const double clock = GetTickCount64() / 1000.;
    frameDelta_ = (float)std::clamp(clock - lastPaint_, .001, .05);
    lastPaint_ = clock;
    reducedMotion_ = in.reducedMotion;
    needsFrames_ = false;
    if (hover_.size() > 2048) hover_.clear();
    if (generation_ != renderer_->DeviceGeneration() || !brush_) {
        brush_.Reset();
        heroBitmap_.Reset();
        dc->CreateSolidColorBrush(white, &brush_);
        generation_ = renderer_->DeviceGeneration();
    }
    if (!brush_) return;
    DrainArt();
    hits_.clear();
    dc->SetTransform(D2D1::IdentityMatrix());
    dc->Clear(bg);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (heroSource_ && !heroBitmap_) dc->CreateBitmapFromWicBitmap(heroSource_.Get(), &heroBitmap_);
    const bool workspace = model.page >= view::HomeModel::Plugins;
    const view::HomeLayout layout(in.size, !workspace);
    const float w = in.size.w, bottom = layout.bottom, side = layout.sidebar;
    const float mainX = layout.mainX, mainW = layout.mainW, end = layout.end;
    const char* heart = "M20.8 4.6C18.5 2.5 14.8 3 12 6C9.2 3 5.5 2.5 3.2 4.6C.4 7.2 2.3 11.6 5.1 14.4L12 21L18.9 14.4C21.7 11.6 23.6 7.2 20.8 4.6Z";
    Box({0, 0, side, bottom}, surface, 0);
    Box({side, 18, 1, bottom - 18}, D2D1::ColorF(0x24252f), 0);
    Box({20, 24, 34, 34}, D2D1::ColorF(0x38232f), 11);
    Symbol({27, 31, 20, 20}, IconId::Moon, accent);
    Text(L"PlayerMoon", {64, 23, 128, 25}, 17, white, 700);
    Text(L"МУЗЫКА РЯДОМ", {65, 49, 120, 16}, 9, muted, 600);
    Text(L"МЕДИАТЕКА", {25, 91, 150, 18}, 9, muted, 600);
    const wchar_t* labels[] = {L"Главная", L"Библиотека", L"Любимое", L"Очередь", L"Плагины", L"Скачивание", L"Настройки"};
    for (int i = 0; i < 7; ++i) {
        Rect r{12, 116.f + i * 42, side - 24, 38};
        bool active = model.page == i;
        if (active || r.Contains(mouse_.x, mouse_.y)) Box(r, active ? D2D1::ColorF(0x35232e) : card, 10);
        const auto color = active ? accent : muted;
        Rect icon{27, r.y + 9, 20, 20};
        if (i == 0) Path(icon, "M3 10L12 3L21 10V21H15V14H9V21H3Z", color);
        if (i == 1) Path(icon, "M4 4V21M10 4V21M16 5L21 20", color);
        if (i == 2) Path(icon, heart, color);
        if (i == 3) Symbol(icon, IconId::Playlist, color);
        if (i == 4) Path(icon, "M8 3H16V8H21V16H16V21H8V16H3V8H8Z", color);
        if (i == 5) Path(icon, "M12 3V16M6 10L12 16L18 10M4 18V22H20V18", color);
        if (i == 6) Symbol(icon, IconId::Settings, color);
        Text(labels[i], {59, r.y, 116, 38}, 13, active ? white : muted, active ? 600 : 400);
        hits_.push_back({r, None, 10000 + i});
    }
    if (bottom > 740) {
        Box({20, bottom - 206, 160, 82}, D2D1::ColorF(0x1e1c26), 12);
        Text(L"Всё самое близкое", {32, bottom - 197, 136, 23}, 12, white, 600);
        Text(in.scanning ? L"Обновляю библиотеку…" : Count(model.tracks.size()) + L" в вашей коллекции",
             {32, bottom - 170, 136, 32}, 11, muted);
    }
    Button({20, bottom - 96, 160, 38}, L"Добавить музыку", OpenFiles);
    IconButton({20, bottom - 50, 34, 34}, IconId::MiniMode, Compact);
    Text(L"Компактный плеер", {64, bottom - 50, 123, 34}, 12, muted);
    hits_.push_back({{20, bottom - 50, 164, 34}, Compact});
    Text(L"ВАШЕ ПРОСТРАНСТВО МУЗЫКИ", {mainX, 17, mainW - 130, 24}, 9, muted, 600);
    IconButton({w - 124, 7, 34, 30}, IconId::Minimize, Minimize);
    IconButton({w - 84, 7, 34, 30}, IconId::Expand, Maximize);
    IconButton({w - 44, 7, 34, 30}, IconId::Close, Close);
    if (workspace) {
        PaintWorkspace({mainX, 58, mainW, bottom - 76}, in);
    } else {
    Box(layout.search, surface, 12);
    if (in.searchFocused) {
        brush_->SetColor(D2D1::ColorF(0x665061));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(Native(layout.search), 12, 12), brush_.Get(), 1);
    }
    Path({mainX + 15, 71, 18, 18}, "M16 16L22 22M18 10A8 8 0 1 1 2 10A8 8 0 1 1 18 10", muted);
    const Rect search = layout.searchText;
    if (model.query.empty()) Text(L"Найти трек, исполнителя или альбом", search, 14, muted);
    else {
        TextSpec spec;
        spec.size = 14;
        const auto a = std::min<size_t>(in.selectionStart, model.query.size());
        const auto b = std::min<size_t>(in.selectionEnd, model.query.size());
        float x = text_->Measure(model.query.substr(0, a), spec);
        if (in.searchFocused && a != b && x < search.w) {
            float width = text_->Measure(model.query.substr(a, b - a), spec);
            Box({search.x + x, search.y, std::min(width, search.w - x), search.h}, D2D1::ColorF(0x713b4b), 2);
        }
        Text(model.query, search, 14, white);
        if (in.searchFocused && a == b && x < search.w) Box({search.x + x, search.y + 2, 1, 18}, accent, 0);
    }
    if (in.searchFocused && model.query.empty()) Box({search.x, search.y + 2, 1, 18}, accent, 0);
    const bool hero = model.page == view::HomeModel::Home && model.query.empty();
    float top = 126;
    if (hero) {
        const float heroH = bottom < 590 ? 144.f : 206.f;
        Rect banner{mainX, top, mainW, heroH};
        Box(banner, D2D1::ColorF(0x251c29), 18);
        Image(heroBitmap_.Get(), banner, 18);
        // A horizontal scrim preserves contrast independently of the artwork.
        D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x16151d, .98f)},
                                     {.58f, D2D1::ColorF(0x16151d, .78f)},
                                     {1, D2D1::ColorF(0x16151d, .05f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        dc->CreateGradientStopCollection(stops, 3, &collection);
        if (collection) dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(mainX, top), D2D1::Point2F(end, top)),
            collection.Get(), &gradient);
        if (gradient) dc->FillRoundedRectangle(D2D1::RoundedRect(Native(banner), 18, 18), gradient.Get());
        Text(L"ЛИЧНАЯ ВОЛНА", {mainX + 26, top + 17, mainW - 52, 20}, 9, accent, 700);
        Text(L"На твоей частоте", {mainX + 26, top + (heroH < 160 ? 36 : 44), mainW - 52, 43}, heroH < 160 ? 28.f : 34.f, white, 700);
        if (heroH >= 160) Text(L"Любимые треки. Новые сочетания.", {mainX + 26, top + 91, mainW - 52, 23}, 12, D2D1::ColorF(0xc6bfcc));
        Button({mainX + 26, top + heroH - 54, 151, 36}, L"Слушать волну", Wave, -1, true);
        Button({mainX + 189, top + heroH - 54, 133, 36}, L"Открыть папку", OpenFolder);
        top += heroH + 22;
    }
    Text(!model.query.empty() ? L"Результаты поиска" : hero ? L"Твоя коллекция" : labels[(int)model.page],
         {mainX, top, mainW - 88, 30}, 22, white, 650);
    if (renderedRevision_ != revision_ || renderedPage_ != (int)model.page || renderedQuery_ != model.query) {
        displayed_.clear();
        if (model.page == view::HomeModel::Queue) {
            for (const auto& t : in.queue) if (model.Match(t)) displayed_.push_back(t);
        } else for (auto index : model.Visible()) displayed_.push_back(model.tracks[index]);
        renderedRevision_ = revision_;
        renderedPage_ = (int)model.page;
        renderedQuery_ = model.query;
    }
    Text(Count(displayed_.size()), {end - 85, top + 5, 85, 22}, 11, muted);
    top += 40;
    const bool listMode = model.page == view::HomeModel::Queue;
    const int columns = listMode ? 1 : std::max(2, (int)((mainW + 18) / (hero && bottom < 590 ? 128.f : 170.f)));
    const float tile = (mainW - (columns - 1) * 18) / columns, row = listMode ? 70.f : tile + 76;
    const float available = std::max(0.f, bottom - 20 - top);
    const int rowCount = ((int)displayed_.size() + columns - 1) / columns;
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, rowCount * row - available));
    // Empty content is clipped to the collection viewport, just like track tiles.
    dc->PushAxisAlignedClip(D2D1::RectF(mainX - 7, top, end + 7, bottom - 16), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (displayed_.empty()) {
        Text(model.query.empty() ? (model.page == view::HomeModel::Favorites ? L"Сохраняй то, что нравится" : L"Здесь начинается твоя музыка") : L"Ничего не найдено",
             {mainX, top + 10, mainW, 32}, 19, white, 600);
        Text(model.query.empty() ? L"Добавь файлы или перетащи папку в окно." : L"Попробуй другое название или исполнителя.",
             {mainX, top + 48, mainW, 26}, 12, muted);
        if (model.query.empty() && available > 125) Button({mainX, top + 86, 166, 38}, L"Добавить музыку", OpenFiles, -1, true);
    }
    int first = std::max(0, (int)(scroll_ / row)) * columns;
    int last = std::min((int)displayed_.size(), first + ((int)(available / row) + 2) * columns);
    for (int i = first; i < last; ++i) {
        float x = mainX + (i % columns) * (tile + 18), y = top + (i / columns) * row - scroll_;
        const auto& t = displayed_[i];
        if (listMode) {
            Rect entry{x, y, mainW, 64};
            const bool current = !in.current.empty() && view::HomePathKey(in.current) == view::HomePathKey(t.path);
            const bool hover = entry.Contains(mouse_.x, mouse_.y);
            if (current || hover) Box(entry, current ? D2D1::ColorF(0x30212d) : card, 12);
            Text(std::to_wstring(i + 1), {x + 9, y + 20, 25, 24}, 11, current ? accent : muted);
            Art(t, {x + 39, y + 8, 48, 48});
            Text(t.title, {x + 101, y + 10, mainW - 225, 24}, 13, current ? accent : white, 600);
            Text(t.artist.empty() ? L"Неизвестный исполнитель" : t.artist,
                 {x + 101, y + 35, mainW - 225, 20}, 11, muted);
            Text(t.duration > 0 ? Time(t.duration) : L"—", {end - 105, y + 22, 49, 22}, 11, muted);
            Rect hit{x, std::max(top, y), mainW, std::max(0.f, std::min(bottom - 16, y + 64) - std::max(top, y))};
            if (hit.h > 0) hits_.push_back({hit, PlayTrack, i});
            const bool favorite = model.favorites.count(view::HomePathKey(t.path));
            Rect love{end - 42, y + 17, 30, 30};
            Path({love.x + 6, love.y + 6, 18, 18}, heart, favorite ? accent : muted, favorite);
            if (love.y >= top && love.Bottom() <= bottom - 16) hits_.push_back({love, Favorite, i});
            if (current || hover) {
                Box({x + 39, y + 8, 48, 48}, D2D1::ColorF(0x101116, .56f), 12);
                Rect toggle{x + 47, y + 16, 32, 32};
                Glyph(toggle, current && in.playback.playing ? 1 : 0, white);
                if (current && toggle.y >= top && toggle.Bottom() <= bottom - 16) hits_.push_back({toggle, Toggle});
            }
            continue;
        }
        Rect hit{x, std::max(top, y), tile, std::max(0.f, std::min(bottom - 16, y + tile + 66) - std::max(top, y))};
        bool current = !in.current.empty() && view::HomePathKey(in.current) == view::HomePathKey(t.path);
        bool hover = hit.Contains(mouse_.x, mouse_.y);
        if (hover || current) Box({x - 6, y - 6, tile + 12, tile + 72}, current ? D2D1::ColorF(0x2b202b) : card, 15);
        Art(t, {x, y, tile, tile});
        if (hit.h > 0) hits_.push_back({hit, PlayTrack, i});
        Text(t.title, {x, y + tile + 10, tile, 22}, 13, current ? accent : white, 600);
        Text(t.artist.empty() ? L"Неизвестный исполнитель" : t.artist, {x, y + tile + 33, tile, 20}, 11, muted);
        const bool favorite = model.favorites.count(view::HomePathKey(t.path));
        Rect love{x + tile - 33, y + 8, 26, 26};
        Box(love, D2D1::ColorF(0x15161d, .88f), 13);
        Path({love.x + 6, love.y + 6, 14, 14}, heart, favorite ? accent : white, favorite);
        if (love.y >= top && love.Bottom() <= bottom - 16) hits_.push_back({love, Favorite, i});
        if (hover || current) {
            Rect play{x + tile - 49, y + tile - 49, 39, 39};
            Box(play, accent, 20);
            Glyph(play, current && in.playback.playing ? 1 : 0, white);
            if (current && play.y >= top && play.Bottom() <= bottom - 16)
                hits_.push_back({play, Toggle});
        }
    }
    dc->PopAxisAlignedClip();
    if (rowCount * row > available && available > 0) {
        float thumb = std::max(24.f, available * available / (rowCount * row));
        float y = top + (available - thumb) * (scroll_ / (rowCount * row - available));
        Box({end + 12, y, 3, thumb}, D2D1::ColorF(0x4c4656), 2);
    }
    if (layout.queue) {
        const float x = end + 32, qw = w - x - 24;
        Box({x - 14, 58, qw + 26, bottom - 76}, surface, 16);
        Text(L"Далее", {x, 77, qw, 26}, 18, white, 650);
        int start = std::max(0, in.playback.index + 1);
        Text(Count(in.queue.size() > (size_t)start ? in.queue.size() - start : 0), {x, 107, qw, 20}, 11, muted);
        int shown = 0;
        for (int i = start; i < (int)in.queue.size() && 150 + (shown + 1) * 68 < bottom - 24; ++i, ++shown) {
            float y = 150.f + shown * 68;
            const auto& t = in.queue[i];
            Rect r{x - 5, y - 5, qw + 10, 61};
            if (r.Contains(mouse_.x, mouse_.y)) Box(r, card, 9);
            Art(t, {x, y, 46, 46});
            Text(t.title, {x + 58, y + 1, qw - 58, 23}, 12, white, 600);
            Text(t.artist.empty() ? L"Локальный трек" : t.artist, {x + 58, y + 25, qw - 58, 20}, 10, muted);
            hits_.push_back({r, PlayTrack, -i - 2});
        }
        if (!shown) {
            Symbol({x + 4, 165, 30, 30}, IconId::Playlist, muted);
            Text(L"Пусть музыка продолжается", {x, 214, qw, 52}, 15, white, 600);
            Text(L"Выбери трек в коллекции — следующие появятся здесь.", {x, 273, qw, 68}, 12, muted);
        }
    }
    }
    Box({0, bottom, w, 112}, D2D1::ColorF(0x181920), 0);
    Box({0, bottom, w, 1}, D2D1::ColorF(0x2b2a35), 0);
    view::HomeTrack current;
    current.path = in.current;
    current.title = in.title;
    current.artist = in.artist;
    if (!current.path.empty()) Art(current, {22, bottom + 26, 56, 56});
    else { Box({22, bottom + 26, 56, 56}, card, 10); Symbol({38, bottom + 42, 24, 24}, IconId::Moon, muted); }
    Text(in.title.empty() ? L"Выбери свой первый трек" : in.title,
         {layout.trackText.x, layout.trackText.y, layout.trackText.w, 23}, 13, white, 600);
    Text(in.artist.empty() ? L"Твоя музыка всегда рядом" : in.artist,
         {layout.trackText.x, layout.trackText.y + 25, layout.trackText.w, 20}, 11, muted);
    const auto control = layout.transport;
    IconButton({control.x, control.y + 4, 34, 34}, IconId::Shuffle, Shuffle, in.playback.shuffle);
    IconButton({control.x + 48, control.y + 4, 34, 34}, IconId::Prev, Previous);
    Rect play{control.CenterX() - 22, control.y, 44, 44};
    Box(play, play.Contains(mouse_.x, mouse_.y) ? D2D1::ColorF(0xff8296) : accent, 22);
    Glyph(play, in.playback.playing ? 1 : 0, white);
    hits_.push_back({play, Toggle});
    IconButton({control.x + 170, control.y + 4, 34, 34}, IconId::Next, Next);
    IconButton({control.x + 218, control.y + 4, 34, 34}, IconId::Repeat, Repeat, in.playback.repeat != pm::Repeat::Off);
    if (in.playback.repeat == pm::Repeat::One) Text(L"1", {control.x + 231, control.y + 2, 13, 14}, 8, accent, 700);
    const Rect seek = layout.seek;
    const float sy = seek.CenterY() - 2;
    Box({seek.x, sy, seek.w, 4}, D2D1::ColorF(0x3c3a49), 2);
    float position = in.playback.duration > 0 ? std::clamp((float)(in.playback.position / in.playback.duration), 0.f, 1.f) : 0;
    if (position > 0) Box({seek.x, sy, seek.w * position, 4}, accent, 2);
    if (seek.Contains(mouse_.x, mouse_.y)) Box({seek.x + seek.w * position - 4, sy - 2, 8, 8}, white, 4);
    hits_.push_back({seek, Seek});
    Text(Time(in.playback.position), {seek.x - 41, seek.y, 36, 20}, 10, muted);
    Text(Time(in.playback.duration), {seek.Right() + 10, seek.y, 37, 20}, 10, muted);
    const Rect volume = layout.volume;
    Symbol({volume.x - 32, volume.y + 1, 20, 20}, IconId::Volume, muted);
    Box({volume.x, volume.CenterY() - 2, volume.w, 4}, D2D1::ColorF(0x3c3a49), 2);
    const float level = std::clamp(in.playback.muted ? 0.f : in.playback.volume, 0.f, 1.f);
    if (level > 0) Box({volume.x, volume.CenterY() - 2, volume.w * level, 4}, muted, 2);
    Box({volume.x + volume.w * level - 3, volume.CenterY() - 3, 6, 6}, white, 3);
    hits_.push_back({volume, Volume});
}
}  // namespace ui
