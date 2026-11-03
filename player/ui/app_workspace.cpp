#include "ui/app.h"
#include "ui/workspace.h"
#include "core/text.h"
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
    if (home_.model.page < view::HomeModel::Plugins) return;
    auto add = [&](std::wstring title, std::wstring subtitle, std::vector<Home::Control> controls = {}) {
        homeInput_.cards.push_back({std::move(title), std::move(subtitle), {}, std::move(controls), -1, false});
    };
    if (home_.model.page == view::HomeModel::Settings) {
        homeInput_.settingsSection = settingsSection_;
        homeInput_.previewMini = previewMini_;
        homeInput_.previewLarge = previewLarge_;
        homeInput_.reducedMotion = systemReduce_ || reduceMotion_;
        homeInput_.canUndo = settingsUndo_.has_value();
        auto toggle = [&](std::wstring title, std::wstring hint, int command, bool on) {
            add(std::move(title), std::move(hint), {{on ? L"Включено" : L"Выключено", command, on, true}});
        };
        if (settingsSection_ == 0) {
            std::vector<Home::Control> themes;
            const auto& ids = themes_.BuiltinIds();
            for (size_t i = 0; i < ids.size() && i < size_t(RemoveFolderFirst - ThemeFirst); ++i) {
                const auto* t = themes_.Builtin(ids[i]);
                if (!t) continue;
                const auto bg = t->settings.winColor, ac = t->settings.accent;
                themes.push_back({Wide(t->name), ThemeFirst + (int)i, theme_.settings.skin == ids[i], false,
                    D2D1::ColorF(bg.r, bg.g, bg.b), D2D1::ColorF(ac.r, ac.g, ac.b)});
            }
            add(L"Выберите настроение", L"Смотрите на плеер справа: это настоящее оформление, которое вы меняете.", themes);
            homeInput_.cards.back().themeGallery = true;
            add(L"Размер текста — " + std::to_wstring((int)std::lround(theme_.settings.textScale * 100)) + L"%",
                L"Название и исполнитель в образце меняются сразу. Кнопки не перекрываются крупным текстом.",
                {{L"Аа  Меньше", FontSmaller}, {L"Аа  Больше", FontLarger}});
            add(L"Форма обложки", L"Квадратная, мягкая или круглая — результат сразу виден в образце.",
                {{L"Квадрат", ShapeFirst, theme_.settings.coverShape == "square"},
                 {L"Скругление", ShapeFirst + 1, theme_.settings.coverShape == "rounded"},
                 {L"Круг", ShapeFirst + 2, theme_.settings.coverShape == "circle"}});
            const char* colors[] = {"#ED647B", "#A78BFA", "#57C9C1", "#83B87D", "#F2B56B", "#79AEF2"};
            const wchar_t* names[] = {L"Роза", L"Лаванда", L"Мята", L"Шалфей", L"Мёд", L"Небо"};
            std::vector<Home::Control> accents;
            for (int i = 0; i < 6; ++i) {
                theme::Rgba ac; theme::ParseHexColor(colors[i], ac);
                const auto bg = theme_.settings.winColor;
                accents.push_back({names[i], AccentFirst + i,
                    !theme_.settings.accentFromCover && theme::HexColor(theme_.settings.accent, false) == colors[i], false,
                    D2D1::ColorF(bg.r, bg.g, bg.b), D2D1::ColorF(ac.r, ac.g, ac.b)});
            }
            add(L"Акцентный цвет", L"Цвет кнопок, полосы времени и волны. Выбор цвета отключает подбор из обложки.", accents);
            homeInput_.cards.back().themeGallery = true;
            toggle(L"Цвета из обложки", L"Плеер подбирает акцент к текущему альбому. В образце используется тот же трек.", AccentFromCover, theme_.settings.accentFromCover);
            add(L"Подсветка обложки", L"Мягкое цветное свечение под изображением. Посмотрите на его края в образце.",
                {{L"Без подсветки", AmbientFirst, theme_.settings.ambient == 0},
                 {L"Мягкая", AmbientFirst + 1, theme_.settings.ambient > 0 && theme_.settings.ambient < .8},
                 {L"Яркая", AmbientFirst + 2, theme_.settings.ambient >= .8}});
            toggle(L"Музыкальная волна", L"Движение под обложкой. Здесь работает демонстрация, при воспроизведении волна следует музыке.", Visualizer, theme_.settings.vizStyle != "off");
            add(L"Детализация волны — " + std::to_wstring((int)theme_.settings.vizBars) + L" полос",
                L"Меньше полос — спокойнее силуэт. Больше — подробнее рисунок звука.", {{L"Меньше", VizLess}, {L"Больше", VizMore}});
            std::vector<Home::Control> motion;
            const wchar_t* motionNames[] = {L"Мягко", L"Пружинисто", L"Спокойно", L"Быстро", L"Плавно", L"Без анимаций"};
            for (int i = 0; i < 6; ++i) motion.push_back({motionNames[i], MotionFirst + i, theme_.settings.motion == theme::MotionCharacters()[i].id});
            add(L"Характер движения плеера", L"Меняет пружины и скорость переходов. Переключите «Плеер / Мини» справа, чтобы увидеть движение.", motion);
            toggle(L"Меньше движения", L"Включите для спокойного интерфейса. Образец перестанет двигаться; музыка продолжит играть как обычно.", ReduceMotion, reduceMotion_);
            add(L"Своя тема", L"Импортируйте оформление или сохраните свои цвета и размеры в файл.",
                {{L"Импорт", workspace::ImportTheme}, {L"Экспорт", workspace::ExportTheme}});
        } else if (settingsSection_ == 1) {
            toggle(L"Выровнять громкость", L"Громкие и тихие треки ближе по уровню. Работает, если в файле есть метки ReplayGain.", Normalize, player_.Normalize());
            {
                const double cf = player_.Crossfade();
                add(L"Плавный переход между треками",
                    L"Конец трека затихает, следующий в это время нарастает. Треки одного альбома идут стыком, как записаны.",
                    {{L"Стык", CrossfadeFirst, cf <= 0},
                     {L"3 с", CrossfadeFirst + 1, cf > 0 && cf < 4.5},
                     {L"6 с", CrossfadeFirst + 2, cf >= 4.5 && cf < 8},
                     {L"10 с", CrossfadeFirst + 3, cf >= 8}});
            }
            toggle(L"Показывать, сколько осталось", L"Посмотрите на время под обложкой в образце. Выключено — полная длительность; включено — обратный отсчёт.", Remaining, view_.showRemaining);
            toggle(L"Мини-плеер поверх окон", L"Мини-полоска останется видна поверх других программ. Обычный плеер это не затрагивает.", MiniTop, miniOnTop_);
            add(L"Размер окна", L"Выберите реальный режим. Образец справа можно переключать, не закрывая настройки.",
                {{L"Открыть плеер", Compact}, {L"Мини-полоска", Mini}});
        } else if (settingsSection_ == 2) {
            add(L"Ваша музыка", scanner_.Running() ? L"Ищем новые треки…" : L"Папки с музыкой. Файлы остаются на своих местах.",
                {{L"Добавить папку", AddFolder}, {L"Пересканировать", Rescan}, {L"К библиотеке", OpenLibrary}});
            toggle(L"Следить за папками", L"Новые файлы появляются в библиотеке автоматически, без ручного обновления.", AutoScan, library_.AutoScan());
            toggle(L"Скрывать одинаковые файлы", L"Точные копии показываются один раз. Сравниваем содержимое, а не только название. Файлы не удаляются.", HideDuplicates, library_.HideDuplicates());
            const auto& folders = library_.Folders();
            // Each list owns a fixed command range (workspace.h); never let one spill into the next
            for (size_t i = 0; i < folders.size() && i < size_t(PluginFirst - RemoveFolderFirst); ++i)
                add(L"Музыкальная папка", folders[i].wstring(), {{L"Убрать из списка", RemoveFolderFirst + (int)i}});
        } else {
            add(L"Открыть музыку", L"Добавьте файлы или папку. Поддерживается перетаскивание в окно.",
                {{L"Файлы", workspace::OpenFiles}, {L"Папка", OpenFolder}});
            add(L"История", L"Посмотрите, что слушали чаще всего. Статистика хранится локально.", {{L"Итоги", Stats}});
            add(L"PlayerMoon", L"Настройки применяются сразу. Последнее изменение можно отменить кнопкой под образцом.", {{L"Закрыть плеер", Exit}});
            // всё выше - кнопками; клавиши запоминать не нужно, это лишь короткий путь для привыкших
            add(L"Клавиши, если удобно", L"Необязательно: всё то же есть кнопками. Пробел — пауза, Ctrl+F — поиск, Ctrl+M — мини-полоска, Ctrl+H — главная или плеер.");
        }
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
        if (plugins.empty()) add(L"Плагины не найдены", L"Добавьте папку плагина с plugin.json и DLL в пользовательскую папку и перезапустите плеер.");
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
    add(L"Умная загрузка", L"Уже скачанные ID и повторы ссылок пропускаются. Обрывы продолжаются, недоступные треки не останавливают плейлист. Существующие файлы не перезаписываются.");
    if (!homeInput_.downloadEnabled) {
        add(L"Загрузчик выключен", L"Включите штатный плагин «Загрузка музыки», чтобы проверить инструменты и управлять заданиями.",
            {{L"Включить загрузчик", EnableDownloader, true}});
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
        add(L"Куда сохранять", L"Готовые треки появятся в библиотеке. Архив успешных загрузок хранится в папке музыки.", controls);
        add(L"Вход для личного плейлиста", L"Список LM — «Мне нравится». Выберите браузер, где уже выполнен вход в YouTube Music. Пароль в PlayerMoon вводить не нужно.",
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
            if (row.id != 1) {
                auto job = std::move(homeInput_.cards.back());
                homeInput_.cards.pop_back();
                homeInput_.cards.insert(homeInput_.cards.begin(), std::move(job));
            }
        }
        if (src.rows.empty()) add(L"Загрузок пока нет", Wide(src.emptyText));
    }
}
void App::WorkspaceCommand(int command) {
    workspaceNotice_.clear();
    if ((command >= ShapeFirst && command <= VizMore) || (command >= ThemeFirst && command < RemoveFolderFirst) || command == FontSmaller || command == FontLarger ||
        command == Normalize || command == Remaining || command == MiniTop || command == ReduceMotion || command == AutoScan || command == HideDuplicates ||
        (command >= CrossfadeFirst && command < CrossfadeFirst + 4))
        settingsUndo_ = SettingsSnapshot{theme_, userTheme_, reduceMotion_, player_.Normalize(), view_.showRemaining,
                                        miniOnTop_, library_.AutoScan(), library_.HideDuplicates(), player_.Crossfade()};
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
        case UndoSettings:
            if (settingsUndo_) {
                auto before = *settingsUndo_; settingsUndo_.reset();
                userTheme_ = before.user; reduceMotion_ = before.reduce; miniOnTop_ = before.top;
                view_.showRemaining = before.remaining; player_.SetNormalize(before.normalize);
                player_.SetCrossfade(before.crossfade);
                library_.SetAutoScan(before.autoScan); library_.SetHideDuplicates(before.hideDuplicates);
                library_.Save(libraryPath_); RestartWatcher(); StartLibraryScan();
                ApplyTheme(before.theme, false); ApplyTopmost();
            }
            break;
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
        case Compact: if (homeMode_) ToggleHome(); break;
        case Mini: EnterMini(); break;
        case workspace::OpenFiles: App::OpenFiles(false); break;
        case OpenFolder: App::OpenFiles(true); break;
        case workspace::ImportTheme: App::ImportTheme(); break;
        case workspace::ExportTheme: App::ExportTheme(); break;
        case PluginsFolder: OpenPluginsFolder(); break;
        case EnableDownloader:
            for (size_t i = 0; i < plugins_.Plugins().size(); ++i)
                if (plugins_.Plugins()[i].id == "downloader" && !plugins_.Plugins()[i].enabled) TogglePlugin((int)i);
            break;
        case SubmitDownload: {
            wchar_t value[8193]{}; GetWindowTextW(downloadEdit_, value, 8193);
            submittedDownload_ = text::WideToUtf8(value);
            if (submittedDownload_.empty()) { workspaceNotice_ = L"Вставьте ссылку в поле перед скачиванием."; break; }
            bool sent = false;
            for (size_t i = 0; i < plugins_.Pages().size(); ++i) {
                const auto& p = plugins_.Pages()[i];
                if (p.plugin < plugins_.Plugins().size() && plugins_.Plugins()[p.plugin].id == "downloader") {
                    plugins_.PageAction(i, 0, 1); sent = true; break;
                }
            }
            submittedDownload_.clear();
            if (!sent) workspaceNotice_ = L"Сначала включите плагин загрузчика.";
            break;
        }
        case FontSmaller:
        case FontLarger: {
            auto theme = theme_;
            theme.settings.textScale = std::clamp(theme.settings.textScale + (command == FontLarger ? .1 : -.1), .8, 1.5);
            userTheme_ = true; ApplyTheme(theme, false); break;
        }
        case Exit: CloseWindow(); break;
        case Stats: if (homeMode_) ToggleHome(); ShowStats(); break;
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
