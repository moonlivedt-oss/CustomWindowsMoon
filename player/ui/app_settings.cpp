// Студия плеера: карточки настроек по пяти разделам, поиск по всем разделам, ползунки,
// готовые наборы, свой цвет, история изменений и сброс раздела.
#include "ui/app.h"
#include "ui/workspace.h"
#include "core/text.h"
#include "view/home_model.h"
#include "view/layout.h"
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace ui {
using namespace workspace;
namespace {
std::wstring Wide(const std::string& s) { return text::Utf8ToWide(s); }
std::wstring Percent(double v) { return std::to_wstring((int)std::lround(v * 100)) + L"%"; }
}  // namespace

App::SettingsSnapshot App::CaptureSettings() const {
    return SettingsSnapshot{theme_,
                            userTheme_,
                            reduceMotion_,
                            player_.Normalize(),
                            view_.showRemaining,
                            miniOnTop_,
                            library_.AutoScan(),
                            library_.HideDuplicates(),
                            player_.Crossfade(),
                            windowFx_};
}

void App::RestoreSettings(const SettingsSnapshot& s) {
    userTheme_ = s.user;
    reduceMotion_ = s.reduce;
    miniOnTop_ = s.top;
    view_.showRemaining = s.remaining;
    player_.SetNormalize(s.normalize);
    player_.SetCrossfade(s.crossfade);
    windowFx_ = s.windowFx;
    library_.SetAutoScan(s.autoScan);
    library_.SetHideDuplicates(s.hideDuplicates);
    library_.Save(libraryPath_);
    RestartWatcher();
    StartLibraryScan();
    ApplyTheme(s.theme, false);
    ApplyTopmost();
    SaveHomePreferences();
}

void App::RememberSettings() {
    settingsHistory_.push_back(CaptureSettings());
    if (settingsHistory_.size() > 30) settingsHistory_.erase(settingsHistory_.begin());
}

void App::BuildSettings() {
    homeInput_.settingsSection = settingsSection_;
    homeInput_.previewMini = previewMini_;
    homeInput_.previewLarge = previewLarge_;
    homeInput_.canUndo = !settingsHistory_.empty();
    homeInput_.undoCount = (int)settingsHistory_.size();
    wchar_t query[256]{};
    if (settingsEdit_) GetWindowTextW(settingsEdit_, query, 256);
    homeInput_.settingsQuery = query;
    homeInput_.settingsSearchFocused = settingsEdit_ && GetFocus() == settingsEdit_;
    if (settingsEdit_)
        SendMessageW(settingsEdit_, EM_GETSEL, (WPARAM)&homeInput_.settingsStart, (LPARAM)&homeInput_.settingsEnd);
    const bool searching = query[0] != 0;
    for (int section = 0; section < 5; ++section) {
        if (!searching && section != settingsSection_) continue;
        const size_t first = homeInput_.cards.size();
        BuildSettingsSection(section);
        if (!searching) continue;
        // Поиск: остаются карточки, где совпали название, пояснение или подпись кнопки
        const auto needle = view::HomeLower(query);
        auto matches = [&](const Home::Card& c) {
            std::wstring hay = c.title + L" " + c.subtitle;
            for (const auto& control : c.controls) hay += L" " + control.label;
            return view::HomeLower(hay).find(needle) != std::wstring::npos;
        };
        const wchar_t* names[] = {L"Вид", L"Движение", L"Звук", L"Библиотека", L"Общее"};
        for (size_t i = first; i < homeInput_.cards.size();) {
            if (!matches(homeInput_.cards[i])) {
                homeInput_.cards.erase(homeInput_.cards.begin() + (long)i);
                continue;
            }
            homeInput_.cards[i].title = homeInput_.cards[i].title + L"  ·  " + names[section];
            ++i;
        }
    }
    if (searching && homeInput_.cards.empty())
        homeInput_.cards.push_back(
            {L"Ничего не нашлось", L"Попробуйте другое слово: «громкость», «цвет», «мини», «папка».", {}, {}});
}

void App::BuildSettingsSection(int section) {
    auto add = [&](std::wstring title, std::wstring subtitle, std::vector<Home::Control> controls = {}) {
        homeInput_.cards.push_back({std::move(title), std::move(subtitle), {}, std::move(controls), -1, false});
    };
    auto toggle = [&](std::wstring title, std::wstring hint, int command, bool on) {
        add(std::move(title), std::move(hint), {{on ? L"Включено" : L"Выключено", command, on, true}});
    };
    auto slider = [&](std::wstring title, std::wstring hint, int command, float value, std::wstring text) {
        Home::Control c{L"", command};
        c.value = std::clamp(value, 0.f, 1.f);
        c.valueText = std::move(text);
        add(std::move(title), std::move(hint), {c});
    };
    const auto& s = theme_.settings;
    if (section == 0) {
        add(L"Готовые наборы", L"Несколько настроек одним нажатием. «Сохранить свой» записывает всё оформление в файл.",
            {{L"Спокойно", PresetFirst},
             {L"Вечеринка", PresetFirst + 1},
             {L"Экономно", PresetFirst + 2},
             {L"Сохранить свой", workspace::ExportTheme}});
        std::vector<Home::Control> themes;
        const auto& ids = themes_.BuiltinIds();
        for (size_t i = 0; i < ids.size() && i < size_t(RemoveFolderFirst - ThemeFirst - 100); ++i) {
            const auto* t = themes_.Builtin(ids[i]);
            if (!t) continue;
            const auto bg = t->settings.winColor, ac = t->settings.accent;
            themes.push_back({Wide(t->name), ThemeFirst + (int)i, s.skin == ids[i], false,
                              D2D1::ColorF(bg.r, bg.g, bg.b), D2D1::ColorF(ac.r, ac.g, ac.b)});
        }
        add(L"Выберите настроение", L"Наведите на тему - образец справа покажет её сразу, щелчок применит.", themes);
        homeInput_.cards.back().themeGallery = true;
        const char* colors[] = {"#ED647B", "#A78BFA", "#57C9C1", "#83B87D", "#F2B56B", "#79AEF2"};
        const wchar_t* names[] = {L"Роза", L"Лаванда", L"Мята", L"Шалфей", L"Мёд", L"Небо"};
        std::vector<Home::Control> accents;
        bool preset = false;
        const auto bg = s.winColor;
        for (int i = 0; i < 6; ++i) {
            theme::Rgba ac;
            theme::ParseHexColor(colors[i], ac);
            const bool on = !s.accentFromCover && theme::HexColor(s.accent, false) == colors[i];
            preset = preset || on;
            accents.push_back(
                {names[i], AccentFirst + i, on, false, D2D1::ColorF(bg.r, bg.g, bg.b), D2D1::ColorF(ac.r, ac.g, ac.b)});
        }
        accents.push_back({L"Свой цвет…", CustomAccent, !preset && !s.accentFromCover, false,
                           D2D1::ColorF(bg.r, bg.g, bg.b), D2D1::ColorF(s.accent.r, s.accent.g, s.accent.b)});
        add(L"Акцентный цвет", L"Кнопки, полоса времени, волна и свечение. «Свой цвет» открывает палитру Windows.",
            accents);
        homeInput_.cards.back().swatches = true;
        toggle(L"Цвета из обложки", L"Акцент подстраивается под альбом - и в плеере, и на главной.", AccentFromCover,
               s.accentFromCover);
        add(L"Форма обложки", L"Квадратная, мягкая или круглая.",
            {{L"Квадрат", ShapeFirst, s.coverShape == "square"},
             {L"Скругление", ShapeFirst + 1, s.coverShape == "rounded"},
             {L"Круг", ShapeFirst + 2, s.coverShape == "circle"}});
        {
            // Рамки - ваши PNG в assets/frames; вариант появляется, когда файл на месте
            const wchar_t* frameNames[] = {L"Лунная", L"Неон", L"Винил", L"Закат", L"Иней", L"Кассета", L"Золото", L"Полароид", L"Плёнка", L"Сияние"};
            std::vector<Home::Control> frames{{L"Без рамки", CoverFrameFirst, coverFrame_ == 0 || !home_.HasFrame(coverFrame_)}};
            for (int i = 1; i <= 10; ++i)
                if (home_.HasFrame(i)) frames.push_back({frameNames[i - 1], CoverFrameFirst + i, coverFrame_ == i});
            add(L"Рамка обложки", frames.size() > 1 ? L"Вокруг обложки в «Сейчас играет». Картинки рамок лежат в assets/frames."
                                                    : L"Положите PNG с прозрачной серединой в assets/frames: lunar, neon, vinyl, sunset, frost или cassette.png.",
                frames);
        }
        slider(L"Подсветка обложки", L"Мягкое цветное свечение под изображением.", SliderAmbient, (float)s.ambient,
               Percent(s.ambient));
        toggle(L"Музыкальная волна", L"Движение под обложкой следует за музыкой.", Visualizer, s.vizStyle != "off");
        slider(L"Детализация волны", L"Меньше полос - спокойнее силуэт, больше - подробнее рисунок звука.",
               SliderVizBars, (float)((s.vizBars - 8) / 88), std::to_wstring((int)s.vizBars));
        slider(L"Размер текста", L"Название и исполнитель. Кнопки не перекрываются крупным текстом.", SliderTextScale,
               (float)((s.textScale - .8) / 1.2), Percent(s.textScale));
        add(L"Своя тема", L"Импортируйте оформление из файла или сохраните своё.",
            {{L"Импорт", workspace::ImportTheme}, {L"Экспорт", workspace::ExportTheme}});
    } else if (section == 1) {
        std::vector<Home::Control> motion;
        const wchar_t* motionNames[] = {L"Мягко", L"Пружинисто", L"Спокойно", L"Быстро", L"Плавно", L"Без анимаций"};
        for (int i = 0; i < 6; ++i)
            motion.push_back({motionNames[i], MotionFirst + i, s.motion == theme::MotionCharacters()[i].id});
        add(L"Характер движения", L"Пружины и скорость переходов. Переключите «Плеер / Мини» справа, чтобы увидеть.",
            motion);
        wchar_t speed[16];
        std::swprintf(speed, 16, L"×%.1f", s.speed);
        slider(L"Длительность анимаций", L"Левее - быстрее и короче, правее - медленнее и плавнее.", SliderSpeed,
               (float)((s.speed - .5) / 1.5), speed);
        const wchar_t* fxNames[] = {L"Своё для каждого", L"Всплытие", L"Подъём",     L"Раскрытие",
                                    L"Диафрагма",        L"Шторка",   L"Без эффекта"};
        std::vector<Home::Control> fx;
        for (int i = 0; i < 7; ++i) fx.push_back({fxNames[i], WindowFxFirst + i, windowFx_ == i});
        add(L"Как появляются окна", L"При запуске и смене вида. Нажмите вариант - окно покажет его сразу.", fx);
        toggle(L"Заставка при открытии и закрытии",
               L"Луна встречает при запуске и провожает при закрытии. Включите - и она покажется сразу.", IntroToggle,
               introEnabled_);
        toggle(L"Меньше движения", L"Спокойный интерфейс без живого фона и перелётов. Учитывает и настройку Windows.",
               ReduceMotion, reduceMotion_);
        toggle(L"Учитывать «Эффекты анимации» Windows",
               windowsAnimations_ ? L"Сейчас в Windows анимации включены."
                                  : L"Сейчас в Windows они выключены - поэтому плеер без движения. Выключите, чтобы анимации плеера работали.",
               FollowSystemMotion, followSystemMotion_);
    } else if (section == 2) {
        toggle(L"Выровнять громкость", L"Громкие и тихие треки ближе по уровню, если в файле есть метки ReplayGain.",
               Normalize, player_.Normalize());
        const double cf = player_.Crossfade();
        wchar_t fade[24];
        std::swprintf(fade, 24, L"%.1f с", cf);
        slider(L"Плавный переход между треками",
               L"Конец трека затихает, следующий нарастает. Треки одного альбома идут стыком.", SliderCrossfade,
               (float)(cf / 12), cf <= 0 ? std::wstring(L"Стык") : std::wstring(fade));
        toggle(L"Показывать, сколько осталось", L"Под обложкой - обратный отсчёт вместо полной длительности.",
               Remaining, view_.showRemaining);
    } else if (section == 3) {
        add(L"Ваша музыка",
            scanner_.Running() ? L"Ищем новые треки…" : L"Папки с музыкой. Файлы остаются на своих местах.",
            {{L"Добавить папку", AddFolder}, {L"Пересканировать", Rescan}, {L"К библиотеке", OpenLibrary}});
        toggle(L"Следить за папками", L"Новые файлы появляются в библиотеке сами.", AutoScan, library_.AutoScan());
        toggle(L"Скрывать одинаковые файлы", L"Точные копии показываются один раз. Файлы не удаляются.", HideDuplicates,
               library_.HideDuplicates());
        const auto& folders = library_.Folders();
        for (size_t i = 0; i < folders.size() && i < size_t(PluginFirst - RemoveFolderFirst); ++i)
            add(L"Музыкальная папка", folders[i].wstring(), {{L"Убрать из списка", RemoveFolderFirst + (int)i}});
        // Загрузки: папка и вход для личных плейлистов - через действия плагина загрузчика
        std::vector<Home::Control> folder{{L"Открыть раздел", workspace::OpenDownloads}};
        std::string login;
        const auto& plugins = plugins_.Plugins();
        const auto& pages = plugins_.Pages();
        for (size_t page = 0; page < pages.size(); ++page) {
            if (pages[page].plugin >= plugins.size() || plugins[pages[page].plugin].id != "downloader") continue;
            for (const auto& a : pages[page].actions) {
                if (a.id == 3) login = a.label;
                if (a.id == 2) {
                    folder.push_back({L"Открыть папку", PageActionFirst + (int)workspaceBindings_.size()});
                    workspaceBindings_.push_back({page, 0, a.id});
                }
            }
        }
        const auto& menu = plugins_.MenuItems();
        for (size_t i = 0; i < menu.size() && i < size_t(PageActionFirst - PluginMenuFirst); ++i)
            if (menu[i].plugin < plugins.size() && plugins[menu[i].plugin].id == "downloader" && menu[i].id == 2)
                folder.push_back({L"Выбрать другую", PluginMenuFirst + (int)i});
        add(L"Куда сохранять загрузки", L"Скачанное сразу попадает в библиотеку и очередь.", folder);
        add(L"Вход для личных плейлистов", L"Только для «Мне нравится» и закрытых списков. Пароль вводить не нужно.",
            {{L"Без входа", BrowserNone, login.empty() || login == "Вход: нет"},
             {L"Firefox", BrowserFirefox, login == "Вход: Firefox"},
             {L"Chrome", BrowserChrome, login == "Вход: Chrome"},
             {L"Edge", BrowserEdge, login == "Вход: Edge"}});
    } else {
        add(L"Размер окна", L"Главная, компактный плеер или мини-полоска.",
            {{L"Открыть плеер", Compact}, {L"Мини-полоска", Mini}});
        toggle(L"Мини-плеер поверх окон", L"Мини-полоска остаётся видна поверх других программ.", MiniTop, miniOnTop_);
        toggle(L"Высокий контраст", L"Чёрный фон, белый текст и системный цвет выделения, без живого фона. Включается сам, если он включён в Windows.",
               HighContrastToggle, HighContrast());
        add(L"Открыть музыку", L"Файлы или папка. Можно и просто перетащить в окно.",
            {{L"Файлы", workspace::OpenFiles}, {L"Папка", OpenFolder}});
        add(L"История", L"Что слушали чаще всего. Статистика хранится только у вас.", {{L"Итоги", Stats}});
        BuildShortcutCards();
        add(L"PlayerMoon", L"Настройки применяются сразу; изменения можно отменить кнопкой под образцом.",
            {{L"Закрыть плеер", Exit}});
    }
}

// Значение ползунка: доля 0..1 от положения курсора. begin - начало перетаскивания (запомнить для отмены)
void App::SliderCommand(int command, float f, bool begin) {
    if (begin) RememberSettings();
    if (command == SliderCrossfade) {
        player_.SetCrossfade(std::round(f * 24) / 2);  // шаг полсекунды
        Invalidate();
        return;
    }
    auto next = theme_;
    auto& s = next.settings;
    switch (command) {
        case SliderTextScale:
            s.textScale = .8 + std::round(f * 14) * .05;
            break;
        case SliderVizBars:
            s.vizBars = 8 + std::round(f * 22) * 4;
            break;
        case SliderSpeed:
            s.speed = .5 + std::round(f * 15) * .1;
            break;
        case SliderAmbient:
            s.ambient = std::round(f * 20) / 20;
            break;
        default:
            return;
    }
    userTheme_ = true;
    ApplyTheme(next, false);
    MarkDirty();
    Invalidate();
}

bool App::SettingsCommand(int command) {
    if (command >= CoverFrameFirst && command < CoverFrameFirst + 11) {
        RememberSettings();
        coverFrame_ = command - CoverFrameFirst;
        SaveHomePreferences();
        if (coverFrame_ && !home_.NowPlayingOpen()) home_.ToggleNowPlaying();  // показать рамку сразу
        return true;
    }
    if (command == FollowSystemMotion) {
        followSystemMotion_ = !followSystemMotion_;
        ReadSystemReduce();
        ApplyTheme(theme_, false);
        SaveHomePreferences();
        return true;
    }
    if (command == HighContrastToggle) {
        highContrast_ = !highContrast_;
        SaveHomePreferences();
        return true;
    }
    if (command == IntroToggle) {
        introEnabled_ = !introEnabled_;
        SaveHomePreferences();
        if (introEnabled_ && MotionAllowed()) intro_.Start(Now(), false);  // показать сразу
        return true;
    }
    if (command >= SettingsTabFirst && command < SettingsTabFirst + 5) {
        settingsSection_ = command - SettingsTabFirst;
        home_.ResetScroll();
        return true;
    }
    if (command == UndoSettings || command == UndoAll) {
        if (settingsHistory_.empty()) return true;
        const auto target = command == UndoAll ? settingsHistory_.front() : settingsHistory_.back();
        if (command == UndoAll)
            settingsHistory_.clear();
        else
            settingsHistory_.pop_back();
        RestoreSettings(target);
        return true;
    }
    if (command == CustomAccent) {
        static COLORREF custom[16]{};
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = hwnd_;
        const auto& a = theme_.settings.accent;
        cc.rgbResult = RGB((int)(a.r * 255), (int)(a.g * 255), (int)(a.b * 255));
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColorW(&cc)) return true;
        RememberSettings();
        auto next = theme_;
        next.settings.accent = {GetRValue(cc.rgbResult) / 255.f, GetGValue(cc.rgbResult) / 255.f,
                                GetBValue(cc.rgbResult) / 255.f, 1};
        next.settings.accent2 = next.settings.accent;
        next.settings.accentFromCover = false;
        userTheme_ = true;
        ApplyTheme(next, false);
        return true;
    }
    if (command >= PresetFirst && command < PresetFirst + 3) {
        RememberSettings();
        auto next = theme_;
        auto& s = next.settings;
        auto character = [&](const char* id) {
            for (const auto& c : theme::MotionCharacters())
                if (c.id == std::string(id)) {
                    s.motion = c.id;
                    s.stiffness = c.stiffness;
                    s.damping = c.damping;
                    s.speed = c.speed == 0 ? 1 : c.speed;
                }
        };
        if (command == PresetFirst) {  // Спокойно
            character("calm");
            s.ambient = .3;
            s.vizStyle = "lines";
            windowFx_ = 2;
            reduceMotion_ = false;
        } else if (command == PresetFirst + 1) {  // Вечеринка
            character("bouncy");
            s.ambient = 1;
            s.vizStyle = "bars";
            s.vizBars = 64;
            s.accentFromCover = true;
            windowFx_ = 0;
            reduceMotion_ = false;
        } else {  // Экономно: меньше кадров и эффектов
            s.ambient = 0;
            s.vizStyle = "off";
            windowFx_ = 6;
            reduceMotion_ = true;
        }
        userTheme_ = true;
        ApplyTheme(next, false);
        SaveHomePreferences();
        workspaceNotice_ = L"Набор применён. Не понравилось - «Отменить» под образцом.";
        return true;
    }
    if (command == ResetSection) {
        RememberSettings();
        const auto defaults = themes_.Make(theme_.settings.skin);
        auto next = theme_;
        if (settingsSection_ == 0) {
            // Вид - как у выбранной темы; движение не трогаем
            const auto keep = theme_.settings;
            next = defaults;
            next.settings.motion = keep.motion;
            next.settings.stiffness = keep.stiffness;
            next.settings.damping = keep.damping;
            next.settings.speed = keep.speed;
            ApplyTheme(next, false);
        } else if (settingsSection_ == 1) {
            next.settings.motion = defaults.settings.motion;
            next.settings.stiffness = defaults.settings.stiffness;
            next.settings.damping = defaults.settings.damping;
            next.settings.speed = defaults.settings.speed;
            windowFx_ = 0;
            reduceMotion_ = false;
            ApplyTheme(next, false);
        } else if (settingsSection_ == 2) {
            player_.SetNormalize(true);
            player_.SetCrossfade(0);
            view_.showRemaining = false;
        } else if (settingsSection_ == 3) {
            library_.SetAutoScan(true);
            library_.SetHideDuplicates(true);
            library_.Save(libraryPath_);
            RestartWatcher();
            StartLibraryScan();
        } else {
            miniOnTop_ = true;
            ApplyTopmost();
            ResetShortcuts();
        }
        SaveHomePreferences();
        workspaceNotice_ = L"Раздел вернулся к настройкам по умолчанию.";
        return true;
    }
    return false;
}

// Предпросмотр по наведению: образец показывает тему, цвет или форму, пока курсор над вариантом
void App::UpdateSettingsHoverPreview() {
    const int hovered = home_.model.page == view::HomeModel::Settings ? home_.HoveredCommand() : -1;
    const bool previewable = (hovered >= ThemeFirst && hovered < ThemeFirst + (int)themes_.BuiltinIds().size()) ||
                             (hovered >= AccentFirst && hovered < AccentFirst + 6) ||
                             (hovered >= ShapeFirst && hovered < ShapeFirst + 3);
    const int want = previewable ? hovered : -1;
    if (want == previewHover_) return;
    previewHover_ = want;
    auto shown = theme_;
    if (want >= ThemeFirst)
        shown = themes_.Make(themes_.BuiltinIds()[(size_t)(want - ThemeFirst)]);
    else if (want >= AccentFirst) {
        const char* colors[] = {"#ED647B", "#A78BFA", "#57C9C1", "#83B87D", "#F2B56B", "#79AEF2"};
        theme::ParseHexColor(colors[want - AccentFirst], shown.settings.accent);
        shown.settings.accent2 = shown.settings.accent;
        shown.settings.accentFromCover = false;
    } else if (want >= ShapeFirst) {
        const char* shapes[] = {"square", "rounded", "circle"};
        shown.settings.coverShape = shapes[want - ShapeFirst];
    }
    // Раскладка - та, что умеет плеер (как в ApplyTheme): иначе геометрия «Заката» ломает образец
    settingsPreview_.Apply(shown, themes_.Layout(view::MakeLayout(shown.settings.layout)->Id()), systemReduce_ || reduceMotion_, Now());
}
}  // namespace ui
