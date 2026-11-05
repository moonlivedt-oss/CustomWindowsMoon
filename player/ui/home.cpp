#include "ui/home.h"
#include "ui/home_colors.h"
#include "ui/workspace.h"
#include "core/tags.h"
#include <cmath>
#include <d2d1_2helper.h>
#include <d2d1helper.h>

namespace ui {
namespace {
using view::Rect;
// Ссылки на текущие цвета: их задаёт Paint из Input::colors
const D2D1_COLOR_F &bg = HomePalette().bg, &surface = HomePalette().surface, &card = HomePalette().card;
const D2D1_COLOR_F &white = HomePalette().white, &muted = HomePalette().muted, &accent = HomePalette().accent;
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
    art_.SetKeepPixels(false);  // обложек бывает больше сотни; загрузить заново просто
    icons_.Init(renderer->Factory());
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    const auto artDir = std::filesystem::path(exe).parent_path() / L"assets" / L"art";
    assetsDir_ = artDir.parent_path();
    // Картинки оформления и рамки читаются при первом показе: все вместе - это полсекунды запуска и десятки мегабайт
    const wchar_t* frames[] = {L"lunar.png",    L"neon.png", L"vinyl.png",    L"sunset.png", L"frost.png",
                               L"cassette.png", L"gold.png", L"polaroid.png", L"film.png",   L"aurora.png"};
    const auto present = [](const std::filesystem::path& file) {
        return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    for (int i = 0; i < kFrames; ++i)
        if (const auto file = assetsDir_ / L"frames" / frames[i]; present(file)) frameFiles_[i] = file;
    if (const auto moon = assetsDir_ / L"frames" / L"moon.png"; frameFiles_[0].empty() && present(moon))
        frameFiles_[0] = moon;
    worker_ = std::thread([this] { ArtLoop(); });
}
// Окно рамки меряется от центра к четырём краям до первого непрозрачного пикселя: так встаёт
// и рамка с другими полями, и кассета, у которой окно смещено вверх
ComPtr<IWICBitmapSource> Home::DecodeFrame(int i) const {
    auto source = LoadAsset(frameFiles_[i], 1024);
    if (!source || frameMeasured_[i]) return source;
    frameMeasured_[i] = true;
    UINT fw = 0, fh = 0;
    source->GetSize(&fw, &fh);
    if (fw < 16 || fh < 16) return source;
    std::vector<uint8_t> row(fw * 4);
    const WICRect line{0, (INT)(fh / 2), (INT)fw, 1};
    if (FAILED(source->CopyPixels(&line, fw * 4, (UINT)row.size(), row.data()))) return source;
    std::vector<uint8_t> strip(fh * 4);
    for (UINT y = 0; y < fh; ++y) {
        const WICRect px{(INT)(fw / 2), (INT)y, 1, 1};
        if (FAILED(source->CopyPixels(&px, 4, 4, strip.data() + y * 4))) break;
    }
    auto solid = [](const std::vector<uint8_t>& v, UINT at) { return v[at * 4 + 3] > 60; };
    UINT l = fw / 2, r = fw / 2, t = fh / 2, b = fh / 2;
    while (l > 0 && !solid(row, l - 1)) --l;
    while (r + 1 < fw && !solid(row, r + 1)) ++r;
    while (t > 0 && !solid(strip, t - 1)) --t;
    while (b + 1 < fh && !solid(strip, b + 1)) ++b;
    frameWindow_[i] = {(float)l / fw, (float)t / fh, (float)(r + 1) / fw, (float)(b + 1) / fh};
    return source;
}
const Home::FrameWindow& Home::WindowOfFrame(int i) const {
    // Первая просьба о проёме: читаем картинку, она сразу пойдёт и на видеокарту (CoverFrame)
    if (!frameMeasured_[i] && !frameFiles_[i].empty()) frameSources_[i] = DecodeFrame(i);
    return frameWindow_[i];
}
ID2D1Bitmap1* Home::HeroBitmap() {
    if (!heroBitmap_ && dc_) {
        if (auto source = LoadAsset(assetsDir_ / L"art" / L"moon-session.png", 0))
            dc_->CreateBitmapFromWicBitmap(source.Get(), &heroBitmap_);
    }
    return heroBitmap_.Get();
}
ID2D1Bitmap1* Home::DownloadsBitmap() {
    if (!downloadsBitmap_ && dc_) {
        if (auto source = LoadAsset(assetsDir_ / L"art" / L"moon-downloads.png", 0))
            dc_->CreateBitmapFromWicBitmap(source.Get(), &downloadsBitmap_);
    }
    return downloadsBitmap_.Get();
}
ComPtr<IWICBitmapSource> Home::LoadAsset(const std::filesystem::path& file, UINT maxSide) const {
    auto* wic = renderer_->Wic();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;
    ComPtr<IWICBitmapSource> source = converter;
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (maxSide && std::max(w, h) > maxSide) {
        // Ужать сразу при чтении: в памяти лежит уже маленькая копия
        ComPtr<IWICBitmapScaler> scaler;
        const float k = (float)maxSide / std::max(w, h);
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(converter.Get(), std::max(1u, (UINT)(w * k)), std::max(1u, (UINT)(h * k)),
                                         WICBitmapInterpolationModeFant)))
            source = scaler;
    }
    ComPtr<IWICBitmap> image;
    if (FAILED(wic->CreateBitmapFromSource(source.Get(), WICBitmapCacheOnLoad, &image))) return nullptr;
    return image;
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
    const std::wstring& key = t.path.native();
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
        if (art_.Load(Key(a.key), a.bytes, a.title, palette)) {
            const auto mood = theme::CoverAccent(palette, true);
            coverMood_[Key(a.key)] = D2D1::ColorF(mood.r, mood.g, mood.b);
        }
    }
}
const std::wstring& Home::PathKey(const std::filesystem::path& path) {
    auto it = pathKeys_.find(path.native());
    if (it != pathKeys_.end()) return it->second;
    if (pathKeys_.size() >= 4096) pathKeys_.clear();
    return pathKeys_.emplace(path.native(), view::HomePathKey(path)).first->second;
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
    if (primary) {
        // Главные кнопки слегка тянутся к курсору
        const auto pull = Magnet((int)a * 100000 + index + 50000, r, 28, .1f);
        r.x += pull.x;
        r.y += pull.y;
    }
    Box(r, primary ? accent : D2D1::ColorF(card.r + amount * .06f, card.g + amount * .05f, card.b + amount * .06f), 9);
    TextSpec spec;
    spec.size = 13;
    spec.weight = 600;
    spec.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    auto layout = text_->Layout(s, spec, std::max(1.f, r.w - 20), r.h);
    if (layout) {
        brush_->SetColor(primary ? HomePalette().onAccent : white);
        dc_->DrawTextLayout(D2D1::Point2F(r.x + 10, r.y), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    hits_.push_back({r, a, index, s});
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
        // Толщина линии одна для всех иконок: ~1.6 px на 20 px размера, независимо от сетки 16 или 24
        if (layer.solid)
            dc_->FillGeometry(layer.geometry.Get(), brush_.Get());
        else
            dc_->DrawGeometry(layer.geometry.Get(), brush_.Get(), std::clamp(1.6f * r.w / 20, 1.3f, 2.6f) / scale);
    }
    dc_->SetTransform(previous);
}
void Home::Path(Rect r, std::string_view path, D2D1_COLOR_F color, bool fill) {
    auto found = paths_.find(path);
    if (found == paths_.end())
        found = paths_.emplace(std::string(path), ParseSvgPath(renderer_->Factory(), std::string(path))).first;
    ID2D1PathGeometry* geo = found->second.Get();
    if (!geo) return;
    D2D1_MATRIX_3X2_F previous;
    dc_->GetTransform(&previous);
    brush_->SetColor(color);
    const float strokePx = std::clamp(1.6f * r.w / 20, 1.3f, 2.6f);
    // Значки превращаются в треугольники один раз на размер: разбирать кривые на каждом кадре дорого
    const MeshKey key{geo, r.w, r.h, fill};
    auto mesh = pathMeshes_.find(key);
    if (mesh == pathMeshes_.end() && dc1_) {
        if (pathMeshes_.size() >= 512) pathMeshes_.clear();  // размеры анимаций не копятся без конца
        float dpiX = 96, dpiY = 96;
        dc_->GetDpi(&dpiX, &dpiY);
        const float tolerance = D2D1::ComputeFlatteningTolerance(D2D1::Matrix3x2F::Identity(), dpiX, dpiY);
        ComPtr<ID2D1TransformedGeometry> sized;
        ComPtr<ID2D1GeometryRealization> made;
        renderer_->Factory()->CreateTransformedGeometry(geo, D2D1::Matrix3x2F::Scale(r.w / 24, r.h / 24), &sized);
        if (sized && fill)
            dc1_->CreateFilledGeometryRealization(sized.Get(), tolerance, &made);
        else if (sized)
            dc1_->CreateStrokedGeometryRealization(sized.Get(), tolerance, strokePx, nullptr, &made);
        mesh = pathMeshes_.emplace(key, std::move(made)).first;
    }
    if (mesh != pathMeshes_.end() && mesh->second) {
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
        dc1_->DrawGeometryRealization(mesh->second.Get(), brush_.Get());
    } else {
        dc_->SetTransform(D2D1::Matrix3x2F::Scale(r.w / 24, r.h / 24) * D2D1::Matrix3x2F::Translation(r.x, r.y));
        if (fill)
            dc_->FillGeometry(geo, brush_.Get());
        else
            dc_->DrawGeometry(geo, brush_.Get(), strokePx / (r.w / 24));
    }
    dc_->SetTransform(previous);
}
void Home::Image(ID2D1Bitmap1* bitmap, Rect r, float radius, float anchor, float anchorY, float zoom) {
    if (!bitmap || r.Empty()) return;
    auto size = bitmap->GetSize();
    float sw = size.width, sh = size.height;
    if (sw / sh > r.w / r.h)
        sw = sh * r.w / r.h;
    else
        sh = sw * r.h / r.w;
    // zoom > 1 - медленный «наезд камеры»: берём меньший участок вокруг той же точки
    sw /= zoom;
    sh /= zoom;
    auto source = D2D1::RectF((size.width - sw) * anchor, (size.height - sh) * anchorY, (size.width - sw) * anchor + sw,
                              (size.height - sh) * anchorY + sh);
    // Кисть из картинки вместо слоя с маской: слой на каждую обложку заставляет Direct2D переключать цель и сбрасывать
    // пакет
    const auto props =
        D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_LINEAR);
    if (imageBrush_)
        imageBrush_->SetBitmap(bitmap);
    else
        dc_->CreateBitmapBrush(bitmap, props, &imageBrush_);
    if (!imageBrush_) return;
    imageBrush_->SetTransform(D2D1::Matrix3x2F::Translation(-source.left, -source.top) *
                              D2D1::Matrix3x2F::Scale(r.w / sw, r.h / sh) * D2D1::Matrix3x2F::Translation(r.x, r.y));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(Native(r), radius, radius), imageBrush_.Get());
}
void Home::IconButton(Rect r, IconId icon, Action action, bool active) {
    const bool hover = r.Contains(mouse_.x, mouse_.y);
    if (hover || active) Box(r, active ? HomePalette().soft : card, 10);
    Symbol({r.CenterX() - 10, r.CenterY() - 10, 20, 20}, icon, active ? accent : hover ? white : muted);
    hits_.push_back({r, action});
}
void Home::Art(const view::HomeTrack& t, Rect r, float radius) {
    RequestArt(t);
    const auto artKey = Key(t.path.native());
    auto bitmap = art_.Get(artKey, dc_, renderer_->DeviceGeneration());
    Box(r, card, radius > 20 ? radius : 9.f);  // круглая обложка - и подложка круглая
    if (bitmap)
        Image(bitmap, r, radius);
    else if (auto* placeholder = PictureBitmap((Picture)(Placeholder1 + artKey % 12))) {
        // Нет обложки - одна из двенадцати картин-заглушек; у каждого трека всегда своя
        Image(placeholder, r, radius);
    } else if (auto* hero = HeroBitmap()) {
        const auto hash = artKey;
        Image(hero, r, radius, .45f + (hash % 5) * .12f);
        // Свой оттенок у каждого трека: диагональная вуаль из пары цветов набора
        static const uint32_t tints[][2] = {{0x5a1f3a, 0x1a1230}, {0x2c2466, 0x120f24}, {0x174a52, 0x0e1420},
                                            {0x5c3a16, 0x1a1016}, {0x4a1d5e, 0x140e22}, {0x1e3a66, 0x0e1222}};
        const auto& tint = tints[hash % 6];
        D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(tint[0], .72f)}, {1, D2D1::ColorF(tint[1], .86f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> veil;
        collection = gradients_.Get(dc_, stops, 2);
        if (collection)
            dc_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.Right(), r.Bottom())),
                collection.Get(), &veil);
        if (veil) dc_->FillRoundedRectangle(D2D1::RoundedRect(Native(r), radius, radius), veil.Get());
        if (r.w >= 90) {
            TextSpec spec;
            spec.size = r.w * .2f;
            spec.weight = 300;
            spec.spacing = r.w * .01f;
            spec.align = DWRITE_TEXT_ALIGNMENT_CENTER;
            std::wstring initials;
            for (const auto c : t.title) {
                if (std::iswalnum(c)) initials += (wchar_t)std::towupper(c);
                if (initials.size() == 2) break;
            }
            if (initials.empty()) initials = L"PM";
            auto label = text_->Layout(initials, spec, r.w, r.h);
            if (label) {
                brush_->SetColor(D2D1::ColorF(1, 1, 1, .62f));
                dc_->DrawTextLayout(D2D1::Point2F(r.x, r.y), label.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    } else {
        Box(r, D2D1::ColorF(0x332735), radius);
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
    ++paintFrame_;
    const double clock = GetTickCount64() / 1000.;
    frameDelta_ = (float)std::clamp(clock - lastPaint_, .001, .05);
    lastPaint_ = clock;
    reducedMotion_ = in.reducedMotion;
    needsFrames_ = false;
    nextWake_ = 0;
    clock_ = clock;
    SetHomePalette(in.colors);
    StepScroll();
    if (currentTrack_.path.native() != in.current.native()) currentTrack_.path = in.current;
    if (currentTrack_.title != in.title) currentTrack_.title = in.title;
    if (currentTrack_.artist != in.artist) currentTrack_.artist = in.artist;
    if (trackKey_ != in.current.native()) {
        if (!trackKey_.empty() || lastPaint_ > 0) trackChangedAt_ = clock;
        trackKey_ = in.current.native();
    }
    if (lastPage_ != (int)model.page) {
        // Прошлый кадр остаётся в своём буфере - он и уезжает; новый рисуется во второй
        if (lastPage_ >= 0 && frames_[frameIndex_] && !reducedMotion_) {
            previousFrame_ = frameIndex_;
            frameIndex_ ^= 1;
            pageSwitchedAt_ = clock;
            pageDirection_ = (int)model.page > lastPage_ ? 1 : -1;
        }
        lastPage_ = (int)model.page;
        pageEnteredAt_ = clock;
    }
    if (hover_.size() > 2048) hover_.clear();
    if (generation_ != renderer_->DeviceGeneration() || !brush_) {
        brush_.Reset();
        heroBitmap_.Reset();
        downloadsBitmap_.Reset();
        hue_.Reset();
        blur_.Reset();
        grain_.Reset();
        frames_[0].Reset();
        frames_[1].Reset();
        for (auto& f : frameBitmaps_) f.Reset();
        for (auto& f : pictureBitmaps_) f.Reset();
        if (generation_) {
            std::lock_guard lock(mutex_);
            requested_.clear();
            art_.Keep({});
        }
        previousFrame_ = -1;
        imageBrush_.Reset();
        blurred_.Reset();
        blurOf_.Reset();
        gradients_.Clear();
        pathMeshes_.clear();
        dc1_.Reset();
        dc->QueryInterface(IID_PPV_ARGS(&dc1_));
        dc->CreateSolidColorBrush(white, &brush_);
        generation_ = renderer_->DeviceGeneration();
    }
    if (!brush_) return;
    DrainArt();
    hits_.clear();
    dc->SetTransform(D2D1::IdentityMatrix());
    const bool offscreen = BeginOffscreen(dc);
    dc->Clear(bg);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const bool workspace = model.page >= view::HomeModel::Plugins;
    const view::HomeLayout layout(in.size, !workspace);
    const float w = in.size.w, bottom = layout.bottom, side = layout.sidebar;
    const float mainX = layout.mainX, mainW = layout.mainW, end = layout.end;
    const char* heart =
        "M20.8 4.6C18.5 2.5 14.8 3 12 6C9.2 3 5.5 2.5 3.2 4.6C.4 7.2 2.3 11.6 5.1 14.4L12 21L18.9 14.4C21.7 11.6 23.6 "
        "7.2 20.8 4.6Z";
    Aurora({0, 0, w, bottom}, in);
    // Панели чуть прозрачные: живой фон просвечивает, как сквозь стекло
    Box({0, 0, side, bottom}, D2D1::ColorF(surface.r, surface.g, surface.b, .78f), 0);
    Box({side, 18, 1, bottom - 18}, D2D1::ColorF(0x24252f), 0);
    if (auto* logo = PictureBitmap(AppLogo)) {
        Glow({20, 24, 34, 34}, accent, 10, 6, .45f);
        Image(logo, {18, 22, 38, 38}, 9);
    } else {
        Box({20, 24, 34, 34}, HomePalette().soft, 11);
        Symbol({27, 31, 20, 20}, IconId::Moon, accent);
    }
    Text(L"PlayerMoon", {64, 23, 128, 25}, 17, white, 700);
    Text(L"МУЗЫКА РЯДОМ", {65, 49, 120, 16}, 9, muted, 600);
    Text(L"МЕДИАТЕКА", {25, 91, 150, 18}, 9, muted, 600);
    const wchar_t* labels[] = {L"Главная", L"Библиотека", L"Любимое",  L"Очередь",
                               L"Плагины", L"Скачивание", L"Настройки"};
    if (model.page >= 0 && model.page < 7) {
        // Подсветка раздела перетекает вниз и вверх по меню вместе с полоской акцента
        const auto nav = LiquidRect(101, {12, 116.f + (int)model.page * 42, side - 24, 38}, (int)model.page);
        Glow(nav, accent, 10, 6, .5f);
        Box(nav, HomePalette().soft, 10);
        Box({nav.x, nav.y + 9, 3, std::max(4.f, nav.h - 18)}, accent, 2);
    }
    for (int i = 0; i < 7; ++i) {
        Rect r{12, 116.f + i * 42, side - 24, 38};
        bool active = model.page == i;
        const float over = Hover(800000 + i, r.Contains(mouse_.x, mouse_.y));
        if (!active && over > .01f) Box(r, D2D1::ColorF(card.r, card.g, card.b, over), 10);
        r.x += over * 3;
        const auto color = active ? accent : muted;
        Rect icon{r.x + 15, r.y + 9, 20, 20};
        D2D1_MATRIX_3X2_F navFlat;
        dc->GetTransform(&navFlat);
        dc->SetTransform(NavIconMotion(i, icon, over) * navFlat);
        if (i == 0) Path(icon, "M3 10L12 3L21 10V21H15V14H9V21H3Z", color);
        if (i == 1) {
            const float pulse = std::max(0.f, 1 - (float)(clock - libraryPulseAt_) / .9f);
            if (pulse > 0) {
                Glow(icon, accent, 10, 10 * pulse, 2 * pulse);
                needsFrames_ = true;
            }
            Path(icon, "M4 9H20V20H4ZM6.5 5.5H17.5M9 2.5H15", pulse > 0 ? MixColor(color, accent, pulse) : color);
            if (pulse > 0) {
                dc->SetTransform(navFlat);
                Text(L"+1", {r.Right() - 40, r.y + 9 - (1 - pulse) * 12, 30, 20}, 12,
                     D2D1::ColorF(accent.r, accent.g, accent.b, pulse), 700);
            }
        }
        if (i == 2) Path(icon, heart, color);
        if (i == 3) Symbol(icon, IconId::Playlist, color);
        if (i == 4) Path(icon, "M8 3H16V8H21V16H16V21H8V16H3V8H8Z", color);
        if (i == 5) Path(icon, "M12 3V16M6 10L12 16L18 10M4 18V22H20V18", color);
        if (i == 6) Symbol(icon, IconId::Settings, color);
        dc->SetTransform(navFlat);
        Text(labels[i], {r.x + 47, r.y, 116, 38}, 13, active ? white : muted, active ? 600 : 400);
        hits_.push_back({r, None, 10000 + i, labels[i]});
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
            brush_->SetColor(MixColor(HomePalette().card, HomePalette().accent, .4f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(Native(layout.search), 12, 12), brush_.Get(), 1);
        }
        Path({mainX + 15, 71, 18, 18}, "M16 16L22 22M18 10A8 8 0 1 1 2 10A8 8 0 1 1 18 10", muted);
        const Rect search = layout.searchText;
        if (model.query.empty())
            Text(L"Найти трек, исполнителя или альбом", search, 14, muted);
        else {
            TextSpec spec;
            spec.size = 14;
            const auto a = std::min<size_t>(in.selectionStart, model.query.size());
            const auto b = std::min<size_t>(in.selectionEnd, model.query.size());
            float x = text_->Measure(model.query.substr(0, a), spec);
            if (in.searchFocused && a != b && x < search.w) {
                float width = text_->Measure(model.query.substr(a, b - a), spec);
                Box({search.x + x, search.y, std::min(width, search.w - x), search.h}, HomePalette().selection, 2);
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
            Glow(banner, accent, 18, 14, .35f + in.energy * .5f);
            Box(banner, D2D1::ColorF(0x251c29), 18);
            const float drift = in.ambient && !reducedMotion_ ? (float)std::sin(clock * .11) : 0;
            // Луна и пластинка «плывут» за курсором - будто стоят в глубине
            const auto depth = Parallax(1, banner);
            // У части тем свой баннер - он уже в нужных цветах и не перекрашивается
            const Picture themed = in.skin == "neon"      ? BannerNeon
                                   : in.skin == "frost"   ? BannerFrost
                                   : in.skin == "sunset"  ? BannerSunset
                                   : in.skin == "minimal" ? BannerMinimal
                                   : in.skin == "glass"   ? BannerGlass
                                                          : kPictures;
            if (auto* own = themed != kPictures ? PictureBitmap(themed) : nullptr)
                Image(own, banner, 18, .5f + drift * .04f + depth.x * .05f, .5f + depth.y * .1f, 1.09f + drift * .03f);
            else
                TintedImage(HeroBitmap(), banner, 18, .5f + drift * .04f + depth.x * .05f, .5f + depth.y * .1f,
                            1.09f + drift * .03f);
            // A horizontal scrim preserves contrast independently of the artwork.
            D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x16151d, .98f)},
                                          {.58f, D2D1::ColorF(0x16151d, .78f)},
                                          {1, D2D1::ColorF(0x16151d, .05f)}};
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> gradient;
            collection = gradients_.Get(dc_, stops, 3);
            if (collection)
                dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(mainX, top), D2D1::Point2F(end, top)),
                    collection.Get(), &gradient);
            if (gradient) dc->FillRoundedRectangle(D2D1::RoundedRect(Native(banner), 18, 18), gradient.Get());
            if (!reducedMotion_) {
                const float sweep = (float)((clock - pageEnteredAt_ - .2) / 1.4);
                Sweep(banner, 18, sweep);
                if (sweep < 1) needsFrames_ = true;
            }
            Text(L"ЛИЧНАЯ ВОЛНА", {mainX + 26, top + 17, mainW - 52, 20}, 9, accent, 700);
            Text(L"На твоей частоте", {mainX + 26, top + (heroH < 160 ? 36 : 44), mainW - 52, 43},
                 heroH < 160 ? 28.f : 34.f, white, 700);
            if (heroH >= 160)
                Text(L"Любимые треки. Новые сочетания.", {mainX + 26, top + 91, mainW - 52, 23}, 12,
                     D2D1::ColorF(0xc6bfcc));
            Button({mainX + 26, top + heroH - 54, 151, 36}, L"Слушать волну", Wave, -1, true);
            Button({mainX + 189, top + heroH - 54, 133, 36}, L"Открыть папку", OpenFolder);
            top += heroH + 22;
            if (bottom > 700 && model.tracks.size() >= 4) {
                // Подборки: коллажи из обложек; щелчок - запустить или открыть
                // Треки подборок пересчитываются только при изменении библиотеки или любимого
                if (mixRevision_ != revision_ || mixCount_ != model.tracks.size() ||
                    mixFavorites_ != model.favorites.size()) {
                    mixRevision_ = revision_;
                    mixCount_ = model.tracks.size();
                    mixFavorites_ = model.favorites.size();
                    std::vector<size_t> order(model.tracks.size());
                    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                    auto take = [&](auto less) {
                        std::vector<size_t> sorted = order;
                        std::partial_sort(sorted.begin(), sorted.begin() + std::min<size_t>(4, sorted.size()),
                                          sorted.end(), less);
                        std::vector<view::HomeTrack> out;
                        for (size_t i = 0; i < sorted.size() && i < 4; ++i) out.push_back(model.tracks[sorted[i]]);
                        return out;
                    };
                    mixPlays_ = take([&](size_t a, size_t b) { return model.tracks[a].plays > model.tracks[b].plays; });
                    mixOld_ = take(
                        [&](size_t a, size_t b) { return model.tracks[a].lastPlayed < model.tracks[b].lastPlayed; });
                    mixFavorites4_.clear();
                    mixFresh_.clear();
                    for (const auto& t : model.tracks) {
                        if (mixFavorites4_.size() < 4 && model.favorites.count(view::HomePathKey(t.path)))
                            mixFavorites4_.push_back(t);
                        if (mixFresh_.size() < 4 && t.plays == 0) mixFresh_.push_back(t);
                    }
                }
                const auto& byPlays = mixPlays_;
                const auto& byOld = mixOld_;
                const auto& favorites = mixFavorites4_;
                const auto& fresh = mixFresh_;
                const float mw = (mainW - 3 * 18) / 4, mh = 96;
                const Rect mixes[] = {{mainX, top, mw, mh},
                                      {mainX + (mw + 18), top, mw, mh},
                                      {mainX + 2 * (mw + 18), top, mw, mh},
                                      {mainX + 3 * (mw + 18), top, mw, mh}};
                MixCard(mixes[0], L"Волна", L"Любимое и забытое под этот час", byPlays, Wave, -1, 0, MixWave);
                MixCard(mixes[1], L"Любимое",
                        favorites.empty() ? L"Отмечайте сердечком" : Count(model.favorites.size()),
                        favorites.empty() ? byPlays : favorites, None, 10002, 1, MixFavorites);
                MixCard(mixes[2], L"Давно не слушали", L"Вспомнить забытое", byOld, WorkspaceCommand,
                        workspace::WaveMoodFirst + 4, 2, MixForgotten);
                MixCard(mixes[3], L"Ещё не играло", fresh.empty() ? L"Всё уже звучало" : L"Новое в коллекции",
                        fresh.empty() ? byOld : fresh, WorkspaceCommand, workspace::WaveMoodFirst + 6, 3, MixFresh);
                top += mh + 14;
                if (bottom > 900) {
                    // «Волна» под настроение - одним щелчком; в окне поменьше этот ряд уступает место коллекции
                    const wchar_t* moods[] = {L"Утро", L"День", L"Вечер", L"Ночь"};
                    const wchar_t* moodHints[] = {L"Что вы слушаете по утрам", L"Что вы слушаете днём",
                                                  L"Что вы слушаете вечером", L"Что вы слушаете ночью"};
                    for (int k = 0; k < 4; ++k)
                        MixCard({mainX + k * (mw + 18), top, mw, 72}, moods[k], moodHints[k], byPlays, WorkspaceCommand,
                                workspace::WaveMoodFirst + k, 4 + k, (Picture)(MixMorning + k));
                    top += 72 + 14;
                }
                top += 12;
            }
        }
        // Шапка раздела: у «Библиотеки», «Любимого» и «Очереди» заголовок лежит на своей картине
        const Picture pageBanner = !model.query.empty()                       ? kPictures
                                   : model.page == view::HomeModel::Library   ? BannerLibrary
                                   : model.page == view::HomeModel::Favorites ? BannerFavorites
                                   : model.page == view::HomeModel::Queue     ? BannerQueue
                                                                              : kPictures;
        auto* pageArt = pageBanner != kPictures && bottom > 620 ? PictureBitmap(pageBanner) : nullptr;
        const Rect bannerRect{mainX, top, mainW, 124};
        if (pageArt) {
            PageBanner(bannerRect, pageArt, 4 + (int)model.page);
            Text(labels[(int)model.page], {mainX + 26, top + 30, mainW - 52, 44}, 32, white, 700);
            top += 124 + 18;
        } else {
            Text(!model.query.empty() ? L"Результаты поиска"
                 : hero               ? L"Твоя коллекция"
                                      : labels[(int)model.page],
                 {mainX, top, mainW - 88, 30}, 22, white, 650);
        }
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
        if (pageArt) {
            Text(Count(displayed_.size()), {mainX + 26, bannerRect.y + 76, mainW - 52, 22}, 12, D2D1::ColorF(0xc6bfcc));
        } else {
            Text(Count(displayed_.size()), {end - 85, top + 5, 85, 22}, 11, muted);
            top += 40;
        }
        const bool listMode = model.page == view::HomeModel::Queue;
        const int columns = listMode ? 1 : std::max(2, (int)((mainW + 18) / (hero && bottom < 590 ? 128.f : 170.f)));
        const float tile = (mainW - (columns - 1) * 18) / columns, row = listMode ? 70.f : tile + 76;
        const float available = std::max(0.f, bottom - 20 - top);
        const int rowCount = ((int)displayed_.size() + columns - 1) / columns;
        ClampScroll(std::max(0.f, rowCount * row - available));
        // Empty content is clipped to the collection viewport, just like track tiles.
        // Края сетки растворяются: строка у нижней панели не обрезается ножом
        PushEdgeFade({mainX, top, mainW, bottom - 16 - top}, scroll_ > 1 ? 18.f : 0.f,
                     rowCount * row - scroll_ > available + 1 ? 44.f : 0.f);
        if (displayed_.empty()) {
            // Пустая страница: луна в кольцах со значком страницы, текст по центру
            const bool favorites = model.page == view::HomeModel::Favorites, searching = !model.query.empty();
            const float cx = mainX + mainW / 2, cyArt = top + std::min(110.f, available * .3f);
            const float breathe = reducedMotion_ ? .5f : (float)std::sin(clock * 1.3) * .5f + .5f;
            if (!reducedMotion_) needsFrames_ = true;
            for (int ring = 3; ring >= 1; --ring) {
                const float r = 40 + ring * 20 + breathe * 3 * ring;
                brush_->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, .05f + (4 - ring) * .045f));
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cyArt), r, r), brush_.Get(), 1.2f);
            }
            const Picture emptyPicture = searching                              ? EmptySearch
                                         : favorites                            ? EmptyFavorites
                                         : model.page == view::HomeModel::Queue ? EmptyQueue
                                                                                : EmptyLibrary;
            if (auto* picture = PictureBitmap(emptyPicture)) {
                // Иллюстрация вместо значка; кольца остаются вокруг неё
                const Rect pic{cx - 92, cyArt - 92, 184, 184};
                Glow({cx - 46, cyArt - 46, 92, 92}, accent, 46, 18 + breathe * 8, .6f + breathe * .4f);
                TintedImage(picture, pic, 0, .5f, .5f, 1);
            } else {
                const Rect disc{cx - 44, cyArt - 44, 88, 88};
                Glow(disc, accent, 44, 16 + breathe * 8, .8f + breathe * .5f);
                Box(disc, MixColor(card, accent, .22f), 44);
                Symbol({cx - 24, cyArt - 24, 48, 48}, IconId::Moon, accent);
                const Rect badge{cx + 22, cyArt + 18, 32, 32};
                Box(badge, accent, 16);
                if (favorites)
                    Path({badge.x + 8, badge.y + 9, 16, 16}, heart, HomePalette().onAccent, true);
                else if (searching)
                    Path({badge.x + 8, badge.y + 8, 16, 16}, "M16 16L22 22M18 10A8 8 0 1 1 2 10A8 8 0 1 1 18 10",
                         white);
                else
                    Path({badge.x + 8, badge.y + 8, 16, 16}, "M12 5V19M5 12H19", HomePalette().onAccent);
            }
            const float ty = cyArt + 124;  // ниже внешнего кольца
            CenterText(searching   ? L"Ничего не найдено"
                       : favorites ? L"Сохраняй то, что нравится"
                                   : L"Здесь начинается твоя музыка",
                       {mainX, ty, mainW, 30}, 20, white, 650);
            CenterText(searching   ? L"Попробуйте другое название или исполнителя."
                       : favorites ? L"Нажмите на сердечко у трека - он появится здесь."
                                   : L"Добавьте файлы, перетащите папку в окно или скачайте по ссылке.",
                       {mainX, ty + 36, mainW, 22}, 12, muted);
            if (!searching && !favorites && available > ty + 120 - top)
                Button({cx - 83, ty + 72, 166, 38}, L"Добавить музыку", OpenFiles, -1, true);
            if (favorites && available > ty + 120 - top)
                Button({cx - 83, ty + 72, 166, 38}, L"К библиотеке", WorkspaceCommand, workspace::OpenLibrary, true);
        }
        int first = std::max(0, (int)(std::max(0.f, scroll_) / row)) * columns;
        int last = std::min((int)displayed_.size(), first + ((int)(available / row) + 2) * columns);
        for (int i = first; i < last; ++i) {
            float x = mainX + (i % columns) * (tile + 18), y = top + (i / columns) * row - scroll_;
            const auto& t = displayed_[i];
            if (listMode) {
                Rect entry{x, y, mainW, 64};
                const bool current = !in.current.empty() && PathKey(in.current) == PathKey(t.path);
                const bool hover = entry.Contains(mouse_.x, mouse_.y);
                if (current || hover) Box(entry, current ? HomePalette().softer : card, 12);
                Text(std::to_wstring(i + 1), {x + 9, y + 20, 25, 24}, 11, current ? accent : muted);
                Art(t, {x + 39, y + 8, 48, 48});
                Text(t.title, {x + 101, y + 10, mainW - 225, 24}, 13, current ? accent : white, 600);
                Text(t.artist.empty() ? L"Неизвестный исполнитель" : t.artist, {x + 101, y + 35, mainW - 225, 20}, 11,
                     muted);
                Text(t.duration > 0 ? Time(t.duration) : L"—", {end - 105, y + 22, 49, 22}, 11, muted);
                Rect hit{x, std::max(top, y), mainW, std::max(0.f, std::min(bottom - 16, y + 64) - std::max(top, y))};
                if (hit.h > 0) hits_.push_back({hit, PlayTrack, i});
                const bool favorite = model.favorites.count(PathKey(t.path));
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
            bool current = !in.current.empty() && PathKey(in.current) == PathKey(t.path);
            bool hover = hit.Contains(mouse_.x, mouse_.y);
            // Лесенка при входе на страницу и подъём под курсором
            const float enter = Enter(i - first);
            const float lift = Hover(700000 + i, hover);
            y += (1 - enter) * 22 - lift * 5;
            if (enter < 1) PushFade(enter);
            const float flash = current ? std::max(0.f, 1 - (float)(clock - trackChangedAt_) / .9f) : 0;
            if (flash > 0) needsFrames_ = true;
            if (lift > .01f || current)
                Glow({x, y, tile, tile}, accent, 12, 12 + flash * 14, current ? .7f + flash * 1.6f : lift * .8f);
            if (hover || current) Box({x - 6, y - 6, tile + 12, tile + 72}, current ? HomePalette().softer : card, 15);
            D2D1_MATRIX_3X2_F flat;
            dc->GetTransform(&flat);
            if (hover && !reducedMotion_ && !drag_.active) {
                // Лёгкий наклон к курсору - плитка будто поворачивается навстречу
                const float dx = (mouse_.x - (x + tile / 2)) / tile, dy = (mouse_.y - (y + tile / 2)) / tile;
                dc->SetTransform(D2D1::Matrix3x2F::Skew(dy * -3.f * lift, dx * 3.f * lift,
                                                        D2D1::Point2F(x + tile / 2, y + tile / 2)) *
                                 flat);
            }
            if (drag_.active && drag_.hit == i) PushFade(.35f);
            Art(t, {x, y, tile, tile});
            if (drag_.active && drag_.hit == i) PopFade();
            dc->SetTransform(flat);
            Spotlight({x, y, tile, tile}, 12, lift);
            if (current && in.playback.playing) {
                Box({x + 10, y + tile - 34, 38, 24}, D2D1::ColorF(0x101116, .82f), 12);
                Equalizer({x + 19, y + tile - 29, 20, 14}, in, accent);
            }
            if (hit.h > 0) hits_.push_back({hit, PlayTrack, i});
            Text(t.title, {x, y + tile + 10, tile, 22}, 13, current ? accent : white, 600);
            Text(t.artist.empty() ? L"Неизвестный исполнитель" : t.artist, {x, y + tile + 33, tile, 20}, 11, muted);
            const bool favorite = model.favorites.count(PathKey(t.path));
            Rect love{x + tile - 33, y + 8, 26, 26};
            Box(love, D2D1::ColorF(0x15161d, .88f), 13);
            Path({love.x + 6, love.y + 6, 14, 14}, heart, favorite ? accent : white, favorite);
            if (love.y >= top && love.Bottom() <= bottom - 16) hits_.push_back({love, Favorite, i});
            if (hover || current) {
                Rect play{x + tile - 49, y + tile - 49, 39, 39};
                Box(play, accent, 20);
                Glyph(play, current && in.playback.playing ? 1 : 0, HomePalette().onAccent);
                if (current && play.y >= top && play.Bottom() <= bottom - 16) hits_.push_back({play, Toggle});
            }
            if (enter < 1) PopFade();
        }
        PopEdgeFade();
        if (rowCount * row > available && available > 0) {
            float thumb = std::max(24.f, available * available / (rowCount * row));
            float y = top + (available - thumb) * std::clamp(scroll_ / (rowCount * row - available), 0.f, 1.f);
            Box({end + 12, y, 3, thumb}, D2D1::ColorF(0x4c4656), 2);
        }
        if (layout.queue) {
            const float x = end + 32, qw = w - x - 24;
            queueRect_ = {x - 14, 58, qw + 26, bottom - 76};
            const bool dropHere = drag_.active && queueRect_.Contains(mouse_.x, mouse_.y);
            if (dropHere) Glow(queueRect_, accent, 16, 10, .9f);
            Box(queueRect_, D2D1::ColorF(surface.r, surface.g, surface.b, .8f), 16);
            if (dropHere) {
                brush_->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, .7f));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(Native(queueRect_), 16, 16), brush_.Get(), 1.5f);
            }
            Text(L"Далее", {x, 77, qw, 26}, 18, white, 650);
            int start = std::max(0, in.playback.index + 1);
            queueStart_ = start;
            Text(Count(in.queue.size() > (size_t)start ? in.queue.size() - start : 0), {x, 107, qw, 20}, 11, muted);
            int shown = 0;
            for (int i = start; i < (int)in.queue.size() && 150 + (shown + 1) * 68 < bottom - 24; ++i, ++shown) {
                const float enter = Enter(shown + 2);
                float y = 150.f + shown * 68;
                const auto& t = in.queue[i];
                Rect r{x - 5, y - 5, qw + 10, 61};
                hits_.push_back({r, PlayTrack, -i - 2});
                const float slide = (1 - enter) * 26;
                if (enter < 1) PushFade(enter);
                if (r.Contains(mouse_.x, mouse_.y)) Box(r, card, 9);
                Art(t, {x + slide, y, 46, 46});
                Text(t.title, {x + 58 + slide, y + 1, qw - 58, 23}, 12, white, 600);
                Text(t.artist.empty() ? L"Локальный трек" : t.artist, {x + 58 + slide, y + 25, qw - 58, 20}, 10, muted);
                if (enter < 1) PopFade();
            }
            queueShown_ = shown;
            {
                // Копия строк нужна только перетаскиванию; пока они те же, не копируем
                const auto from = in.queue.begin() + std::min<size_t>(start, in.queue.size());
                const auto to = in.queue.begin() + std::min<size_t>(start + shown, in.queue.size());
                const bool same = std::equal(from, to, queueVisible_.begin(), queueVisible_.end(),
                                             [](const view::HomeTrack& a, const view::HomeTrack& b) {
                                                 return a.path.native() == b.path.native() && a.title == b.title &&
                                                        a.artist == b.artist;
                                             });
                if (!same) queueVisible_.assign(from, to);
            }
            if (dropHere) {
                // Линия вставки: сюда встанет трек
                const float ly = queueTop_ + DropSlot() * 68.f - 7 - (float)queueStart_ * 68.f;
                Box({x - 4, ly - 1, qw + 8, 3}, accent, 2);
                Glow({x - 4, ly - 1, qw + 8, 3}, accent, 2, 6, 1);
            }
            if (!shown) {
                Symbol({x + 4, 165, 30, 30}, IconId::Playlist, muted);
                Text(L"Пусть музыка продолжается", {x, 214, qw, 52}, 15, white, 600);
                Text(L"Выберите трек в коллекции - следующие появятся здесь.", {x, 273, qw, 68}, 12, muted);
            }
        }
    }
    // «Сейчас играет» поверх всего, кроме нижней панели; пока открыто, щелчки под ним не доходят
    if (const float now = NowPlayingAmount(); now > 0) {
        if (now > .5f) hits_.clear();
        PaintNowPlaying(in, now, w, bottom);
        if (now > .5f) {
            IconButton({w - 124, 7, 34, 30}, IconId::Minimize, Minimize);
            IconButton({w - 84, 7, 34, 30}, IconId::Expand, Maximize);
            IconButton({w - 44, 7, 34, 30}, IconId::Close, Close);
        }
    }
    {
        // Нижняя панель: стеклянный градиент и светящаяся кромка над кнопкой воспроизведения
        D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x1b1b25, .94f)}, {1, D2D1::ColorF(0x111118, .98f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> panel;
        collection = gradients_.Get(dc_, stops, 2);
        if (collection)
            dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, bottom), D2D1::Point2F(0, bottom + 112)),
                collection.Get(), &panel);
        if (panel) dc->FillRectangle(D2D1::RectF(0, bottom, w, bottom + 112), panel.Get());
        Box({0, bottom, w, 1}, D2D1::ColorF(0x2b2a35), 0);
        const float cx = layout.transport.CenterX();
        D2D1_GRADIENT_STOP edge[] = {{0, {accent.r, accent.g, accent.b, 0}},
                                     {.5f, {accent.r, accent.g, accent.b, 1}},
                                     {1, {accent.r, accent.g, accent.b, 0}}};
        ComPtr<ID2D1GradientStopCollection> edgeStops;
        ComPtr<ID2D1LinearGradientBrush> edgeBrush;
        edgeStops = gradients_.Get(dc_, edge, 3);
        if (edgeStops)
            dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - 260, 0), D2D1::Point2F(cx + 260, 0)),
                D2D1::BrushProperties(.55f + in.energy * .45f), edgeStops.Get(), &edgeBrush);
        if (edgeBrush) dc->FillRectangle(D2D1::RectF(cx - 260, bottom, cx + 260, bottom + 1), edgeBrush.Get());
    }
    const view::HomeTrack& current = currentTrack_;
    // Новый трек въезжает справа и проявляется
    const float arrive = reducedMotion_ ? 1 : std::clamp((float)(clock - trackChangedAt_) / .45f, 0.f, 1.f);
    const float arriveEase = 1 - (1 - arrive) * (1 - arrive) * (1 - arrive);
    if (arrive < 1) {
        needsFrames_ = true;
        PushFade(.25f + .75f * arriveEase);
        dc->SetTransform(D2D1::Matrix3x2F::Translation((1 - arriveEase) * 22, 0));
    }
    if (!current.path.empty()) {
        if (in.playback.playing) Glow({22, bottom + 26, 56, 56}, accent, 10, 8, .5f + in.energy);
        Art(current, {22, bottom + 26, 56, 56});
        if (in.playback.playing) {
            Box({50, bottom + 62, 26, 18}, D2D1::ColorF(0x101116, .85f), 9);
            Equalizer({56, bottom + 65, 14, 12}, in, accent);
        }
    } else {
        Box({22, bottom + 26, 56, 56}, card, 10);
        Symbol({38, bottom + 42, 24, 24}, IconId::Moon, muted);
    }
    Text(in.title.empty() ? L"Выберите свой первый трек" : Scramble(in.title, trackChangedAt_),
         {layout.trackText.x, layout.trackText.y, layout.trackText.w, 23}, 13, white, 600);
    Text(in.artist.empty() ? L"Твоя музыка всегда рядом" : in.artist,
         {layout.trackText.x, layout.trackText.y + 25, layout.trackText.w, 20}, 11, muted);
    // Обложка и название открывают «Сейчас играет»
    if (!in.current.empty()) hits_.push_back({{22, bottom + 20, layout.trackText.Right() - 22, 68}, NowPlaying});
    if (arrive < 1) {
        dc->SetTransform(D2D1::IdentityMatrix());
        PopFade();
    }
    const auto control = layout.transport;
    IconButton({control.x, control.y + 4, 34, 34}, IconId::Shuffle, Shuffle, in.playback.shuffle);
    IconButton({control.x + 48, control.y + 4, 34, 34}, IconId::Prev, Previous);
    Rect play{control.CenterX() - 22, control.y, 44, 44};
    {
        const auto pull = Magnet(600003, play, 40, .16f);
        play.x += pull.x;
        play.y += pull.y;
    }
    {
        // Кольцо вокруг кнопки дышит с басом
        const float pulse = in.playback.playing ? .55f + in.energy * 1.4f + in.beat * 1.2f : .25f;
        Glow(play, accent, 22, 10 + in.energy * 8 + in.beat * 10, pulse);
        const float over = Hover(600001, play.Contains(mouse_.x, mouse_.y));
        const float grow = over * 2 + (in.playback.playing ? in.energy * 2.5f + in.beat * 2.5f : 0);
        play = {play.x - grow, play.y - grow, play.w + grow * 2, play.h + grow * 2};
    }
    Box(play, play.Contains(mouse_.x, mouse_.y) ? HomePalette().accentHover : accent, play.w / 2);
    Glyph(play, in.playback.playing ? 1 : 0, HomePalette().onAccent);
    hits_.push_back({play, Toggle});
    IconButton({control.x + 170, control.y + 4, 34, 34}, IconId::Next, Next);
    IconButton({control.x + 218, control.y + 4, 34, 34}, IconId::Repeat, Repeat, in.playback.repeat != pm::Repeat::Off);
    if (in.playback.repeat == pm::Repeat::One) Text(L"1", {control.x + 231, control.y + 2, 13, 14}, 8, accent, 700);
    const Rect seek = layout.seek;
    const float sy = seek.CenterY() - 2;
    Box({seek.x, sy, seek.w, 4}, D2D1::ColorF(0x3c3a49), 2);
    float position =
        in.playback.duration > 0 ? std::clamp((float)(in.playback.position / in.playback.duration), 0.f, 1.f) : 0;
    if (position > 0) {
        const float fill = seek.w * position;
        Glow({seek.x, sy, fill, 4}, accent, 2, 6, .8f);
        D2D1_GRADIENT_STOP stops[] = {{0, HomePalette().accent2}, {1, accent}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        collection = gradients_.Get(dc_, stops, 2);
        if (collection)
            dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(seek.x, 0), D2D1::Point2F(seek.x + fill, 0)),
                collection.Get(), &gradient);
        if (gradient)
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(seek.x, sy, seek.x + fill, sy + 4), 2, 2),
                                     gradient.Get());
    }
    {
        const float knob = Hover(600002, seek.Contains(mouse_.x, mouse_.y));
        const float size = 6 + knob * 6 + (in.playback.playing ? in.energy * 3 : 0);
        const float kx = seek.x + seek.w * position;
        if (in.playback.playing || knob > .01f) {
            Glow({kx - size / 2, sy + 2 - size / 2, size, size}, accent, size / 2, 6, .9f);
            Box({kx - size / 2, sy + 2 - size / 2, size, size}, white, size / 2);
        }
    }
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
    if (morphArt_.w > 0) {
        // Летящая обложка «Сейчас играет» - над нижней панелью
        const bool round = in.coverFrame == 3 && morphGlow_ > .05f;
        Glow(morphArt_, moodNow_, round ? morphArt_.w / 2 : 12 + morphArt_.w * .05f, 10 + morphArt_.w * .08f,
             morphGlow_);
        const view::HomeTrack& flying = currentTrack_;
        // В полёте за обложкой тянется её след
        const float ghostFade[] = {.1f, .22f};
        for (int g = 0; g < 2; ++g) {
            if (morphGhost_[g].w <= 0) continue;
            PushFade(ghostFade[g]);
            Art(flying, morphGhost_[g], 12);
            PopFade();
            morphGhost_[g] = {};
        }
        // Рамка проявляется на подлёте: крошечная рамка на мини-обложке выглядела бы соринкой
        const float frameFade = std::clamp((morphGlow_ / (.9f + in.energy) - .55f) / .45f, 0.f, 1.f);
        if (frameFade > 0 && HasFrame(in.coverFrame)) {
            // Окно рамки шире обложки (кассета) - остаток окна заполняет тёмное стекло
            view::Rect window;
            FrameDest(morphArt_, in.coverFrame, &window);
            if (window.w > morphArt_.w + 2 || window.h > morphArt_.h + 2)
                Box(window, D2D1::ColorF(0x0b0b10, .92f * frameFade), 6);
        }
        // Под виниловое кольцо обложка круглая
        Art(flying, morphArt_, round ? morphArt_.w / 2 : 12.f);
        if (frameFade > 0) {
            if (frameFade < 1) PushFade(frameFade);
            CoverFrame(morphArt_, in.coverFrame, &in);
            if (frameFade < 1) PopFade();
        }
        morphArt_ = {};
    }
    PaintDrag(in);
    PaintRipples();
    PaintParticles();
    PaintStory(in);
    PaintToast();
    PaintPalette(in);
    PaintFilm(in, w, bottom);
    PaintFocus();
    if (offscreen) ComposeFrame(dc, in.size, side, bottom);
}

bool Home::BeginOffscreen(ID2D1DeviceContext* dc) {
    screen_.Reset();
    dc->GetTarget(&screen_);
    ComPtr<ID2D1Bitmap1> target;
    if (!screen_ || FAILED(screen_.As(&target))) return false;
    const auto pixels = target->GetPixelSize();
    float dpiX = 96, dpiY = 96;
    dc->GetDpi(&dpiX, &dpiY);
    for (auto& frame : frames_) {
        if (frame && frame->GetPixelSize().width == pixels.width && frame->GetPixelSize().height == pixels.height)
            continue;
        frame.Reset();
        previousFrame_ = -1;
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            dpiX, dpiY);
        if (FAILED(dc->CreateBitmap(pixels, nullptr, 0, props, &frame))) return false;
    }
    dc->SetTarget(frames_[frameIndex_].Get());
    return true;
}

void Home::ComposeFrame(ID2D1DeviceContext* dc, view::Size size, float sidebar, float bottom) {
    dc->SetTarget(screen_.Get());
    dc->SetTransform(D2D1::IdentityMatrix());
    const auto all = D2D1::RectF(0, 0, size.w, size.h);
    dc->DrawBitmap(frames_[frameIndex_].Get(), all, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, all);
    if (previousFrame_ < 0 || !frames_[previousFrame_]) return;
    const float u = std::clamp((float)((clock_ - pageSwitchedAt_) / .42), 0.f, 1.f);
    if (u >= 1) {
        previousFrame_ = -1;
        return;
    }
    needsFrames_ = true;
    const float e = 1 - (1 - u) * (1 - u) * (1 - u);
    // Только рабочая область: боковая панель и нижняя панель у страниц общие и не двигаются
    const auto area = D2D1::RectF(sidebar + 1, 44, size.w, bottom);
    const float cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
    dc->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(1 - .04f * e, 1 - .04f * e, D2D1::Point2F(cx, cy)) *
                     D2D1::Matrix3x2F::Translation(0, -pageDirection_ * 38.f * e));
    dc->DrawBitmap(frames_[previousFrame_].Get(), area, std::pow(1 - e, 1.4f), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                   area);
    dc->SetTransform(D2D1::IdentityMatrix());
    dc->PopAxisAlignedClip();
}
}  // namespace ui
