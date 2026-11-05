#include "ui/app.h"
#include "core/text.h"
#include "ui/workspace.h"
#include <commctrl.h>
#include <fstream>
#include <map>

namespace ui {
LRESULT CALLBACK App::SearchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_KEYDOWN) {
        if (GetKeyState(VK_CONTROL) < 0 && wp == VK_OEM_COMMA) {
            app->OpenWorkspace(view::HomeModel::Settings);
            return 0;
        }
        if (GetKeyState(VK_CONTROL) < 0 && wp == 'M') {
            app->EnterMini();
            return 0;
        }
        if (hwnd == app->downloadEdit_ && wp == VK_RETURN) {
            app->WorkspaceCommand(workspace::SubmitDownload);
            return 0;
        }
        if (hwnd == app->paletteEdit_) {
            // Палитра: стрелки выбирают, Enter выполняет, Esc закрывает
            if (wp == VK_ESCAPE) {
                app->ClosePalette();
                return 0;
            }
            if (wp == VK_RETURN) {
                app->RunPalette(app->paletteSelected_);
                return 0;
            }
            if (wp == VK_DOWN || wp == VK_UP) {
                app->paletteSelected_ += wp == VK_DOWN ? 1 : -1;
                app->Invalidate();
                return 0;
            }
        }
        if (app->RunShortcut((UINT)wp, true)) return 0;
        if (wp == VK_ESCAPE) {
            SetWindowTextW(hwnd, L"");
            SetFocus(app->hwnd_);
            return 0;
        }
        if (wp == 'H' && GetKeyState(VK_CONTROL) < 0) {
            app->AnimatedToggleHome();
            return 0;
        }
    }
    // TranslateMessage still posts characters for keys handled above: Ctrl+H is Backspace (would erase
    // the query), Enter/Ctrl+M are CR and Esc - the single-line edit beeps on them.
    if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n' || wp == 0x1b || (wp == 0x08 && GetKeyState(VK_CONTROL) < 0)))
        return 0;
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
        if (library_.HideDuplicates())
            for (const auto& alias : t.copies) seen.insert(alias);
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
        reduceMotion_ = document.Get("reduceMotion").AsBool(false);
        introEnabled_ = document.Get("intro").AsBool(true);
        backgroundHintShown_ = document.Get("backgroundHint").AsBool(false);
        highContrast_ = document.Get("highContrast").AsBool(false);
        followSystemMotion_ = document.Get("followSystemMotion").AsBool(true);
        coverFrame_ = std::clamp((int)document.Get("coverFrame").AsNumber(0), 0, 10);
        storyBackground_ = std::clamp((int)document.Get("recapBackground").AsNumber(0), 0, 1);
        windowFx_ = std::clamp((int)document.Get("windowFx").AsNumber(0), 0, 6);
        ResetShortcuts();
        size_t at = 0;
        for (const auto& k : document.Get("shortcuts").Items()) {
            if (at < shortcuts_.size() && k.IsObject() && k.Get("key").AsNumber(0) > 0)
                shortcuts_[at] = {(UINT)k.Get("key").AsNumber(0), k.Get("ctrl").AsBool(false)};
            ++at;
        }
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
    downloadEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 1, 1, hwnd_,
                                    (HMENU)3002, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(downloadEdit_, EM_SETLIMITTEXT, 8192, 0);
    SetWindowLongPtrW(downloadEdit_, GWLP_USERDATA, (LONG_PTR)this);
    SetWindowLongPtrW(downloadEdit_, GWLP_WNDPROC, (LONG_PTR)&App::SearchProc);
    // Поиск настроек и палитра команд - такие же поля под нарисованными
    for (HWND* edit : {&settingsEdit_, &paletteEdit_}) {
        *edit =
            CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 1, 1, hwnd_,
                            (HMENU)(INT_PTR)(edit == &settingsEdit_ ? 3003 : 3004), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(*edit, EM_SETLIMITTEXT, 200, 0);
        SetWindowLongPtrW(*edit, GWLP_USERDATA, (LONG_PTR)this);
        SetWindowLongPtrW(*edit, GWLP_WNDPROC, (LONG_PTR)&App::SearchProc);
    }
    LayoutHomeSearch();
}
void App::SaveHomePreferences() {
    json::Value prefs = json::Value::MakeObject();
    prefs["dashboard"] = homeMode_;
    prefs["reduceMotion"] = reduceMotion_;
    prefs["intro"] = introEnabled_;
    prefs["backgroundHint"] = backgroundHintShown_;
    prefs["highContrast"] = highContrast_;
    prefs["followSystemMotion"] = followSystemMotion_;
    prefs["coverFrame"] = coverFrame_;
    prefs["recapBackground"] = storyBackground_;
    prefs["windowFx"] = windowFx_;
    prefs["shortcuts"] = json::Value::MakeArray();
    for (const auto& k : shortcuts_) {
        json::Value key = json::Value::MakeObject();
        key["key"] = (double)k.key;
        key["ctrl"] = k.ctrl;
        prefs["shortcuts"].Push(key);
    }
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
    const bool downloads = homeMode_ && home_.model.page == view::HomeModel::Downloads;
    const bool search = homeMode_ && home_.model.page < view::HomeModel::Plugins;
    ShowWindow(homeSearch_, search ? SW_SHOW : SW_HIDE);
    if (downloadEdit_) ShowWindow(downloadEdit_, downloads ? SW_SHOW : SW_HIDE);
    const bool settings = homeMode_ && home_.model.page == view::HomeModel::Settings && !paletteOpen_;
    if (settingsEdit_) ShowWindow(settingsEdit_, settings ? SW_SHOW : SW_HIDE);
    if (paletteEdit_) ShowWindow(paletteEdit_, homeMode_ && paletteOpen_ ? SW_SHOW : SW_HIDE);
    if (!homeMode_) return;
    const auto size = ClientDip();
    const auto layout = view::HomeLayout(size, search);
    const auto field = layout.searchText;
    SetWindowPos(homeSearch_, nullptr, ToPx(field.x), ToPx(field.y), ToPx(field.w), ToPx(field.h),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (downloadEdit_)
        // 58 - верх содержимого рабочей страницы, 10 - отступ текста внутри поля
        SetWindowPos(downloadEdit_, nullptr, ToPx(layout.mainX + 15), ToPx(58 + Home::kDownloadFieldTop + 10),
                     ToPx(std::max(100.f, layout.mainW - 194)), ToPx(24), SWP_NOZORDER | SWP_NOACTIVATE);
    HFONT font = CreateFontW(-ToPx(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(homeSearch_, WM_SETFONT, (WPARAM)font, TRUE);
    if (downloadEdit_) SendMessageW(downloadEdit_, WM_SETFONT, (WPARAM)font, TRUE);
    {
        // Поле поиска настроек: справа от заголовка «Студии плеера» (как в Home::PaintSettings)
        const auto work = view::HomeLayout(size, false);
        const float right = work.mainX + work.mainW;
        if (settingsEdit_) {
            SetWindowPos(settingsEdit_, nullptr, ToPx(right - 280 + 38), ToPx(58 + 4 + 9), ToPx(230), ToPx(24),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(settingsEdit_, WM_SETFONT, (WPARAM)font, TRUE);
        }
        if (paletteEdit_) {
            const auto r = Home::PaletteField(size);
            SetWindowPos(paletteEdit_, nullptr, ToPx(r.x), ToPx(r.y), ToPx(r.w), ToPx(r.h),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(paletteEdit_, WM_SETFONT, (WPARAM)font, TRUE);
        }
    }
    if (homeSearchFont_) DeleteObject(homeSearchFont_);
    homeSearchFont_ = font;
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
void App::AnimatedToggleHome() {
    if (!MotionAllowed() || modeSwitchPending_ || IsZoomed(hwnd_)) {
        ToggleHome();
        return;
    }
    // Главная сворачивается кругом к центру, плеер складывается крышкой - и только потом новый вид
    renderer_.PlayExit(homeMode_ ? Entrance::Iris : Entrance::Unfold, .2);
    modeSwitchPending_ = true;
    SetTimer(hwnd_, 23, 200, nullptr);  // kModeSwitch
}
void App::ToggleHome() {
    if (homeMode_ && !IsZoomed(hwnd_)) homeSize_ = ClientDip();
    // Restore while the old mode and frame style still agree with Windows.
    if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    homeMode_ = !homeMode_;
    ResetTickTimer();
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
    if (hit.action == Home::None && hit.index >= 10000 && hit.index < 10007) {
        home_.Navigate(hit.index - 10000);
        workspaceNotice_.clear();
        LayoutHomeSearch();
        SetFocus(hwnd_);
        PrefillDownload();
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
                if (home_.model.favorites.count(view::HomePathKey(home_.Displayed()[hit.index].path)))
                    home_.Burst(point);
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
            AnimatedToggleHome();
            break;
        case Home::WorkspaceCommand:
            WorkspaceCommand(hit.index);
            break;
        case Home::Menu:
            OpenWorkspace(view::HomeModel::Settings);
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
        case Home::NowPlaying:
            home_.ToggleNowPlaying();
            break;
        case Home::StoryNav:
            StoryCommand(hit.index);
            break;
        case Home::Karaoke:
            player_.SetKaraoke(!player_.Karaoke());
            break;
        case Home::PaletteItem:
            if (hit.index == -1)
                ClosePalette();
            else if (hit.index >= 0)
                RunPalette(hit.index);
            break;
        case Home::SeekLyric:
            if (hit.index >= 0 && (size_t)hit.index < view_.lyrics.times.size())
                Seek(view_.lyrics.times[(size_t)hit.index]);
            break;
        default:
            break;
    }
    MarkDirty();
    Invalidate();
}
}  // namespace ui
namespace ui {
int App::EnsureInPlaylist(const view::HomeTrack& t) {
    const auto& pl = player_.GetPlaylist();
    for (int i = 0; i < pl.Size(); ++i)
        if (pl.At(i).path == t.path) return i;
    player_.LoadTracks(
        {{t.path, {text::WideToUtf8(t.title), text::WideToUtf8(t.artist), text::WideToUtf8(t.album), t.duration}}},
        true);
    RebuildRows();
    RefreshHome();
    if (!player_.GetState().hasTrack) player_.OpenAt(0, 0, false);
    return player_.GetPlaylist().Size() - 1;
}

void App::HomeDrop(const Home::Drop& drop) {
    int from = -1;
    if (drop.kind == Home::Drop::ToQueue) {
        if (drop.index < 0 || drop.index >= (int)home_.Displayed().size()) return;
        from = EnsureInPlaylist(home_.Displayed()[(size_t)drop.index]);
    } else {
        from = drop.index;
    }
    const int size = player_.GetPlaylist().Size();
    if (from < 0 || from >= size) return;
    // before - место в списке до переноса; если трек ехал сверху, после изъятия всё сдвигается на один
    int to = drop.before < 0 ? size - 1 : (from < drop.before ? drop.before - 1 : drop.before);
    to = std::clamp(to, 0, size - 1);
    if (to != from) player_.Move(from, to);
    RebuildRows();
    RefreshHome();
    MarkDirty();
    Invalidate();
}

void App::TrackMenu(int index, view::Point at) {
    view::HomeTrack track;
    int playlistIndex = -1;
    if (index >= 0 && index < (int)home_.Displayed().size())
        track = home_.Displayed()[(size_t)index];
    else if (index <= -2 && -index - 2 < player_.GetPlaylist().Size()) {
        playlistIndex = -index - 2;
        const auto& e = player_.GetPlaylist().At(playlistIndex);
        track.path = e.path;
        track.title = text::Utf8ToWide(e.meta.title);
        track.artist = text::Utf8ToWide(e.meta.artist);
        track.album = text::Utf8ToWide(e.meta.album);
        track.duration = e.meta.duration;
    } else
        return;
    const bool favorite = home_.model.favorites.count(view::HomePathKey(track.path)) > 0;
    enum { kPlay = 1, kNext, kQueue, kFavorite, kFolder, kRemove };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kPlay, L"Играть");
    AppendMenuW(menu, MF_STRING, kNext, L"Играть следующим");
    if (playlistIndex < 0) AppendMenuW(menu, MF_STRING, kQueue, L"Добавить в очередь");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kFavorite, favorite ? L"Убрать из любимого" : L"В любимое");
    AppendMenuW(menu, MF_STRING, kFolder, L"Показать в папке");
    if (playlistIndex >= 0) AppendMenuW(menu, MF_STRING, kRemove, L"Убрать из очереди");
    POINT pt{ToPx(at.x), ToPx(at.y)};
    ClientToScreen(hwnd_, &pt);
    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (choice) {
        case kPlay:
            PlayIndex(playlistIndex >= 0 ? playlistIndex : EnsureInPlaylist(track));
            break;
        case kNext:
            player_.PlayNext(playlistIndex >= 0 ? playlistIndex : EnsureInPlaylist(track));
            break;
        case kQueue:
            EnsureInPlaylist(track);
            break;
        case kFavorite:
            home_.model.ToggleFavorite(track.path);
            if (!favorite) home_.Burst(at);
            home_.InvalidateCatalog();
            SaveHomePreferences();
            break;
        case kFolder: {
            const std::wstring arg = L"/select,\"" + track.path.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case kRemove:
            player_.Remove(playlistIndex);
            break;
        default:
            return;
    }
    RebuildRows();
    RefreshHome();
    MarkDirty();
    Invalidate();
}
}  // namespace ui
