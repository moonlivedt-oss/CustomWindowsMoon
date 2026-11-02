#pragma once
#include "ui/painter.h"

namespace ui {
// Uses the actual compact-player renderer; never opens a window or plays audio.
class SettingsPreview {
public:
    void Init(Renderer* renderer, TextSystem* text, IconSet* icons, CoverCache* covers);
    void Apply(const theme::Theme& theme, const theme::LayoutGeometry& layout, bool reduce, double now);
    ID2D1Bitmap1* Render(ID2D1DeviceContext* dc, const view::TrackText& track, double now,
                        bool mini, bool animate, bool remaining);
    bool NeedsFrames() const { return view_.NeedsFrames(); }
    view::Size Size() const { return size_; }
private:
    Renderer* renderer_ = nullptr;
    Painter painter_;
    view::PlayerView view_;
    ComPtr<ID2D1Bitmap1> target_;
    uint64_t generation_ = 0, serial_ = ~0ull;
    view::Size size_{420, 680};
};
}
