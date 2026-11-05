// Клавиши, которые можно переназначить, и палитра команд (Ctrl+K): ввести действие или название трека,
// Enter - выполнить. Назначения хранятся в home.json.
#include "ui/app.h"
#include "ui/workspace.h"
#include "core/text.h"
#include "view/home_model.h"
#include <algorithm>
#include <set>

namespace ui {
using namespace workspace;
namespace {
struct ShortcutInfo {
    const wchar_t* name;
    UINT key;
    bool ctrl;
};
// Порядок совпадает с App::ShortcutAction
const ShortcutInfo kShortcuts[] = {
    {L"Пауза и воспроизведение", VK_SPACE, false},
    {L"Следующий трек", VK_RIGHT, true},
    {L"Предыдущий трек", VK_LEFT, true},
    {L"Мини-полоска", 'M', true},
    {L"Главная или плеер", 'H', true},
    {L"Поиск", 'F', true},
    {L"Настройки", VK_OEM_COMMA, true},
    {L"Сейчас играет", VK_F11, false},
    {L"Палитра команд", 'K', true},
};
constexpr int kShortcutCount = sizeof(kShortcuts) / sizeof(kShortcuts[0]);
}  // namespace

std::wstring App::KeyName(UINT key, bool ctrl) const {
    std::wstring name;
    switch (key) {
        case VK_SPACE:
            name = L"Пробел";
            break;
        case VK_LEFT:
            name = L"←";
            break;
        case VK_RIGHT:
            name = L"→";
            break;
        case VK_UP:
            name = L"↑";
            break;
        case VK_DOWN:
            name = L"↓";
            break;
        case VK_OEM_COMMA:
            name = L",";
            break;
        case VK_OEM_PERIOD:
            name = L".";
            break;
        default:
            if (key >= VK_F1 && key <= VK_F24)
                name = L"F" + std::to_wstring(key - VK_F1 + 1);
            else if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
                name = std::wstring(1, (wchar_t)key);
            else {
                wchar_t buf[32]{};
                if (GetKeyNameTextW((LONG)(MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16), buf, 32) > 0)
                    name = buf;
                else
                    name = L"#" + std::to_wstring(key);
            }
    }
    return ctrl ? L"Ctrl+" + name : name;
}

void App::ResetShortcuts() {
    shortcuts_.clear();
    for (const auto& s : kShortcuts) shortcuts_.push_back({s.key, s.ctrl});
}

void App::BuildShortcutCards() {
    if ((int)shortcuts_.size() != kShortcutCount) ResetShortcuts();
    std::vector<Home::Control> controls;
    for (int i = 0; i < kShortcutCount; ++i) {
        const bool capturing = capturingShortcut_ == i;
        controls.push_back(
            {std::wstring(kShortcuts[i].name) + L"  ·  " +
                 (capturing ? L"нажмите клавиши…" : KeyName(shortcuts_[(size_t)i].key, shortcuts_[(size_t)i].ctrl)),
             ShortcutFirst + i, capturing});
    }
    homeInput_.cards.push_back(
        {L"Клавиши",
         L"Нажмите действие, затем новое сочетание. Esc - отмена. Стрелки ↑↓ - громкость, ←→ - перемотка.",
         {},
         std::move(controls),
         -1,
         false});
}

bool App::CaptureShortcut(UINT key) {
    if (capturingShortcut_ < 0) return false;
    if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LWIN || key == VK_RWIN) return true;
    const int index = capturingShortcut_;
    capturingShortcut_ = -1;
    if (key == VK_ESCAPE) {
        Invalidate();
        return true;
    }
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    // Сочетание было у другого действия - они меняются местами, ничего не пропадает
    for (auto& s : shortcuts_)
        if (s.key == key && s.ctrl == ctrl) s = shortcuts_[(size_t)index];
    shortcuts_[(size_t)index] = {key, ctrl};
    SaveHomePreferences();
    workspaceNotice_ = std::wstring(L"«") + kShortcuts[index].name + L"» - теперь " + KeyName(key, ctrl) + L".";
    Invalidate();
    return true;
}

// fromEdit: нажато в поле ввода - одиночные клавиши (пробел, буквы) остаются полю
bool App::RunShortcut(UINT key, bool fromEdit) {
    if ((int)shortcuts_.size() != kShortcutCount) ResetShortcuts();
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    for (int i = 0; i < kShortcutCount; ++i) {
        const auto& s = shortcuts_[(size_t)i];
        if (s.key != key || s.ctrl != ctrl) continue;
        if (fromEdit && !s.ctrl && !(key >= VK_F1 && key <= VK_F24)) return false;
        RunAction(i);
        return true;
    }
    return false;
}

void App::RunAction(int action) {
    switch (action) {
        case 0:
            TogglePlay();
            break;
        case 1:
            Next();
            break;
        case 2:
            Prev();
            break;
        case 3:
            if (!homeMode_ && view_.mini) {
                view_.ToggleMini(Now());
                ApplyTopmost();
                MarkDirty();
            } else
                EnterMini();
            break;
        case 4:
            AnimatedToggleHome();
            break;
        case 5:
            if (!homeMode_) ToggleHome();
            if (home_.model.page >= view::HomeModel::Plugins) {
                home_.Navigate(view::HomeModel::Library);
                LayoutHomeSearch();
            }
            SetFocus(homeSearch_);
            break;
        case 6:
            OpenWorkspace(view::HomeModel::Settings);
            break;
        case 7:
            if (!homeMode_) ToggleHome();
            home_.ToggleNowPlaying();
            break;
        case 8:
            OpenPalette();
            break;
        default:
            break;
    }
    Invalidate();
}

// ---------------- палитра команд ----------------

namespace {
struct PaletteCommand {
    const wchar_t* title;
    const wchar_t* hint;
};
const PaletteCommand kCommands[] = {
    {L"Пауза / играть", L"Воспроизведение"},
    {L"Следующий трек", L"Воспроизведение"},
    {L"Предыдущий трек", L"Воспроизведение"},
    {L"Перемешать", L"Воспроизведение"},
    {L"Повтор", L"Воспроизведение"},
    {L"Слушать волну", L"Подборка из библиотеки"},
    {L"Сейчас играет", L"На весь экран"},
    {L"Главная", L"Раздел"},
    {L"Библиотека", L"Раздел"},
    {L"Любимое", L"Раздел"},
    {L"Очередь", L"Раздел"},
    {L"Скачивание", L"Раздел"},
    {L"Настройки", L"Раздел"},
    {L"Плагины", L"Раздел"},
    {L"Мини-полоска", L"Вид окна"},
    {L"Компактный плеер", L"Вид окна"},
    {L"Добавить музыку", L"Файлы"},
    {L"Открыть папку с музыкой", L"Файлы"},
    {L"Итоги прослушивания", L"История"},
    {L"Меньше движения", L"Настройка"},
    {L"Караоке", L"Голос тише, текст крупно"},
    // 21..27: «Волна» под настроение - WaveMoodFirst + (id - 21)
    {L"Волна · утро", L"Что вы слушаете по утрам"},
    {L"Волна · день", L"Что вы слушаете днём"},
    {L"Волна · вечер", L"Что вы слушаете вечером"},
    {L"Волна · ночь", L"Что вы слушаете ночью"},
    {L"Волна · забытое", L"Давно не звучало"},
    {L"Волна · любимое", L"Сердечки и частое"},
    {L"Волна · новое", L"Ещё ни разу не играло"},
};
}  // namespace

void App::OpenPalette() {
    if (!homeMode_) ToggleHome();
    paletteOpen_ = true;
    paletteSelected_ = 0;
    SetWindowTextW(paletteEdit_, L"");
    LayoutHomeSearch();
    SetFocus(paletteEdit_);
    Invalidate();
}

void App::ClosePalette() {
    paletteOpen_ = false;
    LayoutHomeSearch();
    SetFocus(hwnd_);
    Invalidate();
}

void App::BuildPalette() {
    homeInput_.palette.clear();
    homeInput_.paletteOpen = paletteOpen_;
    if (!paletteOpen_) return;
    wchar_t buf[256]{};
    GetWindowTextW(paletteEdit_, buf, 256);
    homeInput_.paletteQuery = buf;
    homeInput_.paletteFocused = GetFocus() == paletteEdit_;
    SendMessageW(paletteEdit_, EM_GETSEL, (WPARAM)&homeInput_.paletteStart, (LPARAM)&homeInput_.paletteEnd);
    const auto q = view::HomeLower(buf);
    for (int i = 0; i < (int)(sizeof(kCommands) / sizeof(kCommands[0])); ++i)
        if (q.empty() || view::HomeLower(kCommands[i].title).find(q) != std::wstring::npos)
            homeInput_.palette.push_back({kCommands[i].title, kCommands[i].hint, i});
    // Исполнитель, чьё имя начинается с запроса, - своя «Волна»
    paletteArtists_.clear();
    if (q.size() >= 2) {
        std::set<std::wstring> seen;
        for (const auto& t : home_.model.tracks) {
            const auto name = view::HomeLower(t.artist);
            if (t.artist.empty() || name.find(q) != 0 || !seen.insert(name).second || paletteArtists_.size() >= 3) continue;
            paletteArtists_.push_back(::text::WideToUtf8(t.artist));
            homeInput_.palette.push_back({L"Волна · " + t.artist, L"Только этот исполнитель", 500 + (int)paletteArtists_.size() - 1});
        }
    }
    // Треки - по тем же правилам, что и поиск библиотеки
    if (!q.empty()) {
        view::HomeModel probe;
        probe.query = buf;
        int found = 0;
        for (size_t i = 0; i < home_.model.tracks.size() && found < 8; ++i) {
            const auto& t = home_.model.tracks[i];
            if (!probe.Match(t)) continue;
            homeInput_.palette.push_back({t.title, t.artist.empty() ? L"Трек" : L"Трек · " + t.artist, 1000 + (int)i});
            ++found;
        }
    }
    paletteSelected_ = std::clamp(paletteSelected_, 0, std::max(0, (int)homeInput_.palette.size() - 1));
    homeInput_.paletteSelected = paletteSelected_;
}

void App::RunPalette(int item) {
    const int id = item >= 0 && item < (int)homeInput_.palette.size() ? homeInput_.palette[(size_t)item].id : -1;
    ClosePalette();
    if (id < 0) return;
    if (id >= 500 && id < 1000) {
        if ((size_t)(id - 500) < paletteArtists_.size()) StartMoodWave(7, paletteArtists_[(size_t)(id - 500)]);
        return;
    }
    if (id >= 1000) {
        const size_t index = (size_t)(id - 1000);
        if (index < home_.model.tracks.size()) PlayIndex(EnsureInPlaylist(home_.model.tracks[index]));
        return;
    }
    switch (id) {
        case 0:
            TogglePlay();
            break;
        case 1:
            Next();
            break;
        case 2:
            Prev();
            break;
        case 3:
            ToggleShuffle();
            break;
        case 4:
            CycleRepeat();
            break;
        case 5:
            StartWave();
            break;
        case 6:
            if (!home_.NowPlayingOpen()) home_.ToggleNowPlaying();
            break;
        case 7:
            OpenWorkspace(view::HomeModel::Home);
            break;
        case 8:
            OpenWorkspace(view::HomeModel::Library);
            break;
        case 9:
            OpenWorkspace(view::HomeModel::Favorites);
            break;
        case 10:
            OpenWorkspace(view::HomeModel::Queue);
            break;
        case 11:
            OpenWorkspace(view::HomeModel::Downloads);
            break;
        case 12:
            OpenWorkspace(view::HomeModel::Settings);
            break;
        case 13:
            OpenWorkspace(view::HomeModel::Plugins);
            break;
        case 14:
            EnterMini();
            break;
        case 15:
            if (homeMode_) ToggleHome();
            break;
        case 16:
            OpenFiles(false);
            break;
        case 17:
            OpenFiles(true);
            break;
        case 18:
            if (homeMode_) OpenStory(statsPeriod_);
            else ShowStats();
            break;
        case 19:
            reduceMotion_ = !reduceMotion_;
            ApplyTheme(theme_, false);
            SaveHomePreferences();
            break;
        case 20:
            player_.SetKaraoke(!player_.Karaoke());
            if (!home_.NowPlayingOpen() && player_.Karaoke()) home_.ToggleNowPlaying();  // текст - в «Сейчас играет»
            break;
        case 21: case 22: case 23: case 24: case 25: case 26: case 27:
            StartMoodWave(id - 21);
            break;
        default:
            break;
    }
    MarkDirty();
    Invalidate();
}
}  // namespace ui
