#include "ui/settings_preview.h"
#include <cmath>

namespace ui {
void SettingsPreview::Init(Renderer* renderer, TextSystem* text, IconSet* icons, CoverCache* covers) {
    renderer_ = renderer;
    painter_.Init(renderer, text, icons, covers);
}
void SettingsPreview::Apply(const theme::Theme& theme, const theme::LayoutGeometry& layout, bool reduce, double now) {
    const bool animate = view_.hasTheme && !reduce && theme.settings.motion != "none";
    // Apply immediately so a rapid second selection or Undo cannot revive an older queued theme.
    view_.ApplyTheme(theme, layout, reduce, now, false);
    view_.windowAlpha.Snap(animate ? .82f : 1.f);
    if (animate) view_.windowAlpha.To(1.f, .22, view_.motion.out, view_.timeline);
}
ID2D1Bitmap1* SettingsPreview::Render(ID2D1DeviceContext* dc, const view::TrackText& track, double now,
                                     bool mini, bool animate, bool remaining) {
    if (!renderer_ || !view_.hasTheme) return nullptr;
    if (serial_ != track.serial) {
        view_.OnTrackChanged(track, 1, now, false);
        serial_ = track.serial;
    }
    if (view_.mini != mini) {
        // A thumbnail has a fixed-size canvas: do not crop a full window into the mini canvas mid-morph.
        view_.RestoreModes(mini, false);
        const bool move = animate && !view_.motion.reduce;
        view_.windowAlpha.Snap(move ? .72f : 1.f);
        view_.windowScale.Snap(move ? .97f : 1.f);
        if (move) {
            view_.windowAlpha.To(1.f, view_.motion.base, view_.motion.out, view_.timeline);
            view_.windowScale.To(1.f, view_.motion.base, view_.motion.out, view_.timeline);
        }
    }
    view_.showRemaining = remaining;
    view::PlaybackSnapshot snapshot;
    snapshot.hasTrack = true;
    snapshot.playing = animate;
    snapshot.duration = 213;
    snapshot.position = animate ? 42 + std::fmod(now * 3, 100.) : 42;
    snapshot.volume = .65f;
    std::vector<float> spectrum(64);
    for (size_t i = 0; i < spectrum.size(); ++i)
        spectrum[i] = animate ? .12f + .55f * std::abs(std::sin(float(now * 2.5 + i * .29))) : .06f;
    view_.Update(now, snapshot, &spectrum, true);
    size_ = mini ? view::Size{view::kMiniWidth, view::kMiniHeight}
                 : view_.layout->WindowSize(view_.layoutInput, false);
    const auto pixels = D2D1::SizeU((UINT)std::ceil(size_.w * 2), (UINT)std::ceil(size_.h * 2));
    if (generation_ != renderer_->DeviceGeneration() ||
        (target_ && (target_->GetPixelSize().width != pixels.width || target_->GetPixelSize().height != pixels.height)))
        target_.Reset();
    if (!target_) {
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 192, 192);
        if (FAILED(dc->CreateBitmap(pixels, nullptr, 0, props, &target_))) return nullptr;
        generation_ = renderer_->DeviceGeneration();
    }
    ComPtr<ID2D1Image> previous;
    D2D1_MATRIX_3X2_F transform;
    float dpiX, dpiY;
    dc->GetTarget(&previous);
    dc->GetTransform(&transform);
    dc->GetDpi(&dpiX, &dpiY);
    dc->SetTarget(target_.Get());
    dc->SetDpi(192, 192);
    dc->SetTransform(D2D1::IdentityMatrix());
    dc->Clear(D2D1::ColorF(0x15161d));
    PaintInput input;
    input.window = size_;
    painter_.Paint(dc, view_, input);
    dc->SetTarget(previous.Get());
    dc->SetDpi(dpiX, dpiY);
    dc->SetTransform(transform);
    return target_.Get();
}
}
