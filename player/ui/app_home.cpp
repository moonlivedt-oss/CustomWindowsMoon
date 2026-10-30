#include "ui/app.h"
#include "core/text.h"
#include <commctrl.h>
#include <fstream>
#include <map>

namespace ui {
LRESULT CALLBACK App::SearchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            SetWindowTextW(hwnd, L"");
            SetFocus(app->hwnd_);
            return 0;
        }
        if (wp == 'H' && GetKeyState(VK_CONTROL) < 0) {
            app->ToggleHome();
            return 0;
        }
    }
    auto result = CallWindowProcW(app->homeSearchProc_, hwnd, msg, wp, lp);
    if (msg == WM_KEYUP || msg == WM_LBUTTONUP || msg == WM_SETFOCUS || msg == WM_KILLFOCUS) app->Invalidate();
    return result;
}
namespace {
view::HomeTrack Track(const std::filesystem::path& path, const pm::TrackMeta& meta) {
    view::HomeTrack out;
    out.path = path;
    out.title = meta.title.empty() ? path.stem().wstring() : text::Utf8ToWide(meta.title);
    out.artist = text::Utf8ToWide(meta.artist);
    out.album = text::Utf8ToWide(meta.album);
    out.duration = meta.duration;
    return out;
}
}  // namespace
void App::RefreshHome() {
    home_.InvalidateCatalog();
    home_.model.tracks.clear();
    homeInput_.queue.clear();
    std::set<std::filesystem::path> seen;
    for (const auto& t : library_.Tracks()) {
        auto row = Track(t.path, t.meta);
        row.plays = t.plays;
        row.lastPlayed = t.lastPlayed;
        home_.model.tracks.push_back(std::move(row));
        seen.insert(t.path);
    }
    const auto& pl = player_.GetPlaylist();
    for (int i = 0; i < pl.Size(); ++i) {
        auto t = Track(pl.At(i).path, pl.At(i).meta);
        homeInput_.queue.push_back(t);
        if (seen.insert(t.path).second) home_.model.tracks.push_back(std::move(t));
    }
}
void App::LoadHomePreferences() {
    auto file = statePath_.parent_path() / L"home.json";
    std::ifstream stream(file, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(stream)), {});
    json::Value document;
    if (json::Parse(content, document) && document.IsObject()) {
        homeMode_ = document.Get("dashboard").AsBool(true);
        homeSize_.w = (float)std::clamp(document.Get("width").AsNumber(1200), 900.0, 2400.0);
        homeSize_.h = (float)std::clamp(document.Get("height").AsNumber(800), 640.0, 1600.0);
        for (const auto& path : document.Get("favorites").Items())
            if (path.IsString()) home_.model.favorites.insert(view::HomePathKey(text::Utf8ToPath(path.AsString())));
    }
    homeSearch_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 1, 1, hwnd_,
                                  (HMENU)3001, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(homeSearch_, EM_SETLIMITTEXT, 180, 0);
    SetWindowLongPtrW(homeSearch_, GWLP_USERDATA, (LONG_PTR)this);
    homeSearchProc_ = (WNDPROC)SetWindowLongPtrW(homeSearch_, GWLP_WNDPROC, (LONG_PTR)&App::SearchProc);
    SendMessageW(homeSearch_, EM_SETCUEBANNER, FALSE, (LPARAM)L"Поиск по названию, исполнителю или альбому");
    LayoutHomeSearch();
}
void App::SaveHomePreferences() {
    json::Value prefs = json::Value::MakeObject();
    prefs["dashboard"] = homeMode_;
    prefs["width"] = homeSize_.w;
    prefs["height"] = homeSize_.h;
    prefs["favorites"] = json::Value::MakeArray();
    for (const auto& path : home_.model.favorites) prefs["favorites"].Push(text::WideToUtf8(path));
    auto file = statePath_.parent_path() / L"home.json", tmp = file;
    tmp += L".tmp";
    {
        std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        const auto data = json::Dump(prefs);
        stream.write(data.data(), data.size());
        if (!stream) return;
    }
    MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
void App::LayoutHomeSearch() {
    if (!homeSearch_) return;
    if (!homeMode_) {
        ShowWindow(homeSearch_, SW_HIDE);
        return;
    }
    const auto size = ClientDip();
    const float end = size.w >= 1100 ? size.w - 258 : size.w - 26;
    SetWindowPos(homeSearch_, nullptr, ToPx(247), ToPx(67), ToPx(std::max(100.f, end - 263)), ToPx(24),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    HFONT font = CreateFontW(-ToPx(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(homeSearch_, WM_SETFONT, (WPARAM)font, TRUE);
    if (homeSearchFont_) DeleteObject(homeSearchFont_);
    homeSearchFont_ = font;
    ShowWindow(homeSearch_, SW_SHOW);
}
void App::SetHomeWindowStyle() {
    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    if (homeMode_)
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    else
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void App::ToggleHome() {
    if (homeMode_ && !IsZoomed(hwnd_)) homeSize_ = ClientDip();
    homeMode_ = !homeMode_;
    ResetTickTimer();
    if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetHomeWindowStyle();
    if (view_.mini) view_.ToggleMini(Now());
    ResizeWindow(homeMode_ ? homeSize_
                           : (view_.layout ? view_.layout->WindowSize(view_.layoutInput, view_.playlistOpen)
                                           : view::Size{420, 680}));
    LayoutHomeSearch();
    ApplyTopmost();
    SaveHomePreferences();
    SetFocus(hwnd_);
    Invalidate();
}
void App::HomeClick(view::Point point) {
    auto hit = home_.At(point);
    if (hit.action == Home::None && hit.index >= 10000 && hit.index < 10004) {
        home_.Navigate(hit.index - 10000);
        SetFocus(hwnd_);
        Invalidate();
        return;
    }
    switch (hit.action) {
        case Home::Shuffle:
            ToggleShuffle();
            break;
        case Home::Repeat:
            CycleRepeat();
            break;
        case Home::PlayTrack: {
            if (hit.index <= -2) {
                PlayIndex(-hit.index - 2);
                break;
            }
            const auto& displayed = home_.Displayed();
            if (hit.index < 0 || hit.index >= (int)displayed.size()) break;
            const auto path = displayed[hit.index].path;
            const auto& pl = player_.GetPlaylist();
            int found = -1;
            for (int i = 0; i < pl.Size(); ++i)
                if (pl.At(i).path == path) {
                    found = i;
                    break;
                }
            if (found >= 0)
                PlayIndex(found);
            else {
                std::vector<pm::PlaylistEntry> entries;
                entries.reserve(displayed.size());
                for (const auto& t : displayed)
                    entries.push_back({t.path,
                                       {text::WideToUtf8(t.title), text::WideToUtf8(t.artist),
                                        text::WideToUtf8(t.album), t.duration}});
                const int index = hit.index;
                player_.LoadTracks(entries, false);
                player_.OpenAt(index, 0, true);
                RebuildRows();
                playlistFromLibrary_ = false;
            }
            break;
        }
        case Home::Favorite:
            if (hit.index >= 0 && hit.index < (int)home_.Displayed().size()) {
                home_.model.ToggleFavorite(home_.Displayed()[hit.index].path);
                home_.InvalidateCatalog();
                SaveHomePreferences();
            }
            break;
        case Home::Toggle:
            TogglePlay();
            break;
        case Home::Previous:
            Prev();
            break;
        case Home::Next:
            Next();
            break;
        case Home::Seek:
            Seek(home_.Fraction(point, Home::Seek) * player_.GetState().duration);
            break;
        case Home::Volume:
            SetVolume(home_.Fraction(point, Home::Volume));
            break;
        case Home::OpenFiles:
            OpenFiles(false);
            break;
        case Home::OpenFolder:
            OpenFiles(true);
            break;
        case Home::Wave:
            StartWave();
            break;
        case Home::Compact:
            ToggleHome();
            break;
        case Home::Menu:
            ShowMenu(point);
            break;
        case Home::Minimize:
            MinimizeWindow();
            break;
        case Home::Maximize:
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case Home::Close:
            CloseWindow();
            break;
        default:
            break;
    }
    MarkDirty();
    Invalidate();
}
}  // namespace ui
