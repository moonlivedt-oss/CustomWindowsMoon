#include "ui/home.h"
#include "ui/intro.h"
#include "view/layout.h"
#include <functional>
#include "ui/settings_preview.h"
#include "ui/workspace.h"
#include "core/text.h"
#include "plugins/sdk/playermoon_plugin.h"
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;
namespace {
bool Save(ui::Renderer& renderer, const std::filesystem::path& path) {
    ComPtr<ID2D1Image> image;
    ComPtr<ID2D1Bitmap1> source, cpu;
    auto* dc = renderer.Context();
    dc->GetTarget(&image);
    if (!image || FAILED(image.As(&source))) return false;
    const auto size = source->GetPixelSize();
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                    source->GetPixelFormat());
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, properties, &cpu)) ||
        FAILED(cpu->CopyFromBitmap(nullptr, source.Get(), nullptr)))
        return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    ComPtr<IWICBitmap> bitmap;
    auto* wic = renderer.Wic();
    HRESULT result = wic->CreateBitmapFromMemory(size.width, size.height, GUID_WICPixelFormat32bppPBGRA, mapped.pitch,
                                                 mapped.pitch * size.height, mapped.bits, &bitmap);
    cpu->Unmap();
    if (FAILED(result)) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICFormatConverter> converter;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(wic->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(size.width, size.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
           SUCCEEDED(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0,
                                           WICBitmapPaletteTypeCustom)) &&
           SUCCEEDED(frame->WriteSource(converter.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
           SUCCEEDED(encoder->Commit());
}
}  // namespace
// An invisible, isolated renderer fixture; never attaches to a running player or browser.
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    int failures = 0, frames = 0;
    {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        const auto directory = std::filesystem::path(executable).parent_path();
        // Снимки - в свою папку: рядом с exe их десятки, и они мешаются со сборкой
        const auto shots = directory / "test-output";
        std::filesystem::create_directories(shots);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PlayerMoonSettingsRenderFixture";
        RegisterClassW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"Settings render fixture", WS_POPUP, 0, 0, 1200, 800, nullptr,
                                    nullptr, wc.hInstance, nullptr);
        ui::Renderer renderer;
        if (!window || !renderer.Init(window)) return 1;
        ui::TextSystem text;
        ui::IconSet icons;
        ui::CoverCache covers;
        ui::Home home;
        ui::SettingsPreview preview;
        theme::ThemeLibrary themes;
        if (!text.Init(renderer.DWrite(), directory / "assets/fonts") || !themes.Load(directory / "assets")) return 1;
        icons.Init(renderer.Factory());
        covers.Init(renderer.Wic());
        home.Init(window, &renderer, &text);
        preview.Init(&renderer, &text, &icons, &covers);
        home.model.page = view::HomeModel::Settings;
        view::TrackText track;
        track.title = "Ночная орбита";
        track.artist = "PlayerMoon";
        track.album = "Ваше оформление";
        track.serial = 123;
        track.palette = theme::MakeCoverPalette(track.title);
        const char* variants[] = {"moon", "frost", "moon", "moon", "neon", "sunset"};
        for (int variant = 0; variant < 6; ++variant) {
            const UINT width = variant == 2 ? 900 : 1200, height = variant == 2 ? 640 : 800;
            SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
            renderer.Resize(width, height);
            if (variant == 2) renderer.ResetDevice();
            auto theme = themes.Make(variants[variant]);
            if (variant == 3) {
                theme.settings.textScale = 1.5;
                theme.settings.coverShape = "circle";
            }
            // Раскладка - как в App::ApplyTheme: «Закат» просит horizon, а рисуется classic
            preview.Apply(theme, themes.Layout(view::MakeLayout(theme.settings.layout)->Id()), variant != 4,
                          variant * 10.);
            ui::Home::Input input;
            input.size = {(float)width, (float)height};
            input.title = L"Ночная орбита";
            input.artist = L"PlayerMoon";
            input.reducedMotion = variant != 4;
            input.previewLarge = variant == 3;
            input.canUndo = true;
            input.previewMini = variant == 1;
            ui::Home::Card gallery;
            gallery.title = L"Выберите настроение";
            gallery.subtitle = L"Смотрите на плеер справа: это настоящее оформление, которое вы меняете.";
            gallery.themeGallery = true;
            for (size_t i = 0; i < themes.BuiltinIds().size(); ++i) {
                const auto preset = themes.Make(themes.BuiltinIds()[i]);
                const auto bg = preset.settings.winColor, accent = preset.settings.accent;
                gallery.controls.push_back({text::Utf8ToWide(preset.name), ui::workspace::ThemeFirst + (int)i,
                                            preset.settings.skin == theme.settings.skin, false,
                                            D2D1::ColorF(bg.r, bg.g, bg.b),
                                            D2D1::ColorF(accent.r, accent.g, accent.b)});
            }
            input.cards.push_back(gallery);
            input.cards.push_back(
                {L"Размер текста — 100%",
                 L"Название и исполнитель в образце меняются сразу.",
                 {},
                 {{L"Аа  Меньше", ui::workspace::FontSmaller}, {L"Аа  Больше", ui::workspace::FontLarger}}});
            if (variant == 1) {
                input.settingsSection = 1;
                input.cards = {{L"Выровнять громкость",
                                L"Громкие и тихие треки ближе по уровню, если в файле есть метки ReplayGain.",
                                {},
                                {{L"Включено", ui::workspace::Normalize, true, true}}},
                               {L"Плавный переход между треками",
                                L"Конец трека затихает, следующий в это время нарастает. Треки одного альбома идут "
                                L"стыком, как записаны.",
                                {},
                                {{L"Стык", ui::workspace::CrossfadeFirst},
                                 {L"3 с", ui::workspace::CrossfadeFirst + 1},
                                 {L"6 с", ui::workspace::CrossfadeFirst + 2, true},
                                 {L"10 с", ui::workspace::CrossfadeFirst + 3}}},
                               {L"Показывать, сколько осталось",
                                L"Посмотрите на время под обложкой в образце.",
                                {},
                                {{L"Выключено", ui::workspace::Remaining, false, true}}}};
            }
            for (int step = 0; step < (variant == 4 ? 18 : 3); ++step) {
                auto* dc = renderer.BeginFrame();
                if (!dc) {
                    ++failures;
                    break;
                }
                input.settingsPreview =
                    preview.Render(dc, track, variant * 10. + step * .04, input.previewMini, variant == 4, false);
                input.previewSize = preview.Size();
                if (!input.settingsPreview) ++failures;
                home.Paint(dc, input);
                if (!renderer.EndFrame()) ++failures;
                ++frames;
            }
            if (!Save(renderer, shots / ("settings-preview-" + std::to_string(variant) + ".png"))) ++failures;
        }
        // Главная и загрузки с вымышленными треками и заданиями: снимки для работы над оформлением
        {
            SetWindowPos(window, nullptr, 0, 0, 1200, 800, SWP_NOACTIVATE | SWP_NOZORDER);
            renderer.Resize(1200, 800);
            const wchar_t* titles[] = {L"Ночная орбита", L"Серебро",      L"Тихий порт",      L"Северный ветер",
                                       L"Полнолуние",    L"Стекло и дым", L"Медленный город", L"Ртуть"};
            const wchar_t* artists[] = {L"Луна", L"Эхо", L"Маяк", L"Аврора"};
            for (int i = 0; i < 8; ++i)
                home.model.tracks.push_back({directory / ("fake-" + std::to_string(i) + ".mp3"), titles[i],
                                             artists[i % 4], L"Альбом", 180. + i * 17, 1000 - i, (uint32_t)i});
            ui::Home::Input input;
            input.size = {1200, 800};
            input.title = L"Ночная орбита";
            input.artist = L"Луна";
            input.playback.duration = 240;
            input.playback.position = 96;
            input.playback.playing = true;
            for (const auto& t : home.model.tracks) input.queue.push_back(t);
            input.playback.index = 0;
            input.current = home.model.tracks[0].path;
            const view::HomeModel::Page pages[] = {view::HomeModel::Home, view::HomeModel::Downloads,
                                                   view::HomeModel::Home};
            const char* names[] = {"home", "downloads", "home-neon"};
            for (int shot = 0; shot < 3; ++shot) {
                if (shot == 2) {
                    // Другая тема: бирюзовый акцент и холодный оттенок фона
                    input.colors.accent = D2D1::ColorF(0x2dd4bf);
                    input.colors.accent2 = D2D1::ColorF(0x60a5fa);
                    input.colors.bg = ui::MixColor(input.colors.bg, D2D1::ColorF(0x0b1d2a), .5f);
                    input.colors.surface = ui::MixColor(input.colors.surface, D2D1::ColorF(0x0b1d2a), .4f);
                    input.colors.card = ui::MixColor(input.colors.card, D2D1::ColorF(0x0b1d2a), .3f);
                }
                home.model.page = pages[shot];
                home.ResetScroll();
                input.cards.clear();
                if (pages[shot] == view::HomeModel::Downloads) {
                    input.downloadEnabled = true;
                    input.cards = {{L"Аврора - Северный ветер",
                                    L"YouTube Music, трек 3 из 12",
                                    L"42%  2.1 МБ/с",
                                    {{L"Отмена", ui::workspace::PageActionFirst}},
                                    .3f},
                                   {L"Маяк - Тихий порт",
                                    L"Яндекс Музыка",
                                    L"Готово",
                                    {{L"Играть", ui::workspace::PageActionFirst + 1},
                                     {L"Показать", ui::workspace::PageActionFirst + 2},
                                     {L"Убрать", ui::workspace::PageActionFirst + 3}}},
                                   {L"https://youtu.be/unavailable",
                                    L"Видео недоступно",
                                    L"Ошибка",
                                    {{L"Повторить", ui::workspace::PageActionFirst + 4},
                                     {L"Убрать", ui::workspace::PageActionFirst + 5}},
                                    -1,
                                    true},
                                   {L"Загрузчик готов · yt-dlp 2026.10.01",
                                    L"YouTube Music, YouTube и Яндекс Музыка поддерживаются",
                                    L"",
                                    {{L"Обновить yt-dlp", ui::workspace::PageActionFirst + 6}}}};
                }
                if (pages[shot] == view::HomeModel::Downloads) {
                    const int states[] = {PM_ROW_ACTIVE, PM_ROW_DONE, PM_ROW_ERROR, PM_ROW_DONE};
                    for (size_t c = 0; c < input.cards.size(); ++c) input.cards[c].job = states[c];
                    input.cards[3].tools = true;
                }
                input.ambient = true;
                input.energy = .55f;
                for (int b = 0; b < 8; ++b) input.bands[b] = .3f + .08f * (b % 3);
                for (int step = 0; step < 40; ++step) {
                    if (step == 6) Save(renderer, shots / ("design-" + std::string(names[shot]) + "-enter.png"));
                    auto* dc = renderer.BeginFrame();
                    if (!dc) {
                        ++failures;
                        break;
                    }
                    home.Paint(dc, input);
                    if (!renderer.EndFrame()) ++failures;
                    ++frames;
                    Sleep(16);
                }
                if (!Save(renderer, shots / ("design-" + std::string(names[shot]) + ".png"))) ++failures;
            }
            // «Сейчас играет»: синхронный текст и волна
            home.model.page = view::HomeModel::Home;
            home.ToggleNowPlaying();
            input.lyrics = {
                L"Над водой горит луна",     L"Тихо кружится винил", L"Ты на этой частоте", L"Где-то между двух огней",
                L"Ночь звучит как обещание", L"И мы слушаем её",     L"До рассвета"};
            input.lyricTimes = {80, 86, 92, 98, 104, 110, 116};
            input.lyricsState = 2;
            input.spectrum.resize(48);
            for (size_t b = 0; b < 48; ++b)
                input.spectrum[b] = .25f + .55f * std::fabs(std::sin(b * .37f)) * (1 - b / 60.f);
            for (int step = 0; step < 40; ++step) {
                auto* dc = renderer.BeginFrame();
                if (!dc) {
                    ++failures;
                    break;
                }
                home.Paint(dc, input);
                if (!renderer.EndFrame()) ++failures;
                ++frames;
                Sleep(16);
            }
            if (!Save(renderer, shots / "design-now.png")) ++failures;
            home.ToggleNowPlaying();
            // Новые настройки: ползунки, свой цвет, поиск; затем палитра команд
            auto render = [&](const char* name) {
                for (int step = 0; step < 40; ++step) {
                    auto* dc = renderer.BeginFrame();
                    if (!dc) {
                        ++failures;
                        break;
                    }
                    input.settingsPreview = preview.Render(dc, track, step * .04, false, false, false);
                    input.previewSize = preview.Size();
                    home.Paint(dc, input);
                    if (!renderer.EndFrame()) ++failures;
                    ++frames;
                    Sleep(16);
                }
                if (!Save(renderer, shots / name)) ++failures;
            };
            home.model.page = view::HomeModel::Settings;
            input.settingsSection = 0;
            input.canUndo = true;
            input.undoCount = 3;
            ui::Home::Control size{L"", ui::workspace::SliderTextScale};
            size.value = .3f;
            size.valueText = L"100%";
            ui::Home::Control glow{L"", ui::workspace::SliderAmbient};
            glow.value = .6f;
            glow.valueText = L"60%";
            input.cards = {{L"Готовые наборы",
                            L"Несколько настроек одним нажатием.",
                            {},
                            {{L"Спокойно", ui::workspace::PresetFirst},
                             {L"Вечеринка", ui::workspace::PresetFirst + 1},
                             {L"Экономно", ui::workspace::PresetFirst + 2},
                             {L"Сохранить свой", ui::workspace::ExportTheme}}},
                           {L"Подсветка обложки", L"Мягкое цветное свечение под изображением.", {}, {glow}},
                           {L"Размер текста", L"Название и исполнитель.", {}, {size}},
                           {L"Цвета из обложки",
                            L"Акцент подстраивается под альбом.",
                            {},
                            {{L"Включено", ui::workspace::AccentFromCover, true, true}}},
                           {L"Форма обложки",
                            L"Квадратная, мягкая или круглая.",
                            {},
                            {{L"Квадрат", ui::workspace::ShapeFirst},
                             {L"Скругление", ui::workspace::ShapeFirst + 1, true},
                             {L"Круг", ui::workspace::ShapeFirst + 2}}}};
            {
                // Акцент - кружками: шесть цветов и свой
                ui::Home::Card colors{L"Акцентный цвет", L"Кнопки, полоса времени, волна и свечение.", {}, {}};
                const uint32_t hex[] = {0xED647B, 0xA78BFA, 0x57C9C1, 0x83B87D, 0xF2B56B, 0x79AEF2, 0xFFB86B};
                const wchar_t* swatchNames[] = {L"Роза", L"Лаванда", L"Мята",      L"Шалфей",
                                                L"Мёд",  L"Небо",    L"Свой цвет…"};
                for (int c = 0; c < 7; ++c)
                    colors.controls.push_back({swatchNames[c],
                                               c < 6 ? ui::workspace::AccentFirst + c : ui::workspace::CustomAccent,
                                               c == 0, false, D2D1::ColorF(0x181920), D2D1::ColorF(hex[c])});
                colors.swatches = true;
                input.cards.insert(input.cards.begin() + 1, colors);
            }
            render("design-settings.png");
            // Снимок в середине прокрутки: выбранные кнопки не должны вытягиваться вслед за колесом
            home.Scroll(-1);
            for (int step = 0; step < 4; ++step) {
                auto* sdc = renderer.BeginFrame();
                if (!sdc) break;
                home.Paint(sdc, input);
                renderer.EndFrame();
                Sleep(16);
            }
            if (!Save(renderer, shots / "anim-settings-scroll.png")) ++failures;
            home.ResetScroll();
            // Широкое окно и пустое «Любимое»
            SetWindowPos(window, nullptr, 0, 0, 1920, 1200, SWP_NOACTIVATE | SWP_NOZORDER);
            renderer.Resize(1920, 1200);
            input.size = {1920, 1200};
            home.model.page = view::HomeModel::Home;
            render("design-home-wide.png");
            home.model.page = view::HomeModel::Favorites;
            render("design-favorites-empty.png");
            // Подборки на широкой главной, фокус клавиатуры и высокий контраст
            home.model.page = view::HomeModel::Home;
            home.MoveFocus(1);
            home.MoveFocus(1);
            render("design-mixes-focus.png");
            input.skin = "neon";
            render("design-banner-neon.png");
            input.skin.clear();
            home.ClearFocus();
            {
                const auto saved = input.colors;
                input.colors.bg = D2D1::ColorF(0, 0, 0);
                input.colors.surface = D2D1::ColorF(0x0a0a0a);
                input.colors.card = D2D1::ColorF(0x1c1c1c);
                input.colors.white = D2D1::ColorF(1, 1, 1);
                input.colors.muted = D2D1::ColorF(0xe0e0e0);
                input.colors.accent = input.colors.accent2 = D2D1::ColorF(0x1aebff);
                input.ambient = false;
                input.reducedMotion = true;
                input.highContrast = true;
                render("design-contrast.png");
                input.highContrast = false;
                input.colors = saved;
                input.ambient = true;
                input.reducedMotion = false;
            }
            // «Итоги» историей
            input.story.period = L"Ноябрь 2026";
            input.story.seconds = 45600;
            input.story.hours = L"12 ч 40 мин";
            input.story.playsLine = L"214 прослушиваний · 96 треков · 41 исполнитель";
            input.story.activeDays = 23;
            input.story.topHour = 21;
            for (int h = 0; h < 24; ++h)
                input.story.hourShare[h] = .1f + .9f * std::exp(-std::pow((h - 21) / 3.5f, 2.f));
            input.story.artists = {{L"Луна", L"", L"3 ч 10 мин · 52 прослушивания", 1},
                                   {L"Аврора", L"", L"", .7f},
                                   {L"Маяк", L"", L"", .55f},
                                   {L"Эхо", L"", L"", .4f},
                                   {L"Ртуть", L"", L"", .3f}};
            input.story.tracks = {{L"Ночная орбита", L"Луна", L"31 прослушивание", 0}};
            input.story.topTrackPath = home.model.tracks[0].path;
            home.OpenStory();
            render("story-0.png");
            home.StoryStep(1);
            render("story-1.png");
            home.StoryStep(2);
            render("story-3.png");
            home.StoryStep(1);
            render("story-4.png");
            input.story.background = 1;
            render("story-4-records.png");
            input.story.background = 0;
            home.CloseStory();
            {
                // Пустые страницы: «Скачивание» с иллюстрацией и шапка «Плагинов»
                const auto savedCards = input.cards;
                home.model.page = view::HomeModel::Downloads;
                ui::Home::Card none{
                    L"Загрузок пока нет", L"Скопируйте ссылку на трек или плейлист и нажмите «Скачать».", {}, {}};
                none.illustration = ui::Home::EmptyDownloads;
                input.cards = {none};
                render("downloads-empty.png");
                home.model.page = view::HomeModel::Plugins;
                input.cards = {{L"Загрузка музыки  ·  1.0",
                                L"Скачивает треки и плейлисты по ссылке.",
                                {},
                                {{L"Выключить", 600, true}}}};
                render("plugins-header.png");
                input.cards = savedCards;
                home.model.page = view::HomeModel::Home;
            }
            home.CloseStory();
            // Караоке в «Сейчас играет»
            input.karaoke = true;
            input.playback.position = 94;
            home.ToggleNowPlaying();
            render("design-karaoke.png");
            home.ToggleNowPlaying();
            input.karaoke = false;
            render("design-after-karaoke.png");
            // Рамки и винил в «Сейчас играет»
            home.ToggleNowPlaying();
            const char* frameShots[] = {
                "frame-lunar.png",    "frame-neon.png", "frame-vinyl.png",    "frame-sunset.png", "frame-frost.png",
                "frame-cassette.png", "frame-gold.png", "frame-polaroid.png", "frame-film.png",   "frame-aurora.png"};
            for (int f = 1; f <= 10; ++f) {
                input.coverFrame = home.HasFrame(f) ? f : 0;
                render(frameShots[f - 1]);
            }
            input.coverFrame = 0;
            // Текста нет: иллюстрация над сообщением
            {
                const auto savedLyrics = input.lyrics;
                const auto savedTimes = input.lyricTimes;
                input.lyrics.clear();
                input.lyricTimes.clear();
                input.lyricsState = 4;
                render("now-no-lyrics.png");
                input.lyricsState = 5;
                render("now-instrumental.png");
                input.lyricsState = 6;
                render("now-offline.png");
                // Трек без обложки: свой фон вместо размытия
                const auto savedCurrent = input.current;
                input.current = L"Z:/нет-такого-файла.mp3";
                render("now-no-cover.png");
                input.current = savedCurrent;
                input.lyrics = savedLyrics;
                input.lyricTimes = savedTimes;
                input.lyricsState = 2;
            }
            home.ToggleNowPlaying();
            render("design-after-frames.png");
            // Шапки разделов
            home.model.page = view::HomeModel::Library;
            render("page-library.png");
            home.model.page = view::HomeModel::Queue;
            render("page-queue.png");
            home.model.page = view::HomeModel::Home;
            // Прожектор под курсором на плитке и волна от щелчка
            home.Move({600, 560});
            home.Press({600, 560});
            for (int step = 0; step < 12; ++step) {
                auto* sdc = renderer.BeginFrame();
                if (!sdc) break;
                home.Paint(sdc, input);
                renderer.EndFrame();
                Sleep(16);
            }
            if (!Save(renderer, shots / "fx-spotlight-ripple.png")) ++failures;
            home.Release({600, 560});
            home.Move({-1, -1});
            // Белый акцент («Стекло»): надписи на кнопках с акцентом должны быть тёмными
            {
                const auto saved = input.colors;
                const auto savedCards = input.cards;
                input.colors.accent = input.colors.accent2 = D2D1::ColorF(0xf4f4f6);
                home.model.page = view::HomeModel::Settings;
                const wchar_t* keys[] = {L"Пауза и воспроизведение  ·  Пробел",
                                         L"Следующий трек  ·  Ctrl+→",
                                         L"Предыдущий трек  ·  Ctrl+←",
                                         L"Мини-полоска  ·  Ctrl+M",
                                         L"Главная или плеер  ·  Ctrl+H",
                                         L"Поиск  ·  Ctrl+F",
                                         L"Настройки  ·  Ctrl+,",
                                         L"Сейчас играет  ·  F11",
                                         L"Палитра команд  ·  Ctrl+K"};
                ui::Home::Card shortcuts{L"Клавиши", L"Нажмите действие, затем новое сочетание.", {}, {}};
                for (int k = 0; k < 9; ++k) shortcuts.controls.push_back({keys[k], ui::workspace::ShortcutFirst + k});
                input.cards = {{L"Форма обложки",
                                L"Квадратная, мягкая или круглая.",
                                {},
                                {{L"Квадрат", ui::workspace::ShapeFirst},
                                 {L"Скругление", ui::workspace::ShapeFirst + 1, true},
                                 {L"Круг", ui::workspace::ShapeFirst + 2}}},
                               shortcuts};
                render("design-white-accent.png");
                // Перетекание: «Скругление» -> «Круг», снимок в середине пути
                input.cards[0].controls[1].active = false;
                input.cards[0].controls[2].active = true;
                for (int step = 0; step < 7; ++step) {
                    auto* fdc = renderer.BeginFrame();
                    if (!fdc) break;
                    home.Paint(fdc, input);
                    renderer.EndFrame();
                    Sleep(16);
                }
                if (!Save(renderer, shots / "anim-liquid.png")) ++failures;
                // Эквалайзер на плитке при «меньше движения» - по полосам звука
                home.model.page = view::HomeModel::Home;
                input.reducedMotion = true;
                input.ambient = false;
                input.bandsLive = true;
                for (int k = 0; k < 8; ++k) input.bands[k] = .2f + .2f * (k % 3);
                render("design-eq-reduced.png");
                input.colors = saved;
                input.cards = savedCards;
                input.reducedMotion = false;
                input.ambient = true;
            }
            home.model.page = view::HomeModel::Settings;
            SetWindowPos(window, nullptr, 0, 0, 1200, 800, SWP_NOACTIVATE | SWP_NOZORDER);
            renderer.Resize(1200, 800);
            input.size = {1200, 800};
            input.settingsQuery = L"цвет";
            input.cards.resize(2);
            input.cards[0].title = L"Акцентный цвет  ·  Вид";
            input.cards[1].title = L"Цвета из обложки  ·  Вид";
            input.cards[1].controls = {{L"Включено", ui::workspace::AccentFromCover, true, true}};
            render("design-settings-search.png");
            input.settingsQuery.clear();
            home.model.page = view::HomeModel::Home;
            input.paletteOpen = true;
            input.paletteQuery = L"ноч";
            input.palette = {{L"Ночная орбита", L"Трек · Луна", 1000},
                             {L"Сейчас играет", L"На весь экран", 6},
                             {L"Настройки", L"Раздел", 12}};
            input.paletteSelected = 0;
            render("design-palette.png");
            input.paletteOpen = false;
            // Кадры анимаций: заставка, раскрытие, закрытие, смена страницы, вылет обложки
            auto frame = [&](int steps, const std::function<void(ID2D1DeviceContext*)>& extra) {
                for (int step = 0; step < steps; ++step) {
                    auto* dc = renderer.BeginFrame();
                    if (!dc) {
                        ++failures;
                        break;
                    }
                    home.Paint(dc, input);
                    if (extra) extra(dc);
                    if (!renderer.EndFrame()) ++failures;
                    ++frames;
                    Sleep(16);
                }
            };
            ui::Intro intro;
            const D2D1_SIZE_F full{1200, 800};
            const struct {
                bool closing;
                double at;
                const char* name;
            } moments[] = {
                {false, .42, "anim-intro.png"}, {false, 1.0, "anim-reveal.png"}, {true, .26, "anim-outro.png"}};
            for (const auto& m : moments) {
                intro.Start(0, m.closing);
                frame(2, [&](ID2D1DeviceContext* dc) {
                    intro.Paint(dc, renderer.Factory(), renderer.DWrite(), full, m.at, input.colors.bg,
                                input.colors.accent, home.SharedPicture(dc, ui::Home::IntroBackdrop));
                });
                if (!Save(renderer, shots / m.name)) ++failures;
            }
            home.model.page = view::HomeModel::Home;
            frame(30, nullptr);
            home.Navigate(view::HomeModel::Library);
            frame(9, nullptr);
            if (!Save(renderer, shots / "anim-page.png")) ++failures;
            home.Navigate(view::HomeModel::Home);
            frame(30, nullptr);
            home.ToggleNowPlaying();
            frame(11, nullptr);
            if (!Save(renderer, shots / "anim-cover.png")) ++failures;
            // Обложка в полёте и след за ней
            home.ToggleNowPlaying();
            frame(40, nullptr);
            home.ToggleNowPlaying();
            frame(9, nullptr);
            if (!Save(renderer, shots / "anim-morph.png")) ++failures;
            frame(40, nullptr);
            home.ToggleNowPlaying();
            frame(40, nullptr);
            // Загрузка закончилась: трек летит к «Библиотеке», там вспыхивает «+1»
            {
                const auto savedCards = input.cards;
                home.model.page = view::HomeModel::Downloads;
                ui::Home::Card job{L"Луна - Ночная орбита", L"YouTube Music", {}, {}};
                job.job = PM_ROW_ACTIVE;
                input.cards = {job};
                frame(30, nullptr);
                input.cards[0].job = PM_ROW_DONE;
                frame(30, nullptr);
                if (!Save(renderer, shots / "anim-flyer.png")) ++failures;
                frame(34, nullptr);
                if (!Save(renderer, shots / "anim-flyer-done.png")) ++failures;
                input.cards = savedCards;
                home.model.page = view::HomeModel::Home;
                frame(10, nullptr);
            }
        }
        home.Shutdown();
        renderer.Shutdown();
        DestroyWindow(window);
    }
    CoUninitialize();
    std::printf("Settings: %d full/mini, animated, narrow, large-text and device-reset frames, %d failures\n", frames,
                failures);
    return failures ? 1 : 0;
}
