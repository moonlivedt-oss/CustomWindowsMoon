#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "ui/covers.h"
#include "ui/renderer.h"
#include "ui/text.h"
#include "view/home_model.h"
#include "view/player_view.h"

namespace ui {
class Home {
public:
    enum Action {
        None,
        Shuffle,
        Repeat,
        PlayTrack,
        Favorite,
        Toggle,
        Next,
        Previous,
        Seek,
        Volume,
        OpenFiles,
        OpenFolder,
        Wave,
        Compact,
        Menu,
        Minimize,
        Maximize,
        Close
    };
    struct Hit {
        view::Rect rect;
        Action action;
        int index = -1;
    };
    struct Input {
        view::Size size;
        view::PlaybackSnapshot playback;
        std::wstring title, artist;
        std::filesystem::path current;
        std::vector<view::HomeTrack> queue;
        bool scanning = false;
        bool searchFocused = false;
        unsigned selectionStart = 0, selectionEnd = 0;
    };
    Home() = default;
    ~Home();
    view::HomeModel model;
    void Init(HWND hwnd, Renderer* renderer, TextSystem* text);
    void Shutdown();
    void Paint(ID2D1DeviceContext* dc, const Input& input);
    Hit At(view::Point p) const;
    void Move(view::Point p) { mouse_ = p; }
    void Scroll(float delta) { scroll_ = std::max(0.f, scroll_ - delta * 100); }
    void ResetScroll() { scroll_ = 0; }
    void InvalidateCatalog() { ++revision_; }
    static constexpr UINT kArtReady = WM_APP + 27;
    const std::vector<view::HomeTrack>& Displayed() const { return displayed_; }
    float Fraction(view::Point p, Action a) const;
    void Navigate(int page) {
        model.page = (view::HomeModel::Page)page;
        scroll_ = 0;
    }

private:
    void RequestArt(const view::HomeTrack& track);
    void DrainArt();
    void ArtLoop();
    void Box(view::Rect r, D2D1_COLOR_F c, float radius = 12);
    void Text(const std::wstring& s, view::Rect r, float size, D2D1_COLOR_F c, float weight = 400);
    void Button(view::Rect r, const std::wstring& s, Action a, int index = -1, bool primary = false);
    void Art(const view::HomeTrack& t, view::Rect r);
    void Glyph(view::Rect r, int kind, D2D1_COLOR_F color);
    struct Artwork {
        std::wstring key;
        std::vector<uint8_t> bytes;
        std::string title;
    };
    Renderer* renderer_ = nullptr;
    TextSystem* text_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush_;
    uint64_t generation_ = 0;
    CoverCache art_;
    std::vector<Hit> hits_;
    std::vector<view::HomeTrack> displayed_;
    uint64_t revision_ = 1, renderedRevision_ = 0;
    int renderedPage_ = -1;
    std::wstring renderedQuery_;
    view::Point mouse_{-1, -1};
    float scroll_ = 0;
    HWND hwnd_ = nullptr;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<view::HomeTrack> pending_;
    std::vector<Artwork> ready_;
    std::set<std::wstring> requested_;
    std::thread worker_;
    bool stop_ = false;
};
}  // namespace ui
