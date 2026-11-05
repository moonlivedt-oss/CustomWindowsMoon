#pragma once
// Приложение: окно со своей рамкой, цикл кадров, ввод, меню, медиаклавиши, сохранение.
// Связывает плеер (pm::Player), темы (theme::ThemeLibrary), логику интерфейса
// (view::PlayerView), отрисовку (Painter), библиотеку музыки и плагины.
// Всё живёт в потоке UI; аудиопоток, сканер библиотеки и наблюдатель за папками только
// будят окно сообщениями (kWakeMessage, kLibraryMessage, kWatchMessage).

#include <windows.h>

#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "core/fft.h"
#include "core/history.h"
#include "core/library.h"
#include "core/lyrics.h"
#include "core/player.h"
#include "core/plugins.h"
#include "theme/theme.h"
#include "ui/covers.h"
#include "ui/folder_watch.h"
#include "ui/icons.h"
#include "ui/home.h"
#include "ui/painter.h"
#include "ui/renderer.h"
#include "ui/intro.h"
#include "ui/narrator.h"
#include "core/karaoke.h"
#include "ui/smtc.h"
#include "ui/settings_preview.h"
#include <optional>
#include "ui/taskbar.h"
#include "ui/text.h"
#include "view/player_view.h"

namespace ui {

class App final : public view::IViewHost {
public:
    int Run(HINSTANCE instance, int show);

    // ----- view::IViewHost -----
    void TogglePlay() override;
    void Next() override;
    void Prev() override;
    void Seek(double seconds) override;
    void SetVolume(float volume) override;
    void ToggleMute() override;
    void CycleRepeat() override;
    void ToggleShuffle() override;
    void PlayIndex(int index) override;
    void ResizeWindow(view::Size dip) override;
    void MinimizeWindow() override;
    void CloseWindow() override;
    void ShowMenu(view::Point at) override;
    void ShowRowMenu(int row, view::Point at) override;
    void TogglePlugin(int index) override;
    void OpenPluginsFolder() override;
    void PageAction(int page, int rowId, int actionId) override;
    void OpenFiles(bool folder) override;
    float MeasureText(view::TextStyle style, const std::string& text) override;
    float MeasureWrapped(view::TextStyle style, const std::string& text, float width) override;

private:
    void RefreshHome();
    void OpenWorkspace(view::HomeModel::Page page);
    void BuildWorkspace();
    SettingsPreview settingsPreview_;
    bool previewMini_ = false, previewLarge_ = false;
    int settingsSection_ = 0;
    struct SettingsSnapshot {
        theme::Theme theme;
        bool user, reduce, normalize, remaining, top, autoScan, hideDuplicates;
        double crossfade = 0;
        int windowFx = 0;
    };
    // История изменений настроек: «Отменить» снимает последнее, «Все N» - возвращает начало
    std::vector<SettingsSnapshot> settingsHistory_;
    SettingsSnapshot CaptureSettings() const;
    void RestoreSettings(const SettingsSnapshot& snapshot);
    void RememberSettings();
    void BuildSettings();
    void BuildSettingsSection(int section);
    bool SettingsCommand(int command);
    void SliderCommand(int command, float fraction, bool begin);
    bool sliderDragging_ = false;
    int previewHover_ = -1;
    void UpdateSettingsHoverPreview();
    HWND settingsEdit_ = nullptr;
    // Клавиши: действие -> сочетание (порядок - в app_shortcuts.cpp), назначение по щелчку
    struct ShortcutKey {
        UINT key = 0;
        bool ctrl = false;
    };
    std::vector<ShortcutKey> shortcuts_;
    int capturingShortcut_ = -1;
    std::wstring KeyName(UINT key, bool ctrl) const;
    void ResetShortcuts();
    void BuildShortcutCards();
    bool CaptureShortcut(UINT key);
    bool RunShortcut(UINT key, bool fromEdit);
    void RunAction(int action);
    // Палитра команд
    HWND paletteEdit_ = nullptr;
    bool paletteOpen_ = false;
    int paletteSelected_ = 0;
    std::vector<std::string> paletteArtists_;  // исполнители для «Волна · имя» в палитре
    void OpenPalette();
    void ClosePalette();
    void BuildPalette();
    void RunPalette(int item);
    void WorkspaceCommand(int command);
    void EnterMini();
    std::string submittedDownload_;
    // Ссылка из буфера подставляется в поле загрузки один раз на каждое новое копирование
    DWORD prefillClipboard_ = 0;
    pm::BeatDetector beat_;  // доли такта для пульса главной
    void PrefillDownload();
    bool EnableDownloaderPlugin();
    std::vector<std::filesystem::path> pendingPluginFolders_;
    HWND downloadEdit_ = nullptr;
    bool reduceMotion_ = false;
    std::wstring workspaceNotice_;
    struct WorkspaceBinding {
        size_t page;
        int row, action;
    };
    std::vector<WorkspaceBinding> workspaceBindings_;
    void HomeClick(view::Point point);
    void HomeDrop(const Home::Drop& drop);
    // Меню трека по правому клику: index - как в Home::Hit (>= 0 коллекция, <= -2 очередь)
    void TrackMenu(int index, view::Point at);
    int EnsureInPlaylist(const view::HomeTrack& track);  // номер в плейлисте, при нужде дописывает
    void ToggleHome();
    void LayoutHomeSearch();
    void SetHomeWindowStyle();
    void LoadHomePreferences();
    void SaveHomePreferences();
    void ResetTickTimer();
    Home home_;
    Home::Input homeInput_;
    HWND homeSearch_ = nullptr;
    HFONT homeSearchFont_ = nullptr;
    static LRESULT CALLBACK SearchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    WNDPROC homeSearchProc_ = nullptr;
    bool homeMode_ = true;
    view::Size homeSize_{1200, 800};
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // Уже запущенный экземпляр: передать ему файлы из командной строки. true - он есть, выходим.
    static bool ForwardToRunningInstance();
    void HandleCopyData(const COPYDATASTRUCT* cds);

    bool CreateMainWindow(HINSTANCE instance);
    void ResolvePaths();
    void LoadSavedState();
    void SaveCurrentState();
    // Что-то поменялось: сохранить через 2 секунды (несколько изменений подряд - одна запись)
    void MarkDirty();
    void MessageLoop();
    void RenderFrame();
    double Now() const;
    view::Point ToDip(LPARAM lp) const;
    view::Size ClientDip() const;
    int ToPx(float dip) const;

    void ApplyTheme(const theme::Theme& t, bool animate);
    void ApplyBackdrop();
    void ApplyTopmost();
    void SnapToEdges(RECT* r) const;
    void ReadSystemReduce();
    static bool SystemPrefersLight();
    void OnTrackChanged(int index, int direction);
    void RebuildRows();
    void ShowContextMenu(POINT screen);
    void ImportTheme();
    void ExportTheme();
    void LoadDropped(HDROP drop);
    // append - добавить в конец; иначе заменить плейлист и начать с первого трека
    void LoadPaths(const std::vector<std::filesystem::path>& paths, bool append = false);
    void RegisterMediaKeys();
    void HandleMediaCommand(int command);
    void Invalidate() { dirty_ = true; }

    // ----- библиотека -----
    void InitLibrary(bool firstRun);
    void StartLibraryScan();
    void OnLibraryScanned();
    void RestartWatcher();
    void LoadLibraryPlaylist(bool play);
    void AddLibraryFolder();
    // Запомнить папки, из которых пользователь что-то открыл
    void LearnFolders(const std::vector<std::filesystem::path>& paths);

    // ----- тексты песен -----
    // Вкладка «Текст» на экране: найти текст текущего трека (один раз на трек)
    void RequestLyrics();
    void OnLyricsReady();

    // ----- история, «Итоги», «Волна» (ui/app_stats.cpp) -----
    void EnsureHistory();
    // Трек закончился или сменился: дописать прослушивание в историю
    void FinishListen();
    void BeginListen();
    void ShowStats();
    view::PanelPage BuildStatsPage();
    void StatsAction(int actionId);
    void StartWave();
    // Волна подходит к концу очереди - дописать ещё
    void ExtendWave();

    // ----- плагины -----
    void InitPlugins(const std::set<std::string>& enabled);
    std::filesystem::path UserPluginsDir() const;
    void RebuildPluginRows();
    void NotifyPluginsTrack();
    // Страницы плагинов -> view (если изменились)
    void SyncPages();
    bool InPlaylist(const std::filesystem::path& p) const;
    // host->addFiles: дописать без повторов, play - сразу играть первый
    void AddFilesFromPlugin(const std::vector<std::string>& paths, bool play);

    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    LARGE_INTEGER freq_{};
    UINT taskbarCreatedMsg_ = 0;

    pm::Player player_;
    theme::ThemeLibrary themes_;
    theme::Theme theme_;
    bool userTheme_ = false;  // своя тема (из импорта) - сохраняется документом целиком
    view::PlayerView view_;

    Renderer renderer_;
    TextSystem text_;
    IconSet icons_;
    CoverCache covers_;
    Painter painter_;
    Taskbar taskbar_;
    Smtc smtc_;

    pm::SpectrumAnalyzer analyzer_{2048};
    std::vector<float> pcm_, spectrum_;
    // Спектр считается, только если пришли новые сэмплы: кадров в секунду больше, чем порций звука
    struct SpectrumRun {
        std::vector<float> bars;
        uint64_t written = ~0ull;
    };
    bool UpdateSpectrum(SpectrumRun& run, int bars);
    SpectrumRun playerSpectrum_, homeSpectrum_, nowSpectrum_;
    uint64_t homeTrackSerial_ = ~0ull;
    const void* homeLyricsShown_ = nullptr;
    size_t homeLyricsCount_ = 0;
    bool lazyTimer_ = false;
    // Работа в фоне (app_background.cpp)
    static constexpr UINT kTrayMessage = WM_APP + 7;
    void AddTrayIcon();
    void RemoveTrayIcon();
    void HideToBackground();
    void FinishHide();
    void OnShownFromBackground();
    void ShowFromBackground();
    void ShowTrayMenu();
    bool HandleBackgroundMessage(UINT msg, WPARAM wp, LPARAM lp);
    void StartCloseAnimation();
    void QuitApp();
    UINT trayCreatedMsg_ = 0;
    bool hidden_ = false, hideWhenClosed_ = false, showWhenHidden_ = false;
    bool backgroundHintShown_ = false;
    // Пока пользователь ничего не делает, постоянные анимации идут примерно 60 кадров в секунду; см. PlanFramePace
    void ReadRefreshRate();
    void PlanFramePace(double now);
    UINT refreshHz_ = 60;
    UINT framePace_ = 1;       // обновлений экрана на следующий кадр
    UINT lastFramePace_ = 1;   // обновлений экрана на кадр, который рисуется сейчас
    double lastActive_ = -10;  // последний ввод или смена трека
    bool graphicsRetry_ = false;
    bool systemHighContrast_ = false;
    std::vector<PlaylistRow> rows_;
    std::wstring countText_;

    pm::Library library_;
    pm::LibraryScanner scanner_;
    FolderWatcher watcher_;
    bool rescanPending_ = false;
    bool playlistFromLibrary_ = false;

    pm::LyricsService lyrics_;
    uint64_t lyricsSerial_ = 0;  // трек, для которого уже искали текст

    pm::ListenTracker listen_;
    std::filesystem::path historyPath_;
    std::vector<pm::PlayRecord> history_;
    bool historyLoaded_ = false;
    bool statsOpen_ = false;  // страница «Итоги» в панели
    pm::StatsPeriod statsPeriod_ = pm::StatsPeriod::Month;
    uint64_t statsRevision_ = 0, statsShown_ = ~0ull;
    bool waveMode_ = false;  // плейлист - «Волна», дописывается сама
    bool waveExtend_ = false;

    pm::PluginManager plugins_;
    std::vector<PluginRow> pluginRows_;
    std::wstring pluginCountText_;
    bool pluginsRefreshPending_ = false;
    uint64_t pagesRevision_ = ~0ull;  // открыта папка плагинов - перечитать при возврате в окно

    std::filesystem::path exeDir_, assetsDir_, statePath_, libraryPath_;
    bool systemReduce_ = false;
    bool win11Backdrop_ = false;  // сборка Windows умеет системный акрил (22621+)
    bool acrylicOn_ = false;      // акрил сейчас включён
    bool miniOnTop_ = true;
    // Появление окна при запуске и смене вида. windowFx_: 0 - своё для каждого вида, дальше варианты
    // из WindowFxNames(), последний - без эффекта. Хранится в home.json.
    enum class WindowForm { Unknown, Compact, Home, Mini };
    // Заставка при открытии и закрытии; introEnabled_ - настройка «Заставка при запуске» (home.json)
    Intro intro_;
    Narrator narrator_;
    bool highContrast_ = false;
    // Учитывать ли «Эффекты анимации» Windows: выключены там - и плеер без движения
    bool followSystemMotion_ = true;
    int coverFrame_ = 0;             // рамка обложки в «Сейчас играет» (assets/frames)
    int storyBackground_ = 0;        // фон карточки «Итогов»: 0 - ночь, 1 - пластинки
    bool windowsAnimations_ = true;  // как сейчас в Windows (для подсказки в настройках)  // настройка «Высокий
                                     // контраст»; системный режим Windows включает его сам
    bool HighContrast() const;
    bool introEnabled_ = true, closing_ = false, modeSwitchPending_ = false;
    bool MotionAllowed() const;
    // Главная <-> плеер с уходом старого вида перед входом нового
    void AnimatedToggleHome();
    // «Волна» под настроение (WaveMoodFirst + n); artist - для волны одного исполнителя
    void StartMoodWave(int mood, const std::string& artist = {});
    // «Итоги» историей на главной
    void OpenStory(pm::StatsPeriod period);
    void StoryCommand(int index);
    void SaveStoryCard();
    WindowForm windowForm_ = WindowForm::Unknown;
    int windowFx_ = 0;
    void PlayWindowEntrance(WindowForm from, WindowForm to, Entrance preferred = Entrance::None);
    bool topmostApplied_ = false;
    bool visible_ = true;
    bool dirty_ = true;
    bool rendering_ = false;
    bool started_ = false;  // после запуска смена трека анимируется
    bool trackingMouse_ = false;
    bool tickTimer_ = false;
    bool saveTick_ = false;
    int lastPlaying_ = -1;       // для события «пуск/пауза» плагинам
    double lastCopyData_ = -10;  // время прошлой передачи файлов от второго экземпляра
    std::atomic<bool> wakePosted_{false};
};

}  // namespace ui
