#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "ui/covers.h"
#include <d2d1helper.h>
#include "ui/icons.h"
#include "view/home_layout.h"
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
        Close,
        WorkspaceCommand
    };
    struct Hit {
        view::Rect rect;
        Action action;
        int index = -1;
    };
    struct Control {
        std::wstring label;
        int command = 0;
        bool active = false;
        bool toggle = false;
        D2D1_COLOR_F background = D2D1::ColorF(0x181920);
        D2D1_COLOR_F accent = D2D1::ColorF(0xed647b);
    };
    struct Card {
        std::wstring title, subtitle, status;
        std::vector<Control> controls;
        float progress = -1;
        bool error = false;
        bool themeGallery = false;
    };
    struct Input {
        view::Size size;
        view::PlaybackSnapshot playback;
        std::wstring title, artist;
        std::filesystem::path current;
        std::vector<view::HomeTrack> queue;
        std::vector<Card> cards;
        ID2D1Bitmap1* settingsPreview = nullptr;
        view::Size previewSize{420, 680};
        int settingsSection = 0;
        bool previewMini = false, previewLarge = false, reducedMotion = false;
        bool canUndo = false;
        std::wstring notice, downloadText;
        bool downloadEnabled = false, downloadFocused = false;
        bool scanning = false;
        bool searchFocused = false;
        unsigned selectionStart = 0, selectionEnd = 0;
        unsigned downloadStart = 0, downloadEnd = 0;
    };
    Home() = default;
    ~Home();
    view::HomeModel model;
    void Init(HWND hwnd, Renderer* renderer, TextSystem* text);
    void Shutdown();
    void Paint(ID2D1DeviceContext* dc, const Input& input);
    Hit At(view::Point p) const;
    bool NeedsFrames() const { return needsFrames_; }
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
    void PaintWorkspace(view::Rect content, const Input& input);
    void PaintSettings(view::Rect content, const Input& input);
    float Hover(int key, bool over);
    void RequestArt(const view::HomeTrack& track);
    void DrainArt();
    void ArtLoop();
    void Box(view::Rect r, D2D1_COLOR_F c, float radius = 12);
    void Text(const std::wstring& s, view::Rect r, float size, D2D1_COLOR_F c, float weight = 400);
    void Button(view::Rect r, const std::wstring& s, Action a, int index = -1, bool primary = false);
    void Art(const view::HomeTrack& t, view::Rect r);
    void Glyph(view::Rect r, int kind, D2D1_COLOR_F color);
    void Symbol(view::Rect r, IconId id, D2D1_COLOR_F color);
    void Path(view::Rect r, const std::string& path, D2D1_COLOR_F color, bool fill = false);
    void Image(ID2D1Bitmap1* bitmap, view::Rect r, float radius = 12, float anchor = .5f);
    void IconButton(view::Rect r, IconId icon, Action action, bool active = false);
    IconSet icons_;
    std::map<std::string, ComPtr<ID2D1PathGeometry>> paths_;
    ComPtr<IWICBitmapSource> heroSource_;
    ComPtr<ID2D1Bitmap1> heroBitmap_;
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
    bool needsFrames_ = false;
    double lastPaint_ = 0;
    float frameDelta_ = .016f;
    std::map<int, float> hover_;
    bool reducedMotion_ = false;
};
}  // namespace ui
