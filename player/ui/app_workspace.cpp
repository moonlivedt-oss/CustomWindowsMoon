#include "ui/app.h"
#include "ui/workspace.h"
#include "core/text.h"
#include <algorithm>
#include <cmath>

namespace ui {
using namespace workspace;
namespace {
std::wstring Wide(const std::string& s) { return text::Utf8ToWide(s); }
}
void App::OpenWorkspace(view::HomeModel::Page page) {
    if (!homeMode_) ToggleHome();
    home_.Navigate(page);
    workspaceNotice_.clear();
    LayoutHomeSearch();
    SetFocus(hwnd_);
    PrefillDownload();
    Invalidate();
}
void App::EnterMini() {
    if (homeMode_) ToggleHome();
    if (!view_.mini) view_.ToggleMini(Now());
    ApplyTopmost();
    MarkDirty();
    Invalidate();
}
void App::BuildWorkspace() {
    homeInput_.cards.clear();
    workspaceBindings_.clear();
    homeInput_.notice = workspaceNotice_;
    homeInput_.reducedMotion = systemReduce_ || reduceMotion_;
    homeInput_.downloadEnabled = false;
    BuildPalette();
    if (home_.model.page < view::HomeModel::Plugins) return;
    auto add = [&](std::wstring title, std::wstring subtitle, std::vector<Home::Control> controls = {}) {
        homeInput_.cards.push_back({std::move(title), std::move(subtitle), {}, std::move(controls), -1, false});
    };
    if (home_.model.page == view::HomeModel::Settings) {
        BuildSettings();
        return;
    }
    const auto& plugins = plugins_.Plugins();
    if (home_.model.page == view::HomeModel::Plugins) {
        add(L"Управление расширениями", L"Плагины загружаются из папки рядом с плеером и из пользовательской папки.",
            {{L"Открыть папку плагинов", PluginsFolder}});
        for (size_t i = 0; i < plugins.size() && i < size_t(PluginMenuFirst - PluginFirst); ++i) {
            const auto& p = plugins[i];
            add(Wide(p.name) + L"  ·  " + Wide(p.version),
                Wide(p.error.empty() ? p.description : p.error),
                {{p.enabled ? L"Выключить" : L"Включить", PluginFirst + (int)i, p.enabled}});
            homeInput_.cards.back().error = !p.error.empty();
        }
        if (plugins.empty()) {
            add(L"Плагины не найдены", L"Добавьте папку плагина с plugin.json и DLL в пользовательскую папку и перезапустите плеер.");
            homeInput_.cards.back().illustration = Home::EmptyPlugins;
        }
        // Plugin-defined menu entries remain available without the old nested menu.
        const auto& items = plugins_.MenuItems();
        for (size_t i = 0; i < items.size() && i < size_t(PageActionFirst - PluginMenuFirst); ++i)
            add(Wide(items[i].title), L"Действие расширения", {{L"Открыть", PluginMenuFirst + (int)i}});
        return;
    }
    for (const auto& p : plugins) if (p.id == "downloader") homeInput_.downloadEnabled = p.loaded && p.enabled;
    wchar_t value[8193]{};
    GetWindowTextW(downloadEdit_, value, 8193);
    homeInput_.downloadText = value;
    homeInput_.downloadFocused = GetFocus() == downloadEdit_;
    SendMessageW(downloadEdit_, EM_GETSEL, (WPARAM)&homeInput_.downloadStart, (LPARAM)&homeInput_.downloadEnd);
    if (!homeInput_.downloadEnabled) {
        add(L"Три шага до музыки", L"1. Скопируйте ссылку в браузере.  2. Вернитесь сюда - она сама окажется в поле.  "
                                   L"3. Нажмите «Скачать». Нужные программы плеер установит при первой загрузке.",
            {{L"Подготовить заранее", EnableDownloader, true}});
        return;
    }
    const auto& pages = plugins_.Pages();
    for (size_t page = 0; page < pages.size(); ++page) {
        const auto& src = pages[page];
        if (src.plugin >= plugins.size() || plugins[src.plugin].id != "downloader") continue;
        auto control = [&](const pm::PluginAction& a, int row) {
            const int id = PageActionFirst + (int)workspaceBindings_.size();
            workspaceBindings_.push_back({page, row, a.id});
            return Home::Control{Wide(a.label), id, false};
        };
        std::vector<Home::Control> controls;
        std::string browserLabel;
        for (const auto& a : src.actions) {
            if (a.id == 3) browserLabel = a.label;
            else if (a.id != 1) controls.push_back(control(a, 0));
        }
        const auto& menu = plugins_.MenuItems();
        for (size_t i = 0; i < menu.size() && i < size_t(PageActionFirst - PluginMenuFirst); ++i)
            if (menu[i].plugin == src.plugin && menu[i].id != 1)
                controls.push_back({Wide(menu[i].title), PluginMenuFirst + (int)i, false});
        add(L"Куда сохранять", L"Готовые треки сразу появляются в библиотеке и очереди. Скачанное повторно не загружается, "
                               L"оборванное продолжается, файлы не перезаписываются.", controls);
        add(L"Личные плейлисты - по желанию", L"Нужно только для «Мне нравится» и закрытых списков: выберите браузер, где вы уже вошли в YouTube Music. Пароль вводить не нужно.",
            {{L"Без входа", BrowserNone, browserLabel == "Вход: нет"},
             {L"Firefox", BrowserFirefox, browserLabel == "Вход: Firefox"},
             {L"Chrome", BrowserChrome, browserLabel == "Вход: Chrome"},
             {L"Edge", BrowserEdge, browserLabel == "Вход: Edge"}});
        for (const auto& row : src.rows) {
            controls.clear();
            for (const auto& a : row.actions) controls.push_back(control(a, row.id));
            add(Wide(row.title), Wide(row.subtitle + (row.status.empty() ? "" : " · " + row.status)), controls);
            homeInput_.cards.back().progress = row.progress;
            homeInput_.cards.back().error = row.state == PM_ROW_ERROR;
            homeInput_.cards.back().job = row.state;
            homeInput_.cards.back().tools = row.id == 1;
            if (row.id != 1) {
                auto job = std::move(homeInput_.cards.back());
                homeInput_.cards.pop_back();
                homeInput_.cards.insert(homeInput_.cards.begin(), std::move(job));
            }
        }
        if (src.rows.empty()) {
            add(L"Загрузок пока нет", Wide(src.emptyText));
            homeInput_.cards.back().illustration = Home::EmptyDownloads;
        }
        // Пока загрузчик не готов, его строка - первое, что видно на странице
        for (const auto& row : src.rows) {
            if (row.id != 1 || row.state == PM_ROW_DONE) continue;
            auto it = std::find_if(homeInput_.cards.begin(), homeInput_.cards.end(),
                                   [&](const Home::Card& c) { return c.title == Wide(row.title); });
            if (it != homeInput_.cards.end()) std::rotate(homeInput_.cards.begin(), it, it + 1);
        }
    }
}
void App::WorkspaceCommand(int command) {
    workspaceNotice_.clear();
    if ((command >= ShapeFirst && command <= VizMore) || (command >= ThemeFirst && command < RemoveFolderFirst) || command == FontSmaller ||
        command == FontLarger || command == Normalize || command == Remaining || command == MiniTop || command == ReduceMotion ||
        command == AutoScan || command == HideDuplicates || (command >= CrossfadeFirst && command < CrossfadeFirst + 4) ||
        (command >= WindowFxFirst && command < WindowFxFirst + 7))
        RememberSettings();
    if (command >= ShortcutFirst && command < PresetFirst) {
        capturingShortcut_ = capturingShortcut_ == command - ShortcutFirst ? -1 : command - ShortcutFirst;
        SetFocus(hwnd_);
        Invalidate();
        return;
    }
    if (command >= WaveMoodFirst && command < WaveMoodFirst + 8) {
        StartMoodWave(command - WaveMoodFirst);
        return;
    }
    if (command == OpenDownloads) {
        OpenWorkspace(view::HomeModel::Downloads);
        return;
    }
    if (SettingsCommand(command)) {
        MarkDirty();
        Invalidate();
        return;
    }
    if (command >= WindowFxFirst && command < WindowFxFirst + 7) {
        windowFx_ = command - WindowFxFirst;
        SaveHomePreferences();
        // Показ на живом окне: вариант «своё» показывает переход в главную
        PlayWindowEntrance(windowFx_ == 0 ? WindowForm::Compact : WindowForm::Unknown, WindowForm::Home);
        MarkDirty();
        Invalidate();
        return;
    }
    if (command >= CrossfadeFirst && command < CrossfadeFirst + 4) {
        const double seconds[] = {0, 3, 6, 10};
        player_.SetCrossfade(seconds[command - CrossfadeFirst]);
        Invalidate();
        return;
    }
    if (command >= ShapeFirst && command <= VizMore) {
        auto next = theme_;
        auto& settings = next.settings;
        if (command < MotionFirst) {
            const char* shapes[] = {"square", "rounded", "circle"}; settings.coverShape = shapes[command - ShapeFirst];
        } else if (command < AccentFirst) {
            const auto& character = theme::MotionCharacters()[command - MotionFirst];
            settings.motion = character.id; settings.stiffness = character.stiffness;
            settings.damping = character.damping; settings.speed = character.speed == 0 ? 1 : character.speed;
        } else if (command < AccentFromCover) {
            const char* colors[] = {"#ED647B", "#A78BFA", "#57C9C1", "#83B87D", "#F2B56B", "#79AEF2"};
            theme::ParseHexColor(colors[command - AccentFirst], settings.accent);
            settings.accent2 = settings.accent; settings.accentFromCover = false;
        } else if (command == AccentFromCover) settings.accentFromCover = !settings.accentFromCover;
        else if (command < Visualizer) settings.ambient = (command - AmbientFirst) * .5;
        else if (command == Visualizer) settings.vizStyle = settings.vizStyle == "off" ? "mirror" : "off";
        else settings.vizBars = std::clamp(settings.vizBars + (command == VizMore ? 8 : -8), 8., 96.);
        userTheme_ = true; ApplyTheme(next, false); MarkDirty(); Invalidate(); return;
    }
    switch (command) {
        case SettingsAppearance: case SettingsSound: case SettingsLibrary: case SettingsGeneral:
            settingsSection_ = command - SettingsAppearance; home_.ResetScroll(); break;
        case PreviewMini: previewMini_ = true; break;
        case PreviewFull: previewMini_ = false; break;
        case PreviewLarge: previewLarge_ = !previewLarge_; break;
        case HideDuplicates:
            library_.SetHideDuplicates(!library_.HideDuplicates()); library_.Save(libraryPath_); StartLibraryScan(); break;
        case LikedMusic:
            SetWindowTextW(downloadEdit_, L"https://music.youtube.com/playlist?list=LM");
            workspaceNotice_ = L"Для личного списка выберите ниже браузер, где выполнен вход в YouTube Music.";
            SetFocus(downloadEdit_); break;
        case BrowserNone: case BrowserFirefox: case BrowserChrome: case BrowserEdge:
            for (size_t i = 0; i < plugins_.Pages().size(); ++i) {
                const auto plugin = plugins_.Pages()[i].plugin;
                if (plugin < plugins_.Plugins().size() && plugins_.Plugins()[plugin].id == "downloader") {
                    plugins_.PageAction(i, 0, 21 + command - BrowserNone); break;
                }
            }
            break;
        case AddFolder: AddLibraryFolder(); break;
        case Rescan: StartLibraryScan(); break;
        case AutoScan:
            library_.SetAutoScan(!library_.AutoScan()); RestartWatcher();
            if (library_.AutoScan()) StartLibraryScan();
            library_.Save(libraryPath_); break;
        case Normalize: player_.SetNormalize(!player_.Normalize()); break;
        case Remaining: view_.showRemaining = !view_.showRemaining; break;
        case MiniTop: miniOnTop_ = !miniOnTop_; ApplyTopmost(); break;
        case ReduceMotion: reduceMotion_ = !reduceMotion_; ApplyTheme(theme_, false); break;
        case Compact: if (homeMode_) AnimatedToggleHome(); break;
        case Mini: EnterMini(); break;
        case workspace::OpenFiles: App::OpenFiles(false); break;
        case OpenFolder: App::OpenFiles(true); break;
        case workspace::ImportTheme: App::ImportTheme(); break;
        case workspace::ExportTheme: App::ExportTheme(); break;
        case PluginsFolder: OpenPluginsFolder(); break;
        case EnableDownloader: EnableDownloaderPlugin(); break;
        case SubmitDownload: {
            // Пустое поле - берём ссылку из буфера обмена (её прочитает сам плагин)
            wchar_t value[8193]{}; GetWindowTextW(downloadEdit_, value, 8193);
            submittedDownload_ = text::WideToUtf8(value);
            if (!EnableDownloaderPlugin()) { workspaceNotice_ = L"Загрузчик не запустился - загляните в раздел «Плагины»."; break; }
            bool sent = false;
            for (size_t i = 0; i < plugins_.Pages().size(); ++i) {
                const auto& p = plugins_.Pages()[i];
                if (p.plugin < plugins_.Plugins().size() && plugins_.Plugins()[p.plugin].id == "downloader") {
                    plugins_.PageAction(i, 0, 1); sent = true; break;
                }
            }
            submittedDownload_.clear();
            if (!sent) workspaceNotice_ = L"Загрузчик ещё запускается, нажмите «Скачать» ещё раз.";
            else { SetWindowTextW(downloadEdit_, L""); prefillClipboard_ = GetClipboardSequenceNumber(); }
            break;
        }
        case FontSmaller:
        case FontLarger: {
            auto theme = theme_;
            theme.settings.textScale = std::clamp(theme.settings.textScale + (command == FontLarger ? .1 : -.1), .8, 2.0);
            userTheme_ = true; ApplyTheme(theme, false); break;
        }
        case Exit: QuitApp(); break;
        case Stats: if (homeMode_) OpenStory(statsPeriod_); else ShowStats(); break;
        case OpenLibrary: home_.Navigate(view::HomeModel::Library); LayoutHomeSearch(); break;
        default:
            if (command >= PageActionFirst) {
                const auto index = (size_t)(command - PageActionFirst);
                if (index < workspaceBindings_.size()) {
                    const auto binding = workspaceBindings_[index];
                    plugins_.PageAction(binding.page, binding.row, binding.action);
                }
            } else if (command >= PluginMenuFirst) plugins_.InvokeMenuItem(command - PluginMenuFirst);
            else if (command >= PluginFirst) TogglePlugin(command - PluginFirst);
            else if (command >= RemoveFolderFirst) {
                const auto index = (size_t)(command - RemoveFolderFirst);
                if (index < library_.Folders().size()) {
                    const auto folder = library_.Folders()[index];
                    library_.RemoveFolder(folder); library_.Save(libraryPath_); RestartWatcher(); RefreshHome();
                    workspaceNotice_ = L"Папка убрана из библиотеки. Файлы на диске сохранены.";
                }
            } else if (command >= ThemeFirst) {
                const auto index = (size_t)(command - ThemeFirst);
                if (index < themes_.BuiltinIds().size()) { userTheme_ = false; ApplyTheme(themes_.Make(themes_.BuiltinIds()[index]), false); }
            }
            break;
    }
    MarkDirty();
    Invalidate();
}
}  // namespace ui
