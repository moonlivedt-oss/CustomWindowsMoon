// «Итоги» историей на главной: цифры периода в строки для слайдов, листание и карточка в PNG.
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <ctime>

#include "core/text.h"
#include "ui/app.h"

namespace ui {
namespace {
int LocalOffset() {
    const time_t t = time(nullptr);
    tm local{};
    localtime_s(&local, &t);
    return static_cast<int>(_mkgmtime(&local) - t);
}
std::wstring Duration(double seconds) {
    const long m = std::lround(seconds / 60);
    if (m == 0) return L"меньше минуты";
    if (m < 60) return std::to_wstring(m) + L" мин";
    return std::to_wstring(m / 60) + L" ч " + std::to_wstring(m % 60) + L" мин";
}
std::wstring Count(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many) {
    const int d = n % 10, h = n % 100;
    return std::to_wstring(n) + L" " +
           ((d == 1 && h != 11)                        ? one
            : (d >= 2 && d <= 4 && (h < 12 || h > 14)) ? few
                                                       : many);
}
const wchar_t* const kMonthNames[] = {L"Январь", L"Февраль", L"Март",     L"Апрель",  L"Май",    L"Июнь",
                                      L"Июль",   L"Август",  L"Сентябрь", L"Октябрь", L"Ноябрь", L"Декабрь"};
}  // namespace

void App::OpenStory(pm::StatsPeriod period) {
    EnsureHistory();
    statsPeriod_ = period;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int offset = LocalOffset();
    const pm::ListenSummary s = pm::Summarize(history_, pm::PeriodStart(now, offset, period), now + 1, offset, 5);
    Home::Input::Story story;
    const time_t tt = static_cast<time_t>(now);
    tm local{};
    localtime_s(&local, &tt);
    const auto wide = [](const std::string& v) { return ::text::Utf8ToWide(v); };
    story.period = period == pm::StatsPeriod::Month
                       ? std::wstring(kMonthNames[local.tm_mon]) + L" " + std::to_wstring(local.tm_year + 1900)
                   : period == pm::StatsPeriod::Year ? std::to_wstring(local.tm_year + 1900) + L" год"
                                                     : L"Всё время";
    story.periodIndex = period == pm::StatsPeriod::Month ? 0 : period == pm::StatsPeriod::Year ? 1 : 2;
    story.seconds = s.seconds;
    story.hours = Duration(s.seconds);
    story.playsLine = Count(s.plays, L"прослушивание", L"прослушивания", L"прослушиваний") + L" · " +
                      Count(s.tracks, L"трек", L"трека", L"треков") + L" · " +
                      Count(s.artists, L"исполнитель", L"исполнителя", L"исполнителей");
    story.activeDays = s.activeDays;
    story.topHour = s.topHour;
    double peak = 0;
    for (double v : s.byHour) peak = std::max(peak, v);
    for (int h = 0; h < 24; ++h) story.hourShare[h] = peak > 0 ? (float)(s.byHour[(size_t)h] / peak) : 0.f;
    const double first = s.topArtists.empty() ? 1 : std::max(1.0, s.topArtists[0].seconds);
    for (const auto& a : s.topArtists)
        story.artists.push_back(
            {wide(a.name), wide(a.sub),
             Duration(a.seconds) + L" · " + Count(a.plays, L"прослушивание", L"прослушивания", L"прослушиваний"),
             (float)(a.seconds / first)});
    for (const auto& tr : s.topTracks)
        story.tracks.push_back(
            {wide(tr.name), wide(tr.sub), Count(tr.plays, L"прослушивание", L"прослушивания", L"прослушиваний"), 0});
    // Обложка трека периода - из библиотеки по названию
    if (!story.tracks.empty())
        for (const auto& t : home_.model.tracks)
            if (t.title == story.tracks[0].title) {
                story.topTrackPath = t.path;
                break;
            }
    homeInput_.story = std::move(story);
    if (!home_.StoryOpen()) home_.OpenStory();
    Invalidate();
}

void App::StoryCommand(int index) {
    switch (index) {
        case -1:
            home_.StoryStep(-1);
            break;
        case 1:
            home_.StoryStep(1);
            break;
        case 100:
        case 101:
        case 102:
            OpenStory(index == 100   ? pm::StatsPeriod::Month
                      : index == 101 ? pm::StatsPeriod::Year
                                     : pm::StatsPeriod::All);
            home_.OpenStory();
            break;
        case 200:
            SaveStoryCard();
            break;
        case 201:
            home_.CloseStory();
            StartWave();
            break;
        case 202:
            home_.CloseStory();
            break;
        case 203:
            home_.OpenStory();
            break;
        case 204:
            storyBackground_ = 1 - storyBackground_;
            homeInput_.story.background = storyBackground_;
            SaveHomePreferences();
            break;
        default:
            break;
    }
    Invalidate();
}

// Карточка итогов в PNG 1080x1440 в «Изображения\PlayerMoon»; Проводник открывается с выделенным файлом
void App::SaveStoryCard() {
    auto* dc = renderer_.Context();
    auto* wic = renderer_.Wic();
    if (!dc || !wic) return;
    const D2D1_SIZE_U pixels{1080, 1440};
    const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap1> canvas, cpu;
    if (FAILED(dc->CreateBitmap(pixels, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96),
                                &canvas)) ||
        FAILED(dc->CreateBitmap(
            pixels, nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96),
            &cpu)))
        return;
    ComPtr<ID2D1Image> previous;
    float dpiX = 96, dpiY = 96;
    dc->GetTarget(&previous);
    dc->GetDpi(&dpiX, &dpiY);
    dc->SetTarget(canvas.Get());
    dc->SetDpi(96, 96);
    dc->BeginDraw();
    dc->SetTransform(D2D1::IdentityMatrix());
    home_.RenderStoryCard(dc, {1080, 1440}, homeInput_);
    const HRESULT drawn = dc->EndDraw();
    dc->SetTarget(previous.Get());
    dc->SetDpi(dpiX, dpiY);
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(drawn) || FAILED(cpu->CopyFromBitmap(nullptr, canvas.Get(), nullptr)) ||
        FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
        home_.ShowToast(L"Не получилось сохранить картинку");
        return;
    }
    ComPtr<IWICBitmap> bitmap;
    const HRESULT copied =
        wic->CreateBitmapFromMemory(pixels.width, pixels.height, GUID_WICPixelFormat32bppPBGRA, mapped.pitch,
                                    mapped.pitch * pixels.height, mapped.bits, &bitmap);
    cpu->Unmap();
    PWSTR pictures = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)))
        dir = std::filesystem::path(pictures) / L"PlayerMoon";
    CoTaskMemFree(pictures);
    std::error_code ec;
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    const auto file = dir / (L"Итоги " + homeInput_.story.period + L".png");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICFormatConverter> converter;
    WICPixelFormatGUID out = GUID_WICPixelFormat32bppBGRA;
    const bool ok = SUCCEEDED(copied) && !dir.empty() && SUCCEEDED(wic->CreateStream(&stream)) &&
                    SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
                    SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                    SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                    SUCCEEDED(frame->SetSize(pixels.width, pixels.height)) && SUCCEEDED(frame->SetPixelFormat(&out)) &&
                    SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                    SUCCEEDED(converter->Initialize(bitmap.Get(), out, WICBitmapDitherTypeNone, nullptr, 0,
                                                    WICBitmapPaletteTypeCustom)) &&
                    SUCCEEDED(frame->WriteSource(converter.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
                    SUCCEEDED(encoder->Commit());
    home_.ShowToast(ok ? L"Сохранено в «Изображения\\PlayerMoon»" : L"Не получилось сохранить картинку");
    if (ok) {
        const std::wstring arg = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
}
}  // namespace ui
