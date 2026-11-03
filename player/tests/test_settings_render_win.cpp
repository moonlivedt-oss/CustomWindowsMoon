#include "ui/home.h"
#include "ui/settings_preview.h"
#include "ui/workspace.h"
#include "core/text.h"
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
        const char* variants[] = {"moon", "frost", "moon", "moon", "neon"};
        for (int variant = 0; variant < 5; ++variant) {
            const UINT width = variant == 2 ? 900 : 1200, height = variant == 2 ? 640 : 800;
            SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
            renderer.Resize(width, height);
            if (variant == 2) renderer.ResetDevice();
            auto theme = themes.Make(variants[variant]);
            if (variant == 3) {
                theme.settings.textScale = 1.5;
                theme.settings.coverShape = "circle";
            }
            preview.Apply(theme, themes.Layout("classic"), variant != 4, variant * 10.);
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
            if (!Save(renderer, directory / ("settings-preview-" + std::to_string(variant) + ".png"))) ++failures;
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
