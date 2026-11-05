#pragma once
#include <compare>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include "ui/covers.h"
#include "ui/gradient_cache.h"
#include <d2d1_2.h>
#include <d2d1helper.h>
#include "ui/icons.h"
#include "view/home_layout.h"
#include "ui/renderer.h"
#include "ui/text.h"
#include "ui/home_colors.h"
#include "view/home_model.h"
#include "view/liquid.h"
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
        WorkspaceCommand,
        NowPlaying,   // открыть или свернуть «Сейчас играет»
        Slider,       // index - команда ползунка
        PaletteItem,  // index - строка палитры; -1 - мимо панели (закрыть), -2 - сама панель
        SeekLyric,    // index - строка текста
        Karaoke,      // включить или выключить караоке
        StoryNav  // «Итоги»: -1 назад, 1 дальше, 100+n период, 200 сохранить, 201 волна, 202 закрыть, 203 сначала, 204
                  // фон
    };
    struct Hit {
        view::Rect rect;
        Action action;
        int index = -1;
        std::wstring label = {};  // подпись для экранного диктора (у кнопок - их текст)
    };
    struct Control {
        std::wstring label;
        int command = 0;
        bool active = false;
        bool toggle = false;
        D2D1_COLOR_F background = D2D1::ColorF(0x181920);
        D2D1_COLOR_F accent = D2D1::ColorF(0xed647b);
        // Ползунок: value 0..1 (>= 0 - это ползунок), подпись значения справа
        float value = -1;
        std::wstring valueText = {};
    };
    struct Card {
        std::wstring title, subtitle, status;
        std::vector<Control> controls;
        float progress = -1;
        bool error = false;
        bool themeGallery = false;
        int job = -1;           // PM_ROW_* для строк загрузок: компактная строка с пластинкой; -1 - обычная карточка
        bool tools = false;     // строка о yt-dlp и ffmpeg
        int illustration = -1;  // Picture слева от текста (пустые страницы)
        bool swatches = false;  // цвета кружками в один ряд
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
        int undoCount = 0;
        // Палитра команд
        struct PaletteEntry {
            std::wstring title, hint;
            int id = 0;  // < 1000 - команда, >= 1000 - трек библиотеки
        };
        std::vector<PaletteEntry> palette;
        bool paletteOpen = false, paletteFocused = false;
        std::wstring paletteQuery;
        int paletteSelected = 0;
        unsigned paletteStart = 0, paletteEnd = 0;
        std::wstring settingsQuery;
        bool settingsSearchFocused = false;
        unsigned settingsStart = 0, settingsEnd = 0;
        bool previewMini = false, previewLarge = false, reducedMotion = false;
        bool canUndo = false;
        std::wstring notice, downloadText;
        bool downloadEnabled = false, downloadFocused = false;
        bool scanning = false;
        bool searchFocused = false;
        unsigned selectionStart = 0, selectionEnd = 0;
        unsigned downloadStart = 0, downloadEnd = 0;
        // Живой фон: двигается, только пока играет музыка и окно на переднем плане
        HomeColors colors;  // тема и обложка; App задаёт каждый кадр
        std::string skin;   // тема: у «Неона», «Инея», «Заката», «Минимала» свой баннер
        // «Сейчас играет»: текст песни (состояние как view::LyricsState) и спектр для волны
        std::vector<std::wstring> lyrics;
        std::vector<double> lyricTimes;
        int lyricsState = 0;
        std::vector<float> spectrum;
        bool ambient = false;
        float energy = 0;           // 0..1, сглаженный уровень низких частот
        float beat = 0;             // 1 в момент доли такта, дальше быстро гаснет
        bool karaoke = false;       // голос приглушён, текст крупно с заливкой строки
        bool highContrast = false;  // без цветных пятен и полупрозрачности
        int coverFrame = 0;         // рамка обложки: 0 - нет, 1..10 - assets/frames
        // «Итоги» историей: готовые строки и цифры периода
        struct StoryItem {
            std::wstring title, sub, detail;
            float share = 0;  // доля от первого места, для полосы
        };
        struct Story {
            std::wstring period, hours, playsLine;
            double seconds = 0;
            int activeDays = 0, topHour = -1, periodIndex = 0;
            float hourShare[24] = {};
            std::vector<StoryItem> artists, tracks;
            std::filesystem::path topTrackPath;
            int background = 0;  // фон карточки: 0 - ночь, 1 - пластинки
        } story;
        float bands[8] = {};     // 0..1, для эквалайзера у играющего трека
        bool bandsLive = false;  // полосы посчитаны по звуку (и при «меньше движения» - реже)
    };
    Home() = default;
    ~Home();
    view::HomeModel model;
    void Init(HWND hwnd, Renderer* renderer, TextSystem* text);
    void Shutdown();
    void Paint(ID2D1DeviceContext* dc, const Input& input);
    Hit At(view::Point p) const;
    bool NeedsFrames() const { return needsFrames_; }
    // Кадров не нужно, но к этому времени картинка изменится (мигающий курсор); 0 - ничего не ждём
    double NextWake() const { return nextWake_; }
    void Move(view::Point p) {
        mouse_ = p;
        if (drag_.hit != -1 && !drag_.active && std::hypot(p.x - drag_.start.x, p.y - drag_.start.y) > 6)
            drag_.active = true;
    }
    // Плавная прокрутка: колесо двигает цель, содержимое догоняет её; у краёв - мягкий отскок
    void Scroll(float delta) { scrollTarget_ = std::clamp(scrollTarget_ - delta * 120, -60.f, scrollMax_ + 60); }
    void ResetScroll() { scroll_ = scrollTarget_ = 0; }
    // Перетаскивание трека: из коллекции в очередь или внутри очереди.
    // Press - кнопка нажата; после сдвига на 6 DIP начинается перенос; Release говорит, что бросили.
    struct Drop {
        enum Kind { None, ToQueue, Reorder } kind = None;
        int index = -1;   // ToQueue: номер в Displayed(); Reorder: номер в плейлисте
        int before = -1;  // вставить перед этим номером плейлиста (-1 - в конец)
    };
    void Press(view::Point p);
    Drop Release(view::Point p);
    bool Dragging() const { return drag_.active; }
    bool NowPlayingOpen() const { return nowOpen_; }
    // Картинки оформления из assets: фоны «Итогов», иллюстрации пустых страниц, логотип
    enum Picture {
        RecapNight,
        RecapRecords,
        EmptyDownloads,
        EmptyPlugins,
        AppLogo,
        Placeholder1,
        Placeholder2,
        Placeholder3,
        Placeholder4,
        Placeholder5,
        Placeholder6,
        Placeholder7,
        Placeholder8,
        Placeholder9,
        Placeholder10,
        Placeholder11,
        Placeholder12,
        MixWave,
        MixFavorites,
        MixForgotten,
        MixFresh,
        MixMorning,
        MixDay,
        MixEvening,
        MixNight,
        EmptyFavorites,
        EmptyLibrary,
        EmptyQueue,
        EmptySearch,
        EmptyLyrics,
        EmptyOffline,
        EmptyError,
        EmptyInstrumental,
        BannerNeon,
        BannerFrost,
        BannerSunset,
        BannerMinimal,
        BannerGlass,
        BannerLibrary,
        BannerFavorites,
        BannerQueue,
        BannerSettings,
        RecapTotal,
        RecapArtist,
        RecapTrack,
        RecapTime,
        IntroBackdrop,
        KaraokeBackdrop,
        NowNoCover,
        kPictures
    };
    // Картинка для рисования в другом месте (заставка App); dc - тот же контекст рендерера
    ID2D1Bitmap1* SharedPicture(ID2D1DeviceContext* dc, Picture which) {
        dc_ = dc;
        return PictureBitmap(which);
    }
    // Есть ли картинка рамки style (1..10) в assets/frames
    bool HasFrame(int style) const;
    // «Итоги» историей и всплывающее сообщение главной
    void OpenStory();
    void CloseStory() { storyOpen_ = false; }
    bool StoryOpen() const { return storyOpen_; }
    void StoryStep(int delta);
    void ShowToast(const std::wstring& text);
    // Клавиатура: Tab / Shift+Tab ходят по элементам, Enter нажимает. Возвращает подпись элемента
    std::wstring MoveFocus(int direction);
    bool FocusedPoint(view::Point& center) const;
    void ClearFocus() { focusActive_ = false; }
    std::wstring TakeToast() {
        std::wstring t;
        t.swap(spokenToast_);
        return t;
    }
    // Карточка итогов во весь target (для сохранения картинкой); вызывать между BeginDraw и EndDraw
    void RenderStoryCard(ID2D1DeviceContext* dc, D2D1_SIZE_F size, const Input& in);
    // Ползунок под нажатой кнопкой мыши: команда и доля 0..1 по текущему положению курсора
    bool ActiveSlider(int& command, float& fraction) const;
    // Команда элемента настроек под курсором (для предпросмотра по наведению); -1 - ничего
    int HoveredCommand() const { return hoveredCommand_; }
    // Поле поиска настроек (под ним App держит настоящий EDIT)
    view::Rect SettingsSearchRect() const { return settingsSearch_; }
    // Где поле палитры команд (App ставит туда EDIT)
    static view::Rect PaletteField(view::Size window);
    void ToggleNowPlaying();
    // Сердечки разлетаются из точки (добавили в любимое)
    void Burst(view::Point p) { bursts_.push_back({p.x, p.y, clock_}); }
    void InvalidateCatalog() { ++revision_; }
    static constexpr UINT kArtReady = WM_APP + 27;
    // Поле ссылки на странице загрузок - под баннером; App ставит туда настоящий EDIT
    static constexpr float kDownloadFieldTop = 164;
    const std::vector<view::HomeTrack>& Displayed() const { return displayed_; }
    float Fraction(view::Point p, Action a) const;
    void Navigate(int page) {
        model.page = (view::HomeModel::Page)page;
        scroll_ = scrollTarget_ = 0;
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
    void Art(const view::HomeTrack& t, view::Rect r, float radius = 12);
    void Glyph(view::Rect r, int kind, D2D1_COLOR_F color);
    void Symbol(view::Rect r, IconId id, D2D1_COLOR_F color);
    void Path(view::Rect r, std::string_view path, D2D1_COLOR_F color, bool fill = false);
    void Image(ID2D1Bitmap1* bitmap, view::Rect r, float radius = 12, float anchor = .5f, float anchorY = .5f,
               float zoom = 1);
    // Эффекты (home_fx.cpp)
    void Aurora(view::Rect area, const Input& in);
    void Glow(view::Rect r, D2D1_COLOR_F color, float radius, float spread, float strength = 1);
    void Equalizer(view::Rect r, const Input& in, D2D1_COLOR_F color);
    void Disc(view::Rect r, int state, bool tools);
    void Sweep(view::Rect r, float radius, float progress);
    void JobCard(view::Rect r, const Card& c, size_t index);
    std::vector<view::HomeTrack> mixPlays_, mixOld_, mixFavorites4_, mixFresh_;
    uint64_t mixRevision_ = 0;
    size_t mixCount_ = 0, mixFavorites_ = 0;
    // Подборка: коллаж 2x2 из обложек её треков в оттенке акцента, название и подпись
    void MixCard(view::Rect r, const std::wstring& title, const std::wstring& subtitle,
                 const std::vector<view::HomeTrack>& tracks, Action action, int index, int key, Picture cover);
    // Вход страницы: 0..1 для элемента с номером order (лесенка), с кривой ease-out
    float Enter(int order);
    void PushFade(float opacity);
    void PopFade();
    double clock_ = 0, pageEnteredAt_ = -10;
    int lastPage_ = -1;
    float scrollTarget_ = 0, scrollMax_ = 0;
    void StepScroll();
    void ClampScroll(float maximum);
    // Смена трека: нижняя панель въезжает, плитка трека вспыхивает
    std::wstring trackKey_;
    double trackChangedAt_ = -10;
    struct Particle {
        float x, y;
        double t;
    };
    std::vector<Particle> bursts_;  // сердечки
    // Скачанный трек летит к «Библиотеке»: своя обложка, если трек уже нашёлся в коллекции
    struct Flyer {
        float x, y;
        double t;
        view::HomeTrack track;
        bool known = false;
    };
    std::vector<Flyer> flyers_;
    std::map<std::wstring, int> jobStates_;  // прошлое состояние строк загрузок (для «+1»)
    double libraryPulseAt_ = -10;
    void PaintParticles();
    struct DragState {
        int hit = -1;  // index из Hit::PlayTrack: >= 0 коллекция, <= -2 очередь
        view::Point start;
        bool active = false;
        view::HomeTrack track;
    } drag_;
    view::Rect queueRect_;
    std::vector<view::HomeTrack> queueVisible_;  // строки «Далее» последнего кадра
    int queueStart_ = 0, queueShown_ = 0;
    float queueTop_ = 150;
    int DropSlot() const;  // перед каким номером плейлиста встанет брошенный трек
    void PaintDrag(const Input& in);
    bool nowOpen_ = false;
    int sliderCommand_ = -1;
    double paletteOpenedAt_ = -10;
    bool storyOpen_ = false;
    int storySlide_ = 0;
    double storySlideAt_ = 0, toastAt_ = -10;
    std::wstring toast_;
    void PaintStory(const Input& in);
    void PaintStoryCard(view::Rect r, const Input::Story& story);
    void PaintToast();
    // Фокус клавиатуры: какой элемент (действие и номер) и виден ли сейчас контур
    bool focusActive_ = false;
    Action focusAction_ = None;
    int focusIndex_ = -1;
    std::wstring spokenToast_;
    bool Focusable(const Hit& h) const;
    std::wstring Describe(const Hit& h) const;
    void PaintFocus();
    // Жидкий индикатор выбора (view/liquid.h): key - группа кнопок, target - где сейчас активная
    // кнопка, slot - какая она по счёту. Перетекание запускает только смена slot
    std::map<int, view::Liquid> liquids_;
    uint64_t paintFrame_ = 0;
    view::Rect LiquidRect(int key, view::Rect target, int slot);
    void LiquidFill(view::Rect r, D2D1_COLOR_F color, float radius, float glow);
    // Живые мелочи (home_magic.cpp)
    struct Ripple {
        view::Point p;
        view::Rect rect;
        float radius;
        double at;
    };
    std::vector<Ripple> ripples_;
    std::map<int, view::Point> magnets_;
    // Картинки оформления: фоны «Итогов», иллюстрации пустых страниц, логотип
    ComPtr<IWICBitmapSource> pictureSources_[kPictures];
    ComPtr<ID2D1Bitmap1> pictureBitmaps_[kPictures];
    ID2D1Bitmap1* PictureBitmap(Picture which);
    // Картинки грузятся при первом показе и сразу ужимаются до нужного размера: все разом в полный
    // размер заняли бы сотни мегабайт
    bool pictureTried_[kPictures] = {};
    bool pictureUploaded_[kPictures] = {};
    uint64_t pictureGeneration_ = 0;
    std::filesystem::path assetsDir_;
    ComPtr<IWICBitmapSource> LoadAsset(const std::filesystem::path& file, UINT maxSide) const;
    static constexpr int kFrames = 10;  // лунная, неон, винил, закат, иней, кассета, золото, полароид, плёнка, сияние
    std::filesystem::path frameFiles_[kFrames];               // есть файл - есть вариант рамки
    mutable ComPtr<IWICBitmapSource> frameSources_[kFrames];  // прочитана, но ещё не на видеокарте
    mutable bool frameMeasured_[kFrames] = {};
    ComPtr<ID2D1Bitmap1> frameBitmaps_[kFrames];
    // Проём рамки - доли картинки: левый, верхний, правый, нижний край прозрачного окна
    struct FrameWindow {
        float l = .1f, t = .1f, r = .9f, b = .9f;
    };
    mutable FrameWindow frameWindow_[kFrames];
    ComPtr<IWICBitmapSource> DecodeFrame(int index) const;
    const FrameWindow& WindowOfFrame(int index) const;
    // Куда рисовать рамку, чтобы её окно легло на обложку art; window - само окно на экране
    D2D1_RECT_F FrameDest(view::Rect art, int style, view::Rect* window = nullptr) const;
    void AddRipple(view::Point p, view::Rect r, float radius);
    void PaintRipples();
    void Spotlight(view::Rect r, float radius, float strength);
    view::Point Magnet(int key, view::Rect r, float reach, float pull);
    view::Point Parallax(int key, view::Rect r);
    std::wstring Scramble(const std::wstring& text, double since);
    D2D1_MATRIX_3X2_F NavIconMotion(int index, view::Rect icon, float over);
    // live - кадр с музыкой: рамка оживает (блик, подъём на долю, у каждой рамки свой характер)
    void CoverFrame(view::Rect art, int style, const Input* live = nullptr);
    // Настроение обложки: цвет свечения, фона и волны в «Сейчас играет»
    std::map<uint64_t, D2D1_COLOR_F> coverMood_;
    D2D1_COLOR_F placeholderMood_[12] = {};
    bool placeholderMoodReady_[12] = {};
    D2D1_COLOR_F moodNow_{};
    double moodPaintedAt_ = -10;
    bool auroraMood_ = false;
    D2D1_COLOR_F CoverMood(const Input& in, bool hasCover);
    void PaintNextUp(const Input& in, view::Rect card, float fade);
    void PaintStage(view::Rect area, const Input& in, float stage);
    // Зерно плёнки по всему кадру и засветка при смене вида
    ComPtr<ID2D1BitmapBrush1> grain_;
    void PaintFilm(const Input& in, float w, float bottom);
    void PageBanner(view::Rect r, ID2D1Bitmap1* picture, int key, float radius = 18);
    void LiquidLabel(const std::wstring& text, view::Rect r, view::Rect liquid, D2D1_COLOR_F normal,
                     D2D1_COLOR_F onLiquid, float size, float weight);
    void PaintPalette(const Input& in);
    // Переход страниц: кадр рисуется в один из двух буферов; при смене страницы прошлый кадр
    // уезжает и гаснет поверх новой страницы
    ComPtr<ID2D1Bitmap1> frames_[2];
    ComPtr<ID2D1Image> screen_;
    int frameIndex_ = 0, previousFrame_ = -1, pageDirection_ = 1;
    double pageSwitchedAt_ = -10;
    bool BeginOffscreen(ID2D1DeviceContext* dc);
    void ComposeFrame(ID2D1DeviceContext* dc, view::Size size, float sidebar, float bottom);
    // «Сейчас играет»: обложка летит из нижней панели и рисуется поверх неё
    view::Rect morphArt_, morphGhost_[2];
    float morphGlow_ = 0;
    view::Rect sliderRect_;
    int hoveredCommand_ = -1;
    view::Rect settingsSearch_;
    void SliderControl(view::Rect r, const Control& control);
    // Кнопка без заливки, с тонкой обводкой: второстепенные действия
    void GhostButton(view::Rect r, const std::wstring& label, int command);
    void CenterText(const std::wstring& s, view::Rect r, float size, D2D1_COLOR_F color, float weight = 400);
    // Края прокручиваемого списка растворяются, а не режут строки; вложенность не поддерживается
    void PushEdgeFade(view::Rect area, float top, float bottom);
    void PopEdgeFade();
    bool edgeClip_ = false;  // PushEdgeFade обошёлся клипом без маски
    void Field(view::Rect field, const std::wstring& text, bool focused, unsigned start, unsigned end,
               const std::wstring& placeholder, float size = 13);
    double nowToggledAt_ = -10;
    float lyricPos_ = 0;
    ComPtr<ID2D1Effect> blur_;
    // Готовое размытие: для какой обложки и какого окна посчитано
    ComPtr<ID2D1Bitmap1> blurred_;
    ComPtr<ID2D1Bitmap1> blurOf_;  // ссылка держит картинку: новая обложка не получит её адрес
    float blurW_ = 0, blurH_ = 0;
    float NowPlayingAmount();
    void BlurredCover(ID2D1Bitmap1* bitmap, view::Rect area, float opacity);
    void PaintNowPlaying(const Input& in, float amount, float w, float bottom);
    // Баннеры перекрашиваются под акцент темы: поворот оттенка
    ComPtr<ID2D1Effect> hue_;
    void TintedImage(ID2D1Bitmap1* bitmap, view::Rect r, float radius, float anchor, float anchorY, float zoom);
    void IconButton(view::Rect r, IconId icon, Action action, bool active = false);
    IconSet icons_;
    std::map<std::string, ComPtr<ID2D1PathGeometry>, std::less<>> paths_;
    // Готовые сетки значков: у пары «контур и размер» она одна, пока живо устройство
    struct MeshKey {
        ID2D1PathGeometry* geometry;
        float w, h;
        bool fill;
        auto operator<=>(const MeshKey&) const = default;
    };
    std::map<MeshKey, ComPtr<ID2D1GeometryRealization>> pathMeshes_;
    ComPtr<ID2D1DeviceContext1> dc1_;
    ComPtr<ID2D1Bitmap1> heroBitmap_, downloadsBitmap_;
    ID2D1Bitmap1* HeroBitmap();
    ID2D1Bitmap1* DownloadsBitmap();
    view::HomeTrack currentTrack_;  // трек в нижней панели
    struct Artwork {
        std::wstring key;
        std::vector<uint8_t> bytes;
        std::string title;
    };
    Renderer* renderer_ = nullptr;
    TextSystem* text_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1BitmapBrush1> imageBrush_;
    GradientCache gradients_;
    uint64_t generation_ = 0;
    CoverCache art_;
    std::vector<Hit> hits_;
    std::vector<view::HomeTrack> displayed_;
    // Нормализованный ключ пути: на каждом кадре он нужен для всех видимых плиток
    std::unordered_map<std::wstring, std::wstring> pathKeys_;
    const std::wstring& PathKey(const std::filesystem::path& path);
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
    double nextWake_ = 0;
    double lastPaint_ = 0;
    float frameDelta_ = .016f;
    std::map<int, float> hover_;
    bool reducedMotion_ = false;
};
}  // namespace ui
