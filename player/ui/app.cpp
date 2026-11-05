#include "ui/app.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "core/json.h"
#include "core/state.h"
#include "core/text.h"
#include "motion/easing.h"
#include "theme/assets.h"
#include "theme/palette.h"
#include "ui/http.h"
#include "ui/workspace.h"

namespace ui {

namespace {

constexpr wchar_t kClassName[] = L"PlayerMoonWindow";
constexpr wchar_t kInstanceMutex[] = L"Local\\PlayerMoon.Instance";
constexpr UINT_PTR kMoveFrame = 20;
constexpr UINT_PTR kGraphicsRetry = 19;       // bounded retry after a temporary graphics failure
constexpr UINT kWakeMessage = WM_APP + 1;     // аудиопоток: есть события для PumpEvents
constexpr UINT kLibraryMessage = WM_APP + 2;  // сканер библиотеки закончил
constexpr UINT kWatchMessage = WM_APP + 3;    // в папках библиотеки что-то поменялось
constexpr UINT kPluginMessage = WM_APP + 4;   // плагин передал работу в поток окна (host->post)
constexpr UINT kSmtcMessage = WM_APP + 5;     // кнопка или перемотка из системной медиапанели
constexpr UINT kLyricsMessage = WM_APP + 6;   // найден (или не найден) текст песни
constexpr ULONG_PTR kCopyDataPaths = 0x504D;  // WM_COPYDATA: пути от второго экземпляра
constexpr UINT_PTR kTickTimer = 1;            // раз в секунду: время в мини-режиме и на панели задач
constexpr UINT_PTR kSaveTimer = 2;            // отложенное сохранение после изменений
constexpr UINT_PTR kSaveTick = 3;             // во время игры - позиция раз в 30 секунд
constexpr UINT_PTR kLazyTimer = 4;
constexpr UINT_PTR kCloseFade = 21;  // закрытие: заставка сжалась - окно гаснет
constexpr UINT_PTR kCloseDone = 22;  // закрытие: окно погасло - уничтожить
constexpr UINT_PTR kModeSwitch = 23;  // смена вида: старый ушёл - показать новый            // подсказки и уведомления:
                                      // разовый кадр к нужному времени

// Атрибуты DWM числами: в заголовках MinGW имён может не быть (как в RadialMenu)
constexpr DWORD kDwmDarkMode = 20;
constexpr DWORD kDwmCornerPreference = 33;
constexpr DWORD kDwmSystemBackdrop = 38;
constexpr int kBackdropNone = 1;
constexpr int kBackdropTransient = 3;  // акрил
constexpr int kCornerRound = 2;
constexpr int kCornerRoundSmall = 3;

// Пункты меню
enum MenuId : UINT {
    kMenuOpenFiles = 1,
    kMenuOpenFolder,
    kMenuImportTheme,
    kMenuExportTheme,
    kMenuMini,
    kMenuPlaylist,
    kMenuExit,
    kMenuNormalize,
    kMenuMiniTop,
    kMenuLibraryAll,
    kMenuLibraryRescan,
    kMenuLibraryAdd,
    kMenuLibraryAuto,
    kMenuPlugins,
    kMenuRowPlay,
    kMenuRowNext,
    kMenuRowShow,
    kMenuRowRemove,
    kMenuWave,
    kMenuStats,
    kMenuHome,
    kMenuThemeFirst = 100,          // встроенные темы по порядку
    kMenuLibraryFolderFirst = 200,  // папки библиотеки (убрать)
    kMenuPluginItemFirst = 1000,    // пункты, добавленные плагинами
};

// Глобальные медиаклавиши (RegisterHotKey)
enum HotKeyId : int { kHotPlay = 1, kHotNext, kHotPrev, kHotStop };

// Номер сборки Windows: RtlGetVersion не врёт про версию, в отличие от GetVersionEx
DWORD WindowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (!fn) return 0;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return fn(&info) == 0 ? info.dwBuildNumber : 0;
}

std::wstring FormatDuration(double seconds) {
    if (!(seconds > 0)) return L"";
    const long t = static_cast<long>(std::lround(seconds));
    wchar_t buf[32];
    if (t >= 3600)
        swprintf(buf, 32, L"%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else
        swprintf(buf, 32, L"%ld:%02ld", t / 60, t % 60);
    return buf;
}

// «1 трек», «3 трека», «12 треков»
const wchar_t* TracksWord(int n) {
    const int n10 = n % 10, n100 = n % 100;
    if (n10 == 1 && n100 != 11) return L"трек";
    if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return L"трека";
    return L"треков";
}

pm::Repeat RepeatFromString(const std::string& s) {
    if (s == "all") return pm::Repeat::All;
    if (s == "one") return pm::Repeat::One;
    return pm::Repeat::Off;
}

const char* RepeatToString(pm::Repeat r) {
    switch (r) {
        case pm::Repeat::All:
            return "all";
        case pm::Repeat::One:
            return "one";
        default:
            return "off";
    }
}

// Диалог выбора файлов или папки. Возвращает пустой список при отмене.
std::vector<std::filesystem::path> PickPaths(HWND owner, bool folder, bool multi, const wchar_t* filterName,
                                             const wchar_t* filterSpec, const wchar_t* title = nullptr) {
    std::vector<std::filesystem::path> out;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    if (title) dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder)
        opts |= FOS_PICKFOLDERS;
    else
        opts |= FOS_FILEMUSTEXIST;
    if (multi) opts |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(opts);
    if (!folder && filterSpec) {
        const COMDLG_FILTERSPEC spec[] = {{filterName, filterSpec}, {L"Все файлы", L"*.*"}};
        dlg->SetFileTypes(2, spec);
    }
    if (FAILED(dlg->Show(owner))) return out;
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return out;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return out;
}

// Текст из буфера обмена в UTF-8 (пусто - нет текста)
std::string ClipboardText(HWND owner) {
    std::string out;
    if (!OpenClipboard(owner)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = ::text::WideToUtf8(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

}  // namespace

// ---------------- запуск ----------------

int App::Run(HINSTANCE instance, int show) {
    // Один экземпляр: повторный запуск («Открыть с помощью», двойной клик по файлу) отдаёт
    // файлы уже открытому окну. Мьютекс живёт до выхода из процесса.
    CreateMutexW(nullptr, FALSE, kInstanceMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS && ForwardToRunningInstance()) return 0;

    QueryPerformanceFrequency(&freq_);
    ResolvePaths();
    std::vector<std::string> log;
    themes_.Load(assetsDir_, &log);
    win11Backdrop_ = WindowsBuild() >= 22621;
    ReadSystemReduce();

    if (!CreateMainWindow(instance)) return 1;

    std::string error;
    HWND hwnd = hwnd_;
    std::atomic<bool>* posted = &wakePosted_;
    // Аудиопоток будит окно; флаг не даёт засыпать очередь одинаковыми сообщениями
    auto wake = [hwnd, posted] {
        if (!posted->exchange(true)) PostMessageW(hwnd, kWakeMessage, 0, 0);
    };
    // Нет устройства вывода - плеер всё равно работает (без звука), чтобы не терять плейлист
    bool silent = false;
    if (!player_.Init({}, wake, &error)) {
        pm::Player::Options opts;
        opts.nullBackend = true;
        silent = player_.Init(opts, wake);
    }
    player_.SetOnTrackChanged([this](int index, int direction) { OnTrackChanged(index, direction); });
    pcm_.resize(analyzer_.FftSize());
    historyPath_ = statePath_.parent_path() / L"history.jsonl";
    // Тексты песен: кэш рядом с state.json, сеть - lrclib.net в фоновом потоке
    lyrics_.Start(
        statePath_.parent_path() / L"lyrics",
        [](const std::string& query, std::string& body) { return HttpsGet(L"lrclib.net", query, body); },
        [hwnd] { PostMessageW(hwnd, kLyricsMessage, 0, 0); });

    view_.SetHost(this);
    home_.Init(hwnd_, &renderer_, &text_);
    LoadHomePreferences();
    ReadSystemReduce();  // настройка «Учитывать эффекты анимации Windows» уже прочитана
    SetHomeWindowStyle();
    LoadSavedState();
    RefreshHome();
    if (homeMode_ && view_.mini) view_.ToggleMini(Now());
    ResizeWindow(homeMode_      ? homeSize_
                 : view_.mini   ? view::Size{view::kMiniWidth, view::kMiniHeight}
                 : view_.layout ? view_.layout->WindowSize(view_.layoutInput, view_.playlistOpen)
                                : view::Size{420, 680});
    LayoutHomeSearch();

    // Файлы из командной строки («Открыть с помощью»)
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        std::vector<std::filesystem::path> paths;
        for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
        LocalFree(argv);
        if (!paths.empty()) LoadPaths(paths);
    }
    started_ = true;
    lastCopyData_ = Now();  // файлы, пришедшие следом от других экземпляров, - дописываются

    if (introEnabled_ && MotionAllowed() && show != SW_SHOWMINNOACTIVE && show != SW_SHOWMINIMIZED) {
        intro_.Start(Now(), false);
        renderer_.PlayEntrance(Entrance::None, 0, nullptr);
    }
    ShowWindow(hwnd_, show);
    RegisterMediaKeys();
    trayCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, trayCreatedMsg_, MSGFLT_ALLOW, nullptr);
    AddTrayIcon();
    smtc_.Attach(hwnd_, kSmtcMessage);
    narrator_.Attach(hwnd_);
    if (const pm::NowPlaying& np = player_.Current(); np.index >= 0)  // трек из state.json - сразу в панель
        smtc_.SetTrack(np.title, np.tags.artist, np.tags.album, np.tags.track, np.tags.cover);
    if (silent) {
        const std::wstring msg =
            L"Не удалось открыть звуковое устройство, звук выключен.\n" + ::text::Utf8ToWide(error);
        MessageBoxW(hwnd_, msg.c_str(), L"PlayerMoon", MB_ICONWARNING);
    }
    MessageLoop();

    home_.Shutdown();
    watcher_.Stop();
    scanner_.Stop();
    plugins_.UnloadAll();
    taskbar_.Detach();
    smtc_.Detach();
    lyrics_.Stop();
    FinishListen();
    player_.Shutdown();
    renderer_.Shutdown();
    return 0;
}

void App::ResolvePaths() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    exeDir_ = std::filesystem::path(std::wstring(buf, n)).parent_path();
    assetsDir_ = exeDir_ / L"assets";

    // state.json рядом с exe, если туда можно писать (переносная папка); иначе %APPDATA%\PlayerMoon
    const std::filesystem::path probe = exeDir_ / L".write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        statePath_ = exeDir_ / L"state.json";
        return;
    }
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        std::filesystem::path dir = std::filesystem::path(appData) / L"PlayerMoon";
        CoTaskMemFree(appData);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        statePath_ = dir / L"state.json";
    } else {
        statePath_ = exeDir_ / L"state.json";
    }
}

bool App::CreateMainWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&wc)) return false;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarButtonCreated");

    // WS_CAPTION нужен для системной тени и скруглённых углов; саму рамку убирает WM_NCCALCSIZE
    const DWORD style =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, kClassName, L"PlayerMoon",
                            style, CW_USEDEFAULT, CW_USEDEFAULT, 420, 680, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    ReadRefreshRate();
    // Окно с повышенными правами иначе не получит сообщение о кнопке на панели задач
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);

    const MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    // Пересчитать рамку после того, как WM_NCCALCSIZE начал возвращать 0
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    if (!renderer_.Init(hwnd_)) {
        MessageBoxW(hwnd_, L"Не удалось запустить Direct3D 11 / Direct2D.", L"PlayerMoon", MB_ICONERROR);
        return false;
    }
    renderer_.SetDpi(dpi_);
    text_.Init(renderer_.DWrite(), assetsDir_ / L"fonts");
    icons_.Init(renderer_.Factory());
    covers_.Init(renderer_.Wic());
    painter_.Init(&renderer_, &text_, &icons_, &covers_);
    settingsPreview_.Init(&renderer_, &text_, &icons_, &covers_);
    return true;
}

// ---------------- состояние ----------------

void App::LoadSavedState() {
    pm::AppState st;
    // Нет файла - первый запуск: значения по умолчанию и тема под оформление Windows
    const bool firstRun = !pm::LoadState(statePath_, st);
    if (firstRun) st.themeId = SystemPrefersLight() ? "frost" : "moon";

    // Тема: своя (документ целиком) или встроенная по id
    theme::Theme t;
    std::string err;
    userTheme_ = !st.userTheme.IsNull() && themes_.FromDocument(st.userTheme, t, &err);
    if (!userTheme_) t = themes_.Make(st.themeId);
    view_.showRemaining = st.showRemaining;
    miniOnTop_ = st.miniOnTop;
    playlistFromLibrary_ = st.playlistFromLibrary;
    player_.SetNormalize(st.normalize);
    player_.SetCrossfade(st.crossfade);

    view_.RestoreModes(st.mini, st.playlistOpen);
    ApplyTheme(t, false);

    // Окно на прежнее место, если тот монитор ещё подключён
    if (st.hasWindowPos) {
        const POINT pt{st.windowX + 40, st.windowY + 20};
        if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL))
            SetWindowPos(hwnd_, nullptr, st.windowX, st.windowY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    player_.SetVolume(st.volume);
    player_.SetMuted(st.muted);
    player_.SetRepeat(RepeatFromString(st.repeat));
    player_.SetShuffle(st.shuffle);
    if (!st.playlist.empty()) {
        player_.LoadFiles(st.playlist);
        RebuildRows();
        const int count = player_.GetPlaylist().Size();
        const int index = std::clamp(st.index, 0, std::max(0, count - 1));
        if (count > 0) player_.OpenAt(index, st.position, false);
    }
    InitPlugins(st.plugins);
    InitLibrary(firstRun);
    if (!player_.GetState().hasTrack) OnTrackChanged(-1, 0);
    RebuildRows();
}

void App::SaveCurrentState() {
    pm::AppState st;
    const pm::Playlist& pl = player_.GetPlaylist();
    for (int i = 0; i < pl.Size(); ++i) st.playlist.push_back(pl.At(i).path);
    const pm::PlayerState ps = player_.GetState();
    st.index = ps.index;
    st.position = ps.position;
    st.volume = ps.volume;
    st.muted = ps.muted;
    st.repeat = RepeatToString(ps.repeat);
    st.shuffle = ps.shuffle;
    st.themeId = theme_.id;
    if (userTheme_) st.userTheme = themes_.ToUserDocument(theme_, theme_.name);
    st.normalize = player_.Normalize();
    st.crossfade = player_.Crossfade();
    st.showRemaining = view_.showRemaining;
    st.miniOnTop = miniOnTop_;
    st.playlistFromLibrary = playlistFromLibrary_;
    st.plugins = plugins_.EnabledIds();
    st.mini = view_.mini;
    st.playlistOpen = view_.playlistOpen;
    RECT rc{};
    if (GetWindowRect(hwnd_, &rc) && !IsIconic(hwnd_)) {
        st.hasWindowPos = true;
        st.windowX = rc.left;
        st.windowY = rc.top;
    }
    pm::SaveState(statePath_, st);
    SaveHomePreferences();
}

void App::MarkDirty() {
    // Повторный вызов переставляет таймер: серия изменений даёт одну запись
    if (hwnd_) SetTimer(hwnd_, kSaveTimer, 2000, nullptr);
}
void App::ResetTickTimer() {
    KillTimer(hwnd_, kTickTimer);
    tickTimer_ = false;
}

// ---------------- тема ----------------

void App::ReadSystemReduce() {
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    windowsAnimations_ = anim != FALSE;
    systemReduce_ = followSystemMotion_ && !anim;
    // Читается при запуске и по WM_SETTINGCHANGE: спрашивать систему на каждом кадре незачем
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    systemHighContrast_ =
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool App::SystemPrefersLight() {
    DWORD value = 0, size = sizeof(value);
    const LSTATUS r =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return r == ERROR_SUCCESS && value != 0;
}

void App::ApplyTopmost() {
    // Мини-плеер поверх окон (настройка), полный режим - обычное окно
    const bool want = !homeMode_ && view_.mini && miniOnTop_;
    if (want == topmostApplied_) return;
    SetWindowPos(hwnd_, want ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    topmostApplied_ = want;
}

void App::SnapToEdges(RECT* r) const {
    // Прилипание к краям рабочей области монитора при перетаскивании (ближе 16 DIP)
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const RECT& w = mi.rcWork;
    const LONG th = ToPx(16), width = r->right - r->left, height = r->bottom - r->top;
    if (std::abs(r->left - w.left) < th)
        r->left = w.left;
    else if (std::abs(r->right - w.right) < th)
        r->left = w.right - width;
    if (std::abs(r->top - w.top) < th)
        r->top = w.top;
    else if (std::abs(r->bottom - w.bottom) < th)
        r->top = w.bottom - height;
    r->right = r->left + width;
    r->bottom = r->top + height;
}

void App::ApplyTheme(const theme::Theme& t, bool animate) {
    theme_ = t;
    text_.SetFamilies(t.material.font.family, t.material.font.familyDisplay);
    // Геометрия - той раскладки, что реально будет (на этапе 1 у всех тем «Классика»)
    const std::string layoutId(view::MakeLayout(t.settings.layout)->Id());
    view_.ApplyTheme(t, themes_.Layout(layoutId), systemReduce_ || reduceMotion_, Now(), animate && started_);
    settingsPreview_.Apply(t, themes_.Layout(layoutId), systemReduce_ || reduceMotion_, Now());
    ApplyBackdrop();
    Invalidate();
}

void App::ApplyBackdrop() {
    const theme::Settings& s = theme_.settings;
    acrylicOn_ = win11Backdrop_ && s.backdrop == "acrylic" && renderer_.Composition();
    const int backdrop = acrylicOn_ ? kBackdropTransient : kBackdropNone;
    if (win11Backdrop_ && FAILED(DwmSetWindowAttribute(hwnd_, kDwmSystemBackdrop, &backdrop, sizeof(backdrop))))
        acrylicOn_ = false;
    // Тёмный режим рамки и акрила по цвету текста темы (светлый текст - тёмная тема)
    const BOOL dark = view_.DarkUi() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, kDwmDarkMode, &dark, sizeof(dark));
    // Углы окна системные (этап 1): маленький радиус темы - малое скругление
    const int corner = s.radius < 10 ? kCornerRoundSmall : kCornerRound;
    DwmSetWindowAttribute(hwnd_, kDwmCornerPreference, &corner, sizeof(corner));
}

// ---------------- трек и плейлист ----------------

void App::OnTrackChanged(int index, int direction) {
    lastActive_ = Now();
    view::TrackText t;
    const pm::NowPlaying& np = player_.Current();
    if (index >= 0 && started_)
        narrator_.Announce(L"Играет: " + ::text::Utf8ToWide(np.title) +
                           (np.tags.artist.empty() ? std::wstring() : L", " + ::text::Utf8ToWide(np.tags.artist)));
    if (index >= 0) {
        t.title = np.title;
        t.artist = np.tags.artist;
        t.album = np.tags.album;
        t.serial = np.serial;
        t.hasImage = covers_.Load(np.serial, np.tags.cover, np.title, t.palette);
    } else {
        // Пустой плейлист: подсказка вместо названия
        t.title = "PlayerMoon";
        t.artist = "Нет треков";
        t.album = "Правый клик - открыть файлы или папку";
        t.palette = theme::MakeCoverPalette(t.title);
    }
    view_.OnTrackChanged(t, direction, Now(), started_);
    std::vector<uint64_t> keep;
    for (const view::CoverSlot& c : view_.covers) keep.push_back(c.serial);
    covers_.Keep(keep);
    RebuildRows();      // длительность трека уточнилась после открытия декодера
    lyricsSerial_ = 0;  // текст нового трека ищется, когда видна вкладка «Текст»
    FinishListen();
    if (index >= 0) BeginListen();
    // Не из колбэка плеера: плейлист дописывается на ближайшем кадре
    if (waveMode_ && index >= 0 && index >= player_.GetPlaylist().Size() - 3) waveExtend_ = true;
    if (index >= 0)
        smtc_.SetTrack(np.title, np.tags.artist, np.tags.album, np.tags.track, np.tags.cover);
    else
        smtc_.ClearTrack();
    if (index >= 0 && started_) library_.NotePlayed(np.path, static_cast<int64_t>(time(nullptr)));
    NotifyPluginsTrack();
    MarkDirty();
    Invalidate();
}

void App::RebuildRows() {
    const pm::Playlist& pl = player_.GetPlaylist();
    rows_.clear();
    rows_.reserve(static_cast<size_t>(pl.Size()));
    double total = 0;
    for (int i = 0; i < pl.Size(); ++i) {
        const pm::PlaylistEntry& e = pl.At(i);
        PlaylistRow row;
        row.title = e.meta.title.empty() ? e.path.stem().wstring() : ::text::Utf8ToWide(e.meta.title);
        row.artist = ::text::Utf8ToWide(e.meta.artist);
        row.duration = FormatDuration(e.meta.duration);
        total += e.meta.duration;
        rows_.push_back(std::move(row));
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"%d %ls, %ld мин", pl.Size(), TracksWord(pl.Size()), std::lround(total / 60));
    countText_ = pl.Size() > 0 ? buf : L"";
    view_.SetPlaylistSize(pl.Size());
    RefreshHome();
}

void App::LoadPaths(const std::vector<std::filesystem::path>& paths, bool append) {
    // Одна папка - сканируется целиком; иначе берём файлы и папки по очереди
    std::vector<std::filesystem::path> files;
    bool replaced = append;
    const int before = player_.GetPlaylist().Size();
    playlistFromLibrary_ = false;
    if (!append) waveMode_ = false;
    LearnFolders(paths);
    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            player_.LoadFolder(p, replaced);
            replaced = true;
        } else {
            files.push_back(p);
        }
    }
    if (!files.empty()) player_.LoadFiles(files, replaced);
    RebuildRows();
    if (append && player_.GetPlaylist().Size() > before)
        view_.ShowToast("Добавлено треков: " + std::to_string(player_.GetPlaylist().Size() - before), Now());
    // Дописанные треки не прерывают текущий; новый плейлист начинается сразу
    if (!append && player_.GetPlaylist().Size() > 0)
        player_.PlayIndex(0);
    else if (append && !player_.GetState().hasTrack && player_.GetPlaylist().Size() > before)
        player_.PlayIndex(before);
    MarkDirty();
    Invalidate();
}

void App::OpenFiles(bool folder) {
    auto paths = PickPaths(hwnd_, folder, !folder, L"Аудио (MP3, FLAC, WAV, M4A, AAC, OGG, Opus)",
                           L"*.mp3;*.flac;*.wav;*.m4a;*.m4b;*.aac;*.ogg;*.oga;*.opus");
    if (!paths.empty()) LoadPaths(paths);
}

void App::LoadDropped(HDROP drop) {
    std::vector<std::filesystem::path> paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring s(len + 1, L'\0');
        DragQueryFileW(drop, i, s.data(), len + 1);
        s.resize(len);
        paths.emplace_back(s);
    }
    DragFinish(drop);
    if (!paths.empty()) LoadPaths(paths);
}

// ---------------- темы: импорт и экспорт ----------------

void App::ImportTheme() {
    auto paths = PickPaths(hwnd_, false, false, L"Тема PlayerMoon (*.json)", L"*.json");
    if (paths.empty()) return;
    std::string data, err;
    json::Value doc;
    theme::Theme t;
    if (!theme::ReadFile(paths[0], data) || !json::Parse(data, doc, &err) || !themes_.FromDocument(doc, t, &err)) {
        const std::wstring msg = L"Тема не подходит.\n" + ::text::Utf8ToWide(err);
        MessageBoxW(hwnd_, msg.c_str(), L"Импорт темы", MB_ICONWARNING);
        return;
    }
    userTheme_ = !t.builtin;
    ApplyTheme(t, true);
}

void App::ExportTheme() {
    Microsoft::WRL::ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC spec[] = {{L"Тема PlayerMoon (*.json)", L"*.json"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"json");
    const std::wstring name = ::text::Utf8ToWide(theme_.name.empty() ? std::string("Моя тема") : theme_.name);
    dlg->SetFileName(name.c_str());
    if (FAILED(dlg->Show(hwnd_))) return;
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    const std::filesystem::path file(path);
    CoTaskMemFree(path);
    const json::Value doc = themes_.ToUserDocument(theme_, theme_.name.empty() ? "Моя тема" : theme_.name);
    if (!theme::WriteFileAtomic(file, json::Dump(doc)))
        MessageBoxW(hwnd_, L"Не удалось записать файл.", L"Экспорт темы", MB_ICONWARNING);
}

// ---------------- меню ----------------

void App::ShowContextMenu(POINT) { OpenWorkspace(view::HomeModel::Settings); }

// ---------------- меню строки плейлиста ----------------

void App::ShowRowMenu(int row, view::Point at) {
    const pm::Playlist& pl = player_.GetPlaylist();
    if (row < 0 || row >= pl.Size()) return;
    const pm::PlayerState ps = player_.GetState();
    const bool current = row == ps.index;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuRowPlay, current && ps.playing ? L"Пауза" : L"Играть");
    AppendMenuW(menu, MF_STRING | (current ? MF_GRAYED : 0), kMenuRowNext, L"Играть следующим");
    AppendMenuW(menu, MF_STRING, kMenuRowShow, L"Показать в папке");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRowRemove, L"Убрать из плейлиста");
    POINT pt{ToPx(at.x), ToPx(at.y)};
    ClientToScreen(hwnd_, &pt);
    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, nullptr));
    DestroyMenu(menu);
    const std::filesystem::path path = pl.At(row).path;
    switch (cmd) {
        case kMenuRowPlay:
            if (current)
                player_.Toggle();
            else
                player_.PlayIndex(row);
            break;
        case kMenuRowNext:
            player_.PlayNext(row);
            break;
        case kMenuRowShow:
            // Проводник с выделенным файлом
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
            break;
        case kMenuRowRemove:
            player_.Remove(row);
            break;
        default:
            return;
    }
    RebuildRows();
    MarkDirty();
    Invalidate();
}

// ---------------- медиаклавиши ----------------

void App::RegisterMediaKeys() {
    // Глобально, чтобы работали и без фокуса. Если клавишу уже занял другой плеер, остаётся
    // запасной путь WM_APPCOMMAND (только когда окно активно).
    RegisterHotKey(hwnd_, kHotPlay, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(hwnd_, kHotNext, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(hwnd_, kHotPrev, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(hwnd_, kHotStop, MOD_NOREPEAT, VK_MEDIA_STOP);
}

void App::HandleMediaCommand(int command) {
    switch (command) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE:
            TogglePlay();
            break;
        case APPCOMMAND_MEDIA_PLAY:
            player_.Play();
            break;
        case APPCOMMAND_MEDIA_PAUSE:
        case APPCOMMAND_MEDIA_STOP:
            player_.Pause();
            break;
        case APPCOMMAND_MEDIA_NEXTTRACK:
            Next();
            break;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK:
            Prev();
            break;
        default:
            break;
    }
    Invalidate();
}

// ---------------- IViewHost ----------------

void App::TogglePlay() {
    player_.Toggle();
    Invalidate();
}
void App::Next() {
    player_.Next();
    Invalidate();
}
void App::Prev() {
    player_.Prev();
    Invalidate();
}
void App::Seek(double seconds) {
    player_.Seek(seconds);
    Invalidate();
}
void App::SetVolume(float volume) {
    if (player_.GetState().muted && volume > 0) player_.SetMuted(false);
    player_.SetVolume(volume);
    Invalidate();
}
void App::ToggleMute() {
    player_.SetMuted(!player_.GetState().muted);
    Invalidate();
}
void App::CycleRepeat() {
    const pm::Repeat r = player_.GetState().repeat;
    player_.SetRepeat(r == pm::Repeat::Off   ? pm::Repeat::All
                      : r == pm::Repeat::All ? pm::Repeat::One
                                             : pm::Repeat::Off);
    Invalidate();
}
void App::ToggleShuffle() {
    player_.SetShuffle(!player_.GetState().shuffle);
    Invalidate();
}
void App::PlayIndex(int index) {
    player_.PlayIndex(index);
    Invalidate();
}

void App::ResizeWindow(view::Size dip) {
    if (homeMode_) {
        if (IsZoomed(hwnd_)) {
            Invalidate();
            return;
        }
        dip = homeSize_;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            dip.w = std::min(dip.w, (mi.rcWork.right - mi.rcWork.left) * 96.f / dpi_ - 24);
            dip.h = std::min(dip.h, (mi.rcWork.bottom - mi.rcWork.top) * 96.f / dpi_ - 24);
        }
    }
    // Мгновенно: левый верхний угол на месте, содержимое анимирует PlayerView
    SetWindowPos(hwnd_, nullptr, 0, 0, ToPx(dip.w), ToPx(dip.h), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Смена вида окна (не открытие панели плейлиста) - окно появляется заново со своим эффектом
    const WindowForm form = homeMode_                                                   ? WindowForm::Home
                            : (dip.w == view::kMiniWidth && dip.h == view::kMiniHeight) ? WindowForm::Mini
                                                                                        : WindowForm::Compact;
    if (form != windowForm_) {
        PlayWindowEntrance(windowForm_, form);
        windowForm_ = form;
    }
    Invalidate();
}

bool App::HighContrast() const { return highContrast_ || systemHighContrast_; }

bool App::MotionAllowed() const { return !(systemReduce_ || reduceMotion_) && theme_.settings.motion != "none"; }

void App::PlayWindowEntrance(WindowForm from, WindowForm to, Entrance preferred) {
    if (!MotionAllowed() || (from == WindowForm::Unknown && intro_.Active(Now()) && !intro_.Closing())) {
        renderer_.PlayEntrance(Entrance::None, 0, nullptr);
        return;
    }
    // Своё движение для каждого перехода: главная раскрывается кругом, плеер откидывается крышкой,
    // мини-полоска разъезжается из середины, из полоски плеер поднимается снизу.
    Entrance fx = Entrance::Pop;
    if (windowFx_ == 0 && preferred != Entrance::None) {
        fx = preferred;
    } else if (windowFx_ == 0) {
        if (from == WindowForm::Unknown)
            fx = Entrance::Pop;
        else if (to == WindowForm::Home)
            fx = Entrance::Iris;
        else if (to == WindowForm::Mini)
            fx = Entrance::Split;
        else if (from == WindowForm::Mini)
            fx = Entrance::Rise;
        else
            fx = Entrance::Unfold;
    } else {
        const Entrance fixed[] = {Entrance::Pop,  Entrance::Rise,    Entrance::Unfold,
                                  Entrance::Iris, Entrance::Curtain, Entrance::None};
        fx = fixed[std::clamp(windowFx_ - 1, 0, 5)];
    }
    const auto& s = theme_.settings;
    const auto spring = motion::Easing::Spring({s.stiffness, s.damping});
    const double base = fx == Entrance::Iris || fx == Entrance::Unfold ? 0.52 : 0.44;
    renderer_.PlayEntrance(fx, base * std::clamp(s.speed, 0.5, 2.0), [spring](double u) { return spring.Eval(u); });
}

void App::MinimizeWindow() { ShowWindow(hwnd_, SW_MINIMIZE); }
// Крестик прячет окно; WM_CLOSE (пункт «Выход», другие программы, завершение Windows) по-прежнему завершает плеер
void App::CloseWindow() { HideToBackground(); }
void App::QuitApp() { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }
void App::StartCloseAnimation() {
    intro_.Start(Now(), true);
    SetTimer(hwnd_, kCloseFade, (UINT)(Intro::kClose * 1000) - 120, nullptr);
    Invalidate();
}

void App::ShowMenu(view::Point at) {
    POINT pt{ToPx(at.x), ToPx(at.y)};
    ClientToScreen(hwnd_, &pt);
    ShowContextMenu(pt);
}

float App::MeasureText(view::TextStyle style, const std::string& s) {
    TextSpec spec;
    switch (style) {
        case view::TextStyle::Title:
            spec = Painter::TitleSpec(view_);
            break;
        case view::TextStyle::MiniTitle:
            spec = Painter::MiniTitleSpec(view_);
            break;
        case view::TextStyle::Tab:
            spec = Painter::TabSpecFor(view_, false);
            break;
        case view::TextStyle::TabSmall:
            spec = Painter::TabSpecFor(view_, true);
            break;
        case view::TextStyle::Pill:
            spec = Painter::PillSpec();
            break;
        case view::TextStyle::Tooltip:
            spec = Painter::TooltipSpec();
            break;
        case view::TextStyle::Lyric:
            spec = Painter::LyricSpec(view_);
            break;
    }
    return text_.Measure(::text::Utf8ToWide(s), spec);
}

float App::MeasureWrapped(view::TextStyle style, const std::string& s, float width) {
    const TextSpec spec = style == view::TextStyle::Lyric ? Painter::LyricSpec(view_) : Painter::TooltipSpec();
    return text_.MeasureHeight(::text::Utf8ToWide(s), spec, width);
}

void App::RequestLyrics() {
    const pm::NowPlaying& np = player_.Current();
    if (np.index < 0) {
        view_.SetLyrics(0, view::LyricsState::None, {}, {}, Now());
        return;
    }
    if (lyricsSerial_ == np.serial) return;
    lyricsSerial_ = np.serial;
    pm::LyricsRequest r;
    r.serial = np.serial;
    r.track = np.path;
    r.title = np.title;
    r.artist = np.tags.artist;
    r.album = np.tags.album;
    r.duration = player_.GetState().duration;
    view_.SetLyrics(np.serial, view::LyricsState::Loading, {}, {}, Now());
    lyrics_.Request(std::move(r));
}

void App::OnLyricsReady() {
    pm::LyricsResult r;
    if (!lyrics_.Take(r) || r.serial != player_.Current().serial) return;
    view::LyricsState state = view::LyricsState::NotFound;
    std::vector<std::string> lines;
    std::vector<double> times;
    switch (r.status) {
        case pm::LyricsStatus::Found:
            state = r.lyrics.synced ? view::LyricsState::Synced : view::LyricsState::Plain;
            for (auto& l : r.lyrics.lines) {
                lines.push_back(std::move(l.text));
                times.push_back(l.time);
            }
            break;
        case pm::LyricsStatus::Instrumental:
            state = view::LyricsState::Instrumental;
            break;
        case pm::LyricsStatus::NetworkError:
            state = view::LyricsState::Offline;  // снова - со следующим треком (без повторов на каждом кадре)
            break;
        case pm::LyricsStatus::NotFound:
            break;
    }
    view_.SetLyrics(r.serial, state, std::move(lines), std::move(times), Now());
    Invalidate();
}

void App::PageAction(int page, int rowId, int actionId) {
    // Страница «Итоги» - после страниц плагинов
    if (page >= static_cast<int>(plugins_.Pages().size())) {
        StatsAction(actionId);
        return;
    }
    plugins_.PageAction(static_cast<size_t>(page), rowId, actionId);
    SyncPages();
    Invalidate();
}

void App::SyncPages() {
    if (plugins_.PagesRevision() == pagesRevision_ && statsRevision_ == statsShown_) return;
    pagesRevision_ = plugins_.PagesRevision();
    statsShown_ = statsRevision_;
    std::vector<view::PanelPage> pages;
    for (const auto& src : plugins_.Pages()) {
        view::PanelPage p;
        p.title = src.title;
        p.emptyText = src.emptyText;
        for (const auto& a : src.actions) p.actions.push_back({a.id, a.label, 0});
        for (const auto& r : src.rows) {
            view::PanelRow row{r.id, r.title, r.subtitle, r.status, r.progress, r.state, {}};
            for (const auto& a : r.actions) row.actions.push_back({a.id, a.label, 0});
            p.rows.push_back(std::move(row));
        }
        pages.push_back(std::move(p));
    }
    if (statsOpen_) pages.push_back(BuildStatsPage());
    view_.SetPages(std::move(pages), Now());
    Invalidate();
}

bool App::InPlaylist(const std::filesystem::path& p) const {
    const pm::Playlist& pl = player_.GetPlaylist();
    for (int i = 0; i < pl.Size(); ++i)
        if (pl.At(i).path == p) return true;
    return false;
}

void App::AddFilesFromPlugin(const std::vector<std::string>& paths, bool play) {
    std::vector<std::filesystem::path> files;
    int playIndex = -1;
    const pm::Playlist& pl = player_.GetPlaylist();
    for (const auto& s : paths) {
        const std::filesystem::path p = ::text::Utf8ToPath(s);
        if (!pm::IsSupportedAudio(p)) continue;
        // Уже в плейлисте (например, библиотека успела добавить) - не дублируем
        int found = -1;
        for (int i = 0; i < pl.Size() && found < 0; ++i)
            if (pl.At(i).path == p) found = i;
        if (found >= 0) {
            if (playIndex < 0) playIndex = found;
        } else {
            files.push_back(p);
        }
    }
    const int before = pl.Size();
    if (!files.empty()) {
        player_.LoadFiles(files, true);
        RebuildRows();
        if (playIndex < 0) playIndex = before;
    }
    if (play && playIndex >= 0)
        player_.PlayIndex(playIndex);
    else if (!player_.GetState().hasTrack && player_.GetPlaylist().Size() > 0)
        player_.OpenAt(0, 0, false);
    MarkDirty();
    Invalidate();
}

// ---------------- кадры ----------------

double App::Now() const {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return freq_.QuadPart ? static_cast<double>(t.QuadPart) / static_cast<double>(freq_.QuadPart) : 0;
}

int App::ToPx(float dip) const { return static_cast<int>(std::lround(dip * dpi_ / 96.f)); }

view::Point App::ToDip(LPARAM lp) const {
    const float k = 96.f / dpi_;
    return {static_cast<float>(GET_X_LPARAM(lp)) * k, static_cast<float>(GET_Y_LPARAM(lp)) * k};
}

view::Size App::ClientDip() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float k = 96.f / dpi_;
    return {static_cast<float>(rc.right - rc.left) * k, static_cast<float>(rc.bottom - rc.top) * k};
}

void App::ReadRefreshRate() {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info) &&
        EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        refreshHz_ = mode.dmDisplayFrequency;
}

// Ввод и смена трека - сразу и ещё секунду с полной частотой экрана: наведение, прокрутка и переходы плавные как
// раньше. В остальное время кадр показывается раз в несколько обновлений, ближе всего к 60 в секунду (165 Гц -> 55, 144
// Гц -> 72). Экран 60-75 Гц ничего не теряет: там интервал остаётся 1.
void App::PlanFramePace(double now) {
    const bool active = now - lastActive_ < 1.0 || intro_.Active(now);
    framePace_ = active ? 1 : std::clamp<UINT>((refreshHz_ + 30) / 60, 1, 4);
    renderer_.SetSyncInterval(framePace_);
}

bool App::UpdateSpectrum(SpectrumRun& run, int bars) {
    const uint64_t written = player_.PcmWritten();
    if (written == run.written && run.bars.size() == static_cast<size_t>(bars)) return true;
    if (player_.ReadPcm(pcm_.data(), pcm_.size()) != pcm_.size()) return false;
    analyzer_.Compute(pcm_.data(), player_.SampleRate(), bars, run.bars);
    run.written = written;
    return true;
}

void App::RenderFrame() {
    if (rendering_) return;
    struct RenderGuard {
        bool& flag;
        explicit RenderGuard(bool& value) : flag(value) { flag = true; }
        ~RenderGuard() { flag = false; }
    } guard(rendering_);
    dirty_ = false;
    visible_ = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    const double t = Now();
    // Сглаживание задано «за обновление экрана»: кадр длиной в несколько обновлений делает столько же шагов
    const float paceSteps = static_cast<float>(lastFramePace_);
    const auto paced = [paceSteps](float k) { return paceSteps <= 1 ? k : 1 - std::pow(1 - k, paceSteps); };
    lastFramePace_ = framePace_;
    PlanFramePace(t);
    const pm::PlayerState ps = player_.GetState();
    view::PlaybackSnapshot snap;
    snap.playing = ps.playing;
    snap.hasTrack = ps.hasTrack;
    snap.position = ps.position;
    snap.duration = ps.duration;
    snap.volume = ps.volume;
    snap.muted = ps.muted;
    snap.repeat = ps.repeat;
    snap.shuffle = ps.shuffle;
    snap.index = ps.index;
    snap.count = ps.count;

    // Спектр считаем, только когда визуализатор видно и звук идёт
    const std::vector<float>* spectrum = nullptr;
    if (!homeMode_ && visible_ && ps.playing && view_.WantsSpectrum() && player_.AudioDeviceRunning() &&
        view_.viz.Bars() > 0) {
        if (UpdateSpectrum(playerSpectrum_, view_.viz.Bars())) spectrum = &playerSpectrum_.bars;
    }
    if (waveExtend_) {
        waveExtend_ = false;
        ExtendWave();
    }
    SyncPages();
    if (view_.WantsLyrics() || (homeMode_ && home_.NowPlayingOpen())) RequestLyrics();
    view_.SetBusy(scanner_.Running(), "Поиск музыки");
    view_.Update(t, snap, spectrum, visible_);
    ApplyTopmost();
    // Подсказка или уведомление ждут своего времени: разовый таймер вместо цикла кадров
    const double wake = view_.NextLazyWake();
    if (wake < 1e300 && !view_.NeedsFrames()) {
        SetTimer(hwnd_, kLazyTimer, static_cast<UINT>(std::clamp((wake - t) * 1000.0 + 5, 10.0, 60000.0)), nullptr);
        lazyTimer_ = true;
    } else if (lazyTimer_) {
        KillTimer(hwnd_, kLazyTimer);
        lazyTimer_ = false;
    }
    taskbar_.Update(ps.hasTrack, ps.playing, ps.position, ps.duration);
    smtc_.Update(ps.hasTrack, ps.playing, ps.position, ps.duration);
    listen_.Advance(ps.position, ps.playing);
    if (lastPlaying_ != (ps.playing ? 1 : 0)) {
        if (lastPlaying_ >= 0) plugins_.NotifyPlayState(ps.playing);
        lastPlaying_ = ps.playing ? 1 : 0;
    }

    // Раз в секунду: мини-режим во время игры и свёрнутое окно (полоса на панели задач)
    const bool wantTicks = homeMode_ ? ps.playing : view_.WantsSecondTicks() || (ps.playing && !visible_);
    if (wantTicks != tickTimer_) {
        if (wantTicks)
            SetTimer(hwnd_, kTickTimer, homeMode_ && visible_ ? 250 : 1000, nullptr);
        else
            KillTimer(hwnd_, kTickTimer);
        tickTimer_ = wantTicks;
    }
    // Во время игры позиция сохраняется раз в 30 секунд; на паузе таймера нет
    if (ps.playing != saveTick_) {
        if (ps.playing)
            SetTimer(hwnd_, kSaveTick, 30000, nullptr);
        else
            KillTimer(hwnd_, kSaveTick);
        saveTick_ = ps.playing;
    }
    if (!visible_) return;

    ID2D1DeviceContext* dc = renderer_.BeginFrame();
    if (!dc) {
        SetTimer(hwnd_, kGraphicsRetry, 250, nullptr);
        graphicsRetry_ = true;
        return;
    }
    if (graphicsRetry_) {
        KillTimer(hwnd_, kGraphicsRetry);
        graphicsRetry_ = false;
    }
    PaintInput in;
    in.window = ClientDip();
    in.systemBackdrop = acrylicOn_;
    in.rows = &rows_;
    in.countText = countText_;
    in.emptyHint = scanner_.Running() ? L"Ищу музыку в папках библиотеки..." : L"Перетащите сюда файлы или папку";
    in.plugins = &pluginRows_;
    in.pluginCountText = pluginCountText_;
    in.pluginsHint = L"Плагинов пока нет. Положите папку плагина в plugins рядом с PlayerMoon.exe";
    if (homeMode_) {
        homeInput_.size = ClientDip();
        homeInput_.playback = snap;
        homeInput_.scanning = scanner_.Running();
        homeInput_.searchFocused = GetFocus() == homeSearch_;
        if (homeInput_.searchFocused)  // без фокуса выделение нигде не рисуется
            SendMessageW(homeSearch_, EM_GETSEL, (WPARAM)&homeInput_.selectionStart, (LPARAM)&homeInput_.selectionEnd);
        const auto& np = player_.Current();
        if (homeTrackSerial_ != np.serial) {  // другой трек - другой serial; иначе те же строки
            homeTrackSerial_ = np.serial;
            homeInput_.current = np.path;
            homeInput_.title = ::text::Utf8ToWide(np.title);
            homeInput_.artist = ::text::Utf8ToWide(np.tags.artist);
        }
        {
            // Главная в цветах темы: живой акцент плеера (из темы или обложки) и оттенок окна темы
            HomeColors colors;
            const auto live = view_.accent.Value(t);
            const auto& s = theme_.settings;
            colors.accent = D2D1::ColorF(live.r, live.g, live.b);
            colors.accent2 = s.accentFromCover ? MixColor(colors.accent, D2D1::ColorF(0xa78bfa), .55f)
                                               : MixColor(D2D1::ColorF(s.accent2.r, s.accent2.g, s.accent2.b),
                                                          D2D1::ColorF(0xa78bfa), .35f);
            const auto win = D2D1::ColorF(s.winColor.r, s.winColor.g, s.winColor.b);
            const float light = .2126f * win.r + .7152f * win.g + .0722f * win.b;
            // Тёмное окно темы - основа фона; у светлой темы берём только лёгкий оттенок акцента
            const D2D1_COLOR_F tint = light < .35f ? D2D1_COLOR_F(win) : colors.accent;
            const float k = light < .35f ? .55f : .05f;
            colors.bg = MixColor(colors.bg, tint, k * .6f);
            colors.surface = MixColor(colors.surface, tint, k * .5f);
            colors.card = MixColor(colors.card, tint, k * .35f);
            homeInput_.colors = colors;
        }
        BuildWorkspace();
        // Живой фон главной слушает музыку: 8 полос спектра, бас сглаживается, чтобы свет дышал, а не мигал
        homeInput_.ambient = ps.playing && !(systemReduce_ || reduceMotion_) && theme_.settings.motion != "none" &&
                             GetForegroundWindow() == hwnd_;
        float bass = 0;
        // Полосы эквалайзера - всегда, пока играет музыка: это показатель звука, а не украшение.
        // При «меньше движения» кадров не просим - они идут по таймеру четыре раза в секунду
        homeInput_.bandsLive = false;
        if (ps.playing && visible_ && player_.AudioDeviceRunning() && UpdateSpectrum(homeSpectrum_, 8)) {
            const auto& homeBands = homeSpectrum_.bars;
            for (size_t i = 0; i < 8 && i < homeBands.size(); ++i)
                homeInput_.bands[i] += (homeBands[i] - homeInput_.bands[i]) * paced(.45f);
            homeInput_.bandsLive = true;
            bass = homeBands.size() >= 2 ? (homeBands[0] + homeBands[1]) * .5f : 0;
        }
        homeInput_.energy +=
            (std::clamp(bass, 0.f, 1.f) - homeInput_.energy) * paced(bass > homeInput_.energy ? .35f : .08f);
        // Доля такта: вспышка 1 и быстрое затухание (~150 мс)
        if (homeInput_.ambient) beat_.Feed(bass, t, paced(.06f));
        homeInput_.beat = homeInput_.ambient ? (float)std::exp(-(t - beat_.Last()) * 7) : 0.f;
        homeInput_.karaoke = player_.Karaoke();
        homeInput_.coverFrame = home_.HasFrame(coverFrame_) ? coverFrame_ : 0;
        homeInput_.skin = theme_.settings.skin;
        // Полноэкранный вид: текст песни и 48 полос для волны
        homeInput_.lyricsState = 0;
        if (home_.NowPlayingOpen()) {
            // Текст песни меняется вместе с вектором строк: на каждом кадре его не перекодируем
            if (homeLyricsShown_ != view_.lyrics.lines.data() || homeLyricsCount_ != view_.lyrics.lines.size()) {
                homeLyricsShown_ = view_.lyrics.lines.data();
                homeLyricsCount_ = view_.lyrics.lines.size();
                homeInput_.lyrics.clear();
                for (const auto& line : view_.lyrics.lines) homeInput_.lyrics.push_back(::text::Utf8ToWide(line));
                homeInput_.lyricTimes = view_.lyrics.times;
            }
            homeInput_.lyricsState = (int)view_.lyrics.state;
            if (ps.playing && player_.AudioDeviceRunning() && UpdateSpectrum(nowSpectrum_, 48)) {
                const auto& bars = nowSpectrum_.bars;
                homeInput_.spectrum.resize(bars.size());
                for (size_t i = 0; i < bars.size(); ++i)
                    homeInput_.spectrum[i] += (bars[i] - homeInput_.spectrum[i]) * paced(.5f);
            } else {
                for (auto& v : homeInput_.spectrum) v *= 1 - paced(.15f);
            }
        } else {
            homeInput_.spectrum.clear();
            homeInput_.lyrics.clear();
            homeInput_.lyricTimes.clear();
            homeLyricsShown_ = nullptr;
            homeLyricsCount_ = 0;
        }
        homeInput_.settingsPreview = nullptr;
        if (home_.model.page == view::HomeModel::Settings) {
            view::TrackText demo;
            demo.title = np.title.empty() ? "Ночная орбита" : np.title;
            demo.artist = np.tags.artist.empty() ? "PlayerMoon" : np.tags.artist;
            demo.album = np.tags.album.empty() ? "Ваше оформление" : np.tags.album;
            demo.serial = np.serial;
            demo.hasImage = !np.tags.cover.empty();
            demo.palette = theme::MakeCoverPalette(demo.title);
            if (!view_.covers.empty() && view_.covers.back().serial == np.serial) {
                demo.palette = view_.covers.back().palette;
                demo.hasImage = view_.covers.back().hasImage;
            }
            homeInput_.settingsPreview = settingsPreview_.Render(
                dc, demo, t, previewMini_,
                !(systemReduce_ || reduceMotion_) && theme_.settings.motion != "none" && GetForegroundWindow() == hwnd_,
                view_.showRemaining);
            homeInput_.previewSize = settingsPreview_.Size();
        }
        UpdateSettingsHoverPreview();
        if (HighContrast()) {
            // Высокий контраст: чёрный фон, белый текст, системный цвет выделения, без живого фона
            HomeColors hc;
            hc.bg = D2D1::ColorF(0, 0, 0);
            hc.surface = D2D1::ColorF(0x0a0a0a);
            hc.card = D2D1::ColorF(0x1c1c1c);
            hc.white = D2D1::ColorF(1, 1, 1);
            hc.muted = D2D1::ColorF(0xe0e0e0);
            const COLORREF sel = GetSysColor(COLOR_HIGHLIGHT);
            hc.accent = D2D1::ColorF(GetRValue(sel) / 255.f, GetGValue(sel) / 255.f, GetBValue(sel) / 255.f);
            hc.accent2 = hc.accent;
            homeInput_.colors = hc;
            homeInput_.ambient = false;
            homeInput_.reducedMotion = true;
        }
        homeInput_.highContrast = HighContrast();
        home_.Paint(dc, homeInput_);
        if (const auto spoken = home_.TakeToast(); !spoken.empty()) narrator_.Announce(spoken);
        if (const double homeWake = home_.NextWake(); homeWake > 0 && !home_.NeedsFrames()) {
            // Paint считает время по GetTickCount64, таймеру важен только промежуток
            SetTimer(hwnd_, kLazyTimer,
                     static_cast<UINT>(std::clamp((homeWake - GetTickCount64() / 1000.) * 1000.0 + 5, 10.0, 60000.0)),
                     nullptr);
            lazyTimer_ = true;
        }
    } else
        painter_.Paint(dc, view_, in);
    if (intro_.Active(t)) {
        const auto live = view_.accent.Value(t);
        const auto win = theme_.settings.winColor;
        const D2D1_COLOR_F bg = homeMode_ ? homeInput_.colors.bg : D2D1::ColorF(win.r * .8f, win.g * .8f, win.b * .8f);
        const auto size = ClientDip();
        intro_.Paint(dc, renderer_.Factory(), renderer_.DWrite(), {size.w, size.h}, t, bg,
                     D2D1::ColorF(live.r, live.g, live.b), home_.SharedPicture(dc, Home::IntroBackdrop));
    }
    if (!renderer_.EndFrame()) dirty_ = true;  // устройство пересоздано - повторить кадр
}

void App::MessageLoop() {
    MSG msg;
    const auto noteInput = [this](UINT message) {
        if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            (message >= WM_NCMOUSEMOVE && message <= WM_NCMBUTTONDBLCLK))
            lastActive_ = Now();
    };
    for (;;) {
        const bool demo =
            homeMode_ && home_.model.page == view::HomeModel::Settings && GetForegroundWindow() == hwnd_ &&
            (settingsPreview_.NeedsFrames() || (!(systemReduce_ || reduceMotion_) && theme_.settings.motion != "none"));
        const bool frames =
            dirty_ ||
            (visible_ && (intro_.Active(Now()) || (homeMode_ ? home_.NeedsFrames() || demo : view_.NeedsFrames())));
        HANDLE waitable = renderer_.FrameWaitable();
        if (frames && waitable) {
            // Ждём, пока DWM готов принять кадр, и одновременно обрабатываем сообщения
            const DWORD r = MsgWaitForMultipleObjectsEx(1, &waitable, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT) {
                RenderFrame();
            } else if (r == WAIT_FAILED) {
                renderer_.ResetDevice();
                RenderFrame();
            }
        } else if (frames) {
            RenderFrame();
        } else {
            // Покой: ноль кадров, поток спит до сообщения
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) return;
            noteInput(msg.message);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            noteInput(msg.message);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// ---------------- окно ----------------

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = nullptr;
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (HandleBackgroundMessage(msg, wp, lp)) return 0;
    if (msg == WM_DISPLAYCHANGE || msg == WM_EXITSIZEMOVE) ReadRefreshRate();
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
        taskbar_.Attach(hwnd_, renderer_.Factory(), renderer_.Wic());
        Invalidate();
        return 0;
    }
    switch (msg) {
        case Home::kArtReady:
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO:
            if (homeMode_) {
                auto* info = reinterpret_cast<MINMAXINFO*>(lp);
                info->ptMinTrackSize = {ToPx(900), ToPx(640)};
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
                    info->ptMaxPosition = {mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top};
                    info->ptMaxSize = {mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
                }
                return 0;
            }
            break;
        case WM_GETOBJECT: {
            bool handled = false;
            const LRESULT result = narrator_.OnGetObject(wp, lp, handled);
            if (handled) return result;
            break;
        }
        case WM_NCPAINT:
            return 0;  // The custom titlebar is drawn by Direct2D, including after activation.
        case WM_NCACTIVATE:
            Invalidate();
            return DefWindowProcW(hwnd_, msg, wp, -1);
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == homeSearch_ || (HWND)lp == downloadEdit_ || (HWND)lp == settingsEdit_ ||
                (HWND)lp == paletteEdit_) {
                SetTextColor((HDC)wp, RGB(239, 235, 238));
                SetBkColor((HDC)wp, RGB(17, 20, 28));
                SetDCBrushColor((HDC)wp, RGB(17, 20, 28));
                return (LRESULT)GetStockObject(DC_BRUSH);
            }
            break;
        case WM_NCCALCSIZE:
            // Клиентская область = всё окно: рамку и заголовок рисуем сами
            if (wp) {
                if (homeMode_ && IsZoomed(hwnd_)) {
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi))
                        reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] = mi.rcWork;
                }
                return 0;
            }
            break;
        case WM_NCLBUTTONDBLCLK:
            if (!homeMode_ && wp == HTCAPTION) return 0;
            break;
        case WM_SYSCOMMAND:
            if (!homeMode_ && ((wp & 0xfff0) == SC_MAXIMIZE || (wp & 0xfff0) == SC_SIZE)) return 0;
            break;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const float k = 96.f / dpi_;
            const view::Point p{static_cast<float>(pt.x) * k, static_cast<float>(pt.y) * k};
            if (homeMode_) {
                auto size = ClientDip();
                if (!IsZoomed(hwnd_)) {
                    bool l = p.x<6, r = p.x> size.w - 6, t = p.y<6, b = p.y> size.h - 6;
                    if (t && l) return HTTOPLEFT;
                    if (t && r) return HTTOPRIGHT;
                    if (b && l) return HTBOTTOMLEFT;
                    if (b && r) return HTBOTTOMRIGHT;
                    if (l) return HTLEFT;
                    if (r) return HTRIGHT;
                    if (t) return HTTOP;
                    if (b) return HTBOTTOM;
                }
                return p.y < 48 && p.x < size.w - 132 ? HTCAPTION : HTCLIENT;
            }
            return view_.HitTest(p) == view::PlayerView::Hit::Caption ? HTCAPTION : HTCLIENT;
        }
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) renderer_.Resize(LOWORD(lp), HIWORD(lp));
            if (wp != SIZE_MINIMIZED) LayoutHomeSearch();
            visible_ = wp != SIZE_MINIMIZED;
            Invalidate();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            EndPaint(hwnd_, &paint);
            Invalidate();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // Direct2D owns the full client area.
        case WM_SHOWWINDOW:
            Invalidate();
            break;
        case WM_DPICHANGED: {
            dpi_ = static_cast<float>(HIWORD(wp));
            renderer_.SetDpi(dpi_);
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            // Use Windows' monitor-aware rectangle, especially for a maximized dashboard.
            int width = suggested->right - suggested->left, height = suggested->bottom - suggested->top;
            if (!homeMode_) {
                const auto size = view_.mini     ? view::Size{view::kMiniWidth, view::kMiniHeight}
                                  : view_.layout ? view_.layout->WindowSize(view_.layoutInput, view_.playlistOpen)
                                                 : view::Size{420, 680};
                width = ToPx(size.w);
                height = ToPx(size.h);
            }
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutHomeSearch();
            Invalidate();
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                trackingMouse_ = TrackMouseEvent(&tme);
            }
            if (homeMode_) {
                home_.Move(ToDip(lp));
                // Ползунок настроек тянется за мышью
                int command = 0;
                float fraction = 0;
                if ((wp & MK_LBUTTON) && home_.ActiveSlider(command, fraction)) {
                    SliderCommand(command, fraction, !sliderDragging_);
                    sliderDragging_ = true;
                }
            } else
                view_.OnMouseMove(ToDip(lp), Now());
            Invalidate();
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            if (homeMode_)
                home_.Move({-1, -1});
            else
                view_.OnMouseLeave(Now());
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            home_.ClearFocus();  // мышью - контур клавиатуры не нужен
            SetCapture(hwnd_);
            if (homeMode_) {
                SetFocus(hwnd_);
                home_.Move(ToDip(lp));
                home_.Press(ToDip(lp));
                int command = 0;
                float fraction = 0;
                if (home_.ActiveSlider(command, fraction)) {
                    SliderCommand(command, fraction, true);
                    sliderDragging_ = true;
                }
            } else
                view_.OnMouseDown(ToDip(lp), Now());
            Invalidate();
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            if (homeMode_) {
                const bool slid = sliderDragging_;
                sliderDragging_ = false;
                const auto drop = home_.Release(ToDip(lp));
                if (slid) {
                    SaveHomePreferences();
                    MarkDirty();
                    Invalidate();
                    return 0;
                }
                if (drop.kind != Home::Drop::None)
                    HomeDrop(drop);
                else if (drop.index != -2)
                    HomeClick(ToDip(lp));  // -2: перенос брошен мимо очереди
            } else
                view_.OnMouseUp(ToDip(lp), Now());
            MarkDirty();
            Invalidate();
            return 0;
        case WM_RBUTTONUP:
            if (homeMode_) {
                const auto hit = home_.At(ToDip(lp));
                if (hit.action == Home::PlayTrack && hit.index != -1)
                    TrackMenu(hit.index, ToDip(lp));
                else
                    ShowMenu(ToDip(lp));
            } else
                view_.OnRightClick(ToDip(lp));
            return 0;
        case WM_NCRBUTTONUP:
            // Правый клик по заголовку - наше меню вместо системного
            if (wp == HTCAPTION) {
                ShowContextMenu({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                return 0;
            }
            break;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const float k = 96.f / dpi_;
            if (homeMode_)
                home_.Scroll(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA);
            else
                view_.OnWheel({static_cast<float>(pt.x) * k, static_cast<float>(pt.y) * k},
                              static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA, Now());
            MarkDirty();
            Invalidate();
            return 0;
        }
        case WM_KEYDOWN: {
            // Сначала - назначение новой клавиши, потом настроенные сочетания
            if (CaptureShortcut((UINT)wp)) return 0;
            if (homeMode_ && home_.StoryOpen() && (wp == VK_ESCAPE || wp == VK_LEFT || wp == VK_RIGHT)) {
                StoryCommand(wp == VK_ESCAPE ? 202 : wp == VK_LEFT ? -1 : 1);
                return 0;
            }
            if (homeMode_ && paletteOpen_ && wp == VK_ESCAPE) {
                ClosePalette();
                return 0;
            }
            // Клавиатура на главной: Tab ходит по элементам, Enter нажимает
            if (homeMode_ && wp == VK_TAB) {
                const auto label = home_.MoveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
                narrator_.Announce(label);
                Invalidate();
                return 0;
            }
            if (homeMode_ && wp == VK_RETURN) {
                view::Point center;
                if (home_.FocusedPoint(center)) {
                    HomeClick(center);
                    return 0;
                }
            }
            if (RunShortcut((UINT)wp, false)) return 0;
            if (homeMode_ && wp == VK_ESCAPE && home_.NowPlayingOpen()) {
                home_.ToggleNowPlaying();
                Invalidate();
                return 0;
            }
            if (homeMode_ && wp == VK_ESCAPE) {
                SetWindowTextW(homeSearch_, L"");
                SetFocus(hwnd_);
                return 0;
            }
            const pm::PlayerState ps = player_.GetState();
            switch (wp) {
                case VK_SPACE:
                    TogglePlay();
                    break;
                case VK_LEFT:
                    Seek(std::max(0.0, ps.position - 5));
                    break;
                case VK_RIGHT:
                    Seek(ps.position + 5);
                    break;
                case VK_UP:
                    SetVolume(std::min(1.f, ps.volume + 0.05f));
                    break;
                case VK_DOWN:
                    SetVolume(std::max(0.f, ps.volume - 0.05f));
                    break;
                case 'M':
                    ToggleMute();
                    break;
                case 'L':
                    if (homeMode_)
                        home_.Navigate(view::HomeModel::Queue);
                    else
                        view_.TogglePlaylist(Now());
                    break;
                case 'O':
                    if (GetKeyState(VK_CONTROL) < 0) OpenFiles(false);
                    break;
                default:
                    break;
            }
            MarkDirty();
            Invalidate();
            return 0;
        }
        case WM_HOTKEY:
            switch (static_cast<int>(wp)) {
                case kHotPlay:
                    HandleMediaCommand(APPCOMMAND_MEDIA_PLAY_PAUSE);
                    break;
                case kHotNext:
                    HandleMediaCommand(APPCOMMAND_MEDIA_NEXTTRACK);
                    break;
                case kHotPrev:
                    HandleMediaCommand(APPCOMMAND_MEDIA_PREVIOUSTRACK);
                    break;
                case kHotStop:
                    HandleMediaCommand(APPCOMMAND_MEDIA_STOP);
                    break;
                default:
                    break;
            }
            return 0;
        case WM_APPCOMMAND: {
            const int cmd = GET_APPCOMMAND_LPARAM(lp);
            if (cmd == APPCOMMAND_MEDIA_PLAY_PAUSE || cmd == APPCOMMAND_MEDIA_PLAY || cmd == APPCOMMAND_MEDIA_PAUSE ||
                cmd == APPCOMMAND_MEDIA_STOP || cmd == APPCOMMAND_MEDIA_NEXTTRACK ||
                cmd == APPCOMMAND_MEDIA_PREVIOUSTRACK) {
                HandleMediaCommand(cmd);
                return TRUE;
            }
            break;
        }
        case WM_DROPFILES:
            LoadDropped(reinterpret_cast<HDROP>(wp));
            return 0;
        case WM_TIMER:
            if (wp == kMoveFrame && started_) RenderFrame();
            if (wp == kGraphicsRetry) {
                KillTimer(hwnd_, kGraphicsRetry);
                Invalidate();
            }
            if (wp == kTickTimer) Invalidate();
            if (wp == kSaveTimer) {
                KillTimer(hwnd_, kSaveTimer);
                SaveCurrentState();
            }
            if (wp == kSaveTick) SaveCurrentState();
            if (wp == kLazyTimer) {
                KillTimer(hwnd_, kLazyTimer);
                Invalidate();
            }
            if (wp == kCloseFade) {
                KillTimer(hwnd_, kCloseFade);
                renderer_.PlayExit(Entrance::Pop, .16);
                SetTimer(hwnd_, kCloseDone, 170, nullptr);
            }
            if (wp == kCloseDone) {
                KillTimer(hwnd_, kCloseDone);
                if (hideWhenClosed_)
                    FinishHide();
                else
                    DestroyWindow(hwnd_);
            }
            if (wp == kModeSwitch) {
                KillTimer(hwnd_, kModeSwitch);
                modeSwitchPending_ = false;
                ToggleHome();
            }
            return 0;
        case kLyricsMessage:
            OnLyricsReady();
            return 0;
        case kSmtcMessage:
            switch (wp) {
                case Smtc::kPlay:
                    player_.Play();
                    break;
                case Smtc::kPause:
                case Smtc::kStop:
                    player_.Pause();
                    break;
                case Smtc::kNext:
                    Next();
                    break;
                case Smtc::kPrevious:
                    Prev();
                    break;
                case Smtc::kSeek:
                    Seek(static_cast<double>(lp) / 1000.0);
                    break;
                default:
                    break;
            }
            Invalidate();
            return 0;
        case kPluginMessage:
            plugins_.RunPosted();
            SyncPages();
            Invalidate();
            return 0;
        case WM_COMMAND:
            if (((HWND)lp == downloadEdit_ || (HWND)lp == paletteEdit_) && HIWORD(wp) == EN_CHANGE) {
                if ((HWND)lp == paletteEdit_) paletteSelected_ = 0;
                Invalidate();
                return 0;
            }
            if ((HWND)lp == settingsEdit_ && HIWORD(wp) == EN_CHANGE) {
                home_.ResetScroll();
                Invalidate();
                return 0;
            }
            if ((HWND)lp == homeSearch_ && HIWORD(wp) == EN_CHANGE) {
                wchar_t query[181]{};
                GetWindowTextW(homeSearch_, query, 181);
                home_.model.query = query;
                home_.ResetScroll();
                Invalidate();
                return 0;
            }
            // Кнопки в превью окна на панели задач
            if (HIWORD(wp) == THBN_CLICKED) {
                switch (LOWORD(wp)) {
                    case Taskbar::kPrev:
                        Prev();
                        break;
                    case Taskbar::kPlayPause:
                        TogglePlay();
                        break;
                    case Taskbar::kNext:
                        Next();
                        break;
                    default:
                        break;
                }
                MarkDirty();
                return 0;
            }
            break;
        case WM_COPYDATA:
            HandleCopyData(reinterpret_cast<const COPYDATASTRUCT*>(lp));
            return TRUE;
        case WM_MOVING:
            SnapToEdges(reinterpret_cast<RECT*>(lp));
            return TRUE;
        case WM_ENTERSIZEMOVE:
            SetTimer(hwnd_, kMoveFrame, 33, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            KillTimer(hwnd_, kMoveFrame);
            if (homeMode_ && !IsZoomed(hwnd_)) {
                homeSize_ = ClientDip();
                SaveHomePreferences();
            }
            MarkDirty();  // новое место окна
            return 0;
        case kLibraryMessage:
            OnLibraryScanned();
            return 0;
        case kWatchMessage:
            StartLibraryScan();
            return 0;
        case kWakeMessage:
            wakePosted_ = false;
            player_.PumpEvents();
            Invalidate();
            return 0;
        case WM_SETTINGCHANGE: {
            const bool before = systemReduce_;
            ReadSystemReduce();
            if (before != systemReduce_) ApplyTheme(theme_, false);
            break;
        }
        case WM_ACTIVATE:
            // Вернулись из папки плагинов - перечитать список
            if (LOWORD(wp) != WA_INACTIVE && pluginsRefreshPending_) {
                pluginsRefreshPending_ = false;
                plugins_.Discover({exeDir_ / L"plugins", UserPluginsDir()});
                RebuildPluginRows();
            }
            // Скопировали ссылку в браузере и вернулись - она уже в поле загрузки
            if (LOWORD(wp) != WA_INACTIVE) PrefillDownload();
            Invalidate();
            break;
        case WM_ENDSESSION:
            if (wp) {
                SaveCurrentState();
                library_.Save(libraryPath_);
            }
            return 0;
        case WM_CLOSE:
            if (closing_) {
                DestroyWindow(hwnd_);  // второй запрос не ждёт анимации
                return 0;
            }
            SaveCurrentState();
            library_.Save(libraryPath_);
            if (introEnabled_ && MotionAllowed() && visible_ && !IsIconic(hwnd_) && IsWindowVisible(hwnd_)) {
                // Окно сжимается в круг вокруг луны, луна гаснет, затем окно исчезает
                closing_ = true;
                StartCloseAnimation();
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            if (homeSearchFont_) {
                DeleteObject(homeSearchFont_);
                homeSearchFont_ = nullptr;
            }
            for (int id = kHotPlay; id <= kHotStop; ++id) UnregisterHotKey(hwnd_, id);
            RemoveTrayIcon();
            narrator_.Detach();
            KillTimer(hwnd_, kTickTimer);
            KillTimer(hwnd_, kSaveTimer);
            KillTimer(hwnd_, kSaveTick);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------- один экземпляр ----------------

bool App::ForwardToRunningInstance() {
    // Первый экземпляр мог ещё не создать окно - ждём до двух секунд
    HWND other = nullptr;
    for (int i = 0; i < 40 && !other; ++i) {
        other = FindWindowW(kClassName, nullptr);
        if (!other) Sleep(50);
    }
    if (!other) return false;
    // Пути через '\n' (в путях Windows его не бывает); пустой список - просто показать окно
    std::wstring data;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (!data.empty()) data += L'\n';
            data += argv[i];
        }
        LocalFree(argv);
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);  // чтобы окно могло выйти на передний план
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataPaths;
    cds.cbData = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    cds.lpData = data.data();
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000,
                               &result) != 0;
}

void App::HandleCopyData(const COPYDATASTRUCT* cds) {
    if (!cds || cds->dwData != kCopyDataPaths || cds->cbData < sizeof(wchar_t)) return;
    const std::wstring data(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t) - 1);
    std::vector<std::filesystem::path> paths;
    size_t start = 0;
    while (start < data.size()) {
        const size_t end = std::min(data.find(L'\n', start), data.size());
        if (end > start) paths.emplace_back(data.substr(start, end - start));
        start = end + 1;
    }
    // A shell launcher can start with SW_HIDE; such a window is not iconic.
    // Reopening must make it visible in either case, without replacing its queue.
    if (hidden_ || closing_) {
        ShowFromBackground();
    } else {
        ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
    if (paths.empty()) return;
    // Проводник запускает по экземпляру на каждый выделенный файл: файлы, пришедшие подряд
    // (меньше чем через 1.5 с), дописываются к только что открытым
    const double now = Now();
    const bool append = now - lastCopyData_ < 1.5;
    lastCopyData_ = now;
    LoadPaths(paths, append);
}

// ---------------- библиотека ----------------

void App::InitLibrary(bool firstRun) {
    libraryPath_ = statePath_.parent_path() / L"library.json";
    const bool loaded = library_.Load(libraryPath_);
    if (!loaded || (firstRun && library_.Folders().empty())) {
        // Первый запуск: стандартные папки, где обычно лежит музыка
        auto known = [](REFKNOWNFOLDERID id) {
            std::filesystem::path p;
            PWSTR s = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &s))) p = s;
            CoTaskMemFree(s);
            return p;
        };
        std::vector<std::filesystem::path> candidates = {known(FOLDERID_Music), known(FOLDERID_Downloads)};
        const std::filesystem::path oneDrive = known(FOLDERID_SkyDrive);
        if (!oneDrive.empty()) {
            candidates.push_back(oneDrive / L"Music");
            candidates.push_back(oneDrive / L"Музыка");
        }
        for (const auto& c : candidates) {
            std::error_code ec;
            if (!c.empty() && std::filesystem::is_directory(c, ec)) library_.AddFolder(c);
        }
        library_.Save(libraryPath_);
    }
    // Plugins start before the library is loaded; merge their folders only after loading,
    // otherwise FromJson would silently replace those additions.
    bool addedPluginFolder = false;
    for (const auto& folder : pendingPluginFolders_)
        addedPluginFolder = library_.AddFolder(folder) || addedPluginFolder;
    pendingPluginFolders_.clear();
    if (addedPluginFolder) library_.Save(libraryPath_);
    // Плейлист пуст, а библиотека уже знает треки (кэш прошлого запуска) - сразу показываем их
    if (player_.GetPlaylist().Empty() && !library_.Tracks().empty()) LoadLibraryPlaylist(false);
    RestartWatcher();
    if (library_.AutoScan()) StartLibraryScan();
}

void App::RestartWatcher() {
    watcher_.Stop();
    if (library_.AutoScan()) watcher_.Start(library_.Folders(), hwnd_, kWatchMessage);
}

void App::StartLibraryScan() {
    if (library_.Folders().empty()) return;
    HWND hwnd = hwnd_;
    // Уже идёт - повторим по окончании (изменения могли прийти посреди обхода)
    if (!scanner_.Start(
            library_.Folders(), library_.Tracks(), [hwnd] { PostMessageW(hwnd, kLibraryMessage, 0, 0); },
            library_.HideDuplicates()))
        rescanPending_ = true;
    Invalidate();
}

void App::OnLibraryScanned() {
    std::vector<pm::LibraryTrack> tracks;
    if (!scanner_.Take(tracks)) return;
    // Новые для библиотеки файлы (не просто отсутствующие в плейлисте: убранное руками не возвращается)
    std::set<std::filesystem::path> known;
    for (const auto& t : library_.Tracks()) known.insert(t.path);
    std::vector<pm::PlaylistEntry> fresh;
    for (const auto& t : tracks)
        if (!known.count(t.path) && !InPlaylist(t.path)) fresh.push_back({t.path, t.meta});
    const bool wasEmpty = library_.Tracks().empty();
    library_.SetTracks(std::move(tracks));
    RefreshHome();
    library_.Save(libraryPath_);

    if (player_.GetPlaylist().Empty() && !library_.Tracks().empty()) {
        LoadLibraryPlaylist(false);
    } else if (playlistFromLibrary_ && !fresh.empty() && !wasEmpty) {
        player_.LoadTracks(fresh, true);
        RebuildRows();
        view_.ShowToast(fresh.size() == 1 ? "Новый трек в библиотеке"
                                          : "Новых треков в библиотеке: " + std::to_string(fresh.size()),
                        Now());
        MarkDirty();
    }
    if (rescanPending_) {
        rescanPending_ = false;
        StartLibraryScan();
    }
    Invalidate();
}

void App::LoadLibraryPlaylist(bool play) {
    if (library_.Tracks().empty()) return;
    player_.LoadTracks(library_.Entries());
    playlistFromLibrary_ = true;
    waveMode_ = false;
    RebuildRows();
    if (play)
        player_.PlayIndex(0);
    else
        player_.OpenAt(0, 0, false);
    MarkDirty();
    Invalidate();
}

void App::AddLibraryFolder() {
    auto paths = PickPaths(hwnd_, true, false, nullptr, nullptr);
    if (paths.empty()) return;
    if (library_.AddFolder(paths[0])) {
        library_.Save(libraryPath_);
        RestartWatcher();
        StartLibraryScan();
    }
}

void App::LearnFolders(const std::vector<std::filesystem::path>& paths) {
    bool changed = false;
    for (const auto& p : paths) changed = library_.LearnFrom(p) || changed;
    if (!changed) return;
    library_.Save(libraryPath_);
    RestartWatcher();
    StartLibraryScan();
}

// ---------------- плагины ----------------

std::filesystem::path App::UserPluginsDir() const {
    // Рядом с exe, если туда можно писать (там же state.json), иначе в %APPDATA%\PlayerMoon
    return statePath_.parent_path() / L"plugins";
}

void App::InitPlugins(const std::set<std::string>& enabled) {
    plugins_.SetDataRoot(statePath_.parent_path() / L"plugins-data");
    pm::PluginHostFunctions host;
    host.playback = [this] {
        const pm::PlayerState ps = player_.GetState();
        return PmPlayback{ps.playing ? 1 : 0, ps.index, ps.count, ps.position, ps.duration, ps.volume};
    };
    host.command = [this](int cmd) {
        switch (cmd) {
            case PM_CMD_TOGGLE:
                player_.Toggle();
                break;
            case PM_CMD_PLAY:
                player_.Play();
                break;
            case PM_CMD_PAUSE:
                player_.Pause();
                break;
            case PM_CMD_NEXT:
                player_.Next();
                break;
            case PM_CMD_PREV:
                player_.Prev();
                break;
            default:
                break;
        }
        Invalidate();
    };
    host.seek = [this](double s) {
        player_.Seek(s);
        Invalidate();
    };
    // Из потоков плагинов: только разбудить окно
    HWND hwnd = hwnd_;
    host.wake = [hwnd] { PostMessageW(hwnd, kPluginMessage, 0, 0); };
    host.pageShow = [this](size_t page) {
        SyncPages();
        if (page < plugins_.Pages().size()) {
            const auto plugin = plugins_.Pages()[page].plugin;
            if (plugin < plugins_.Plugins().size() && plugins_.Plugins()[plugin].id == "downloader") {
                OpenWorkspace(view::HomeModel::Downloads);
                return;
            }
        }
        if (homeMode_) ToggleHome();
        view_.ShowPanelTab(view_.PageTab(static_cast<int>(page)), Now());
        Invalidate();
    };
    host.toast = [this](const std::string& t) {
        workspaceNotice_ = ::text::Utf8ToWide(t);
        view_.ShowToast(t, Now());
        Invalidate();
    };
    host.clipboardText = [this] { return submittedDownload_.empty() ? ClipboardText(hwnd_) : submittedDownload_; };
    host.openPath = [this](const std::string& p) {
        ShellExecuteW(hwnd_, L"open", ::text::Utf8ToWide(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    host.addFiles = [this](const std::vector<std::string>& paths, bool play) { AddFilesFromPlugin(paths, play); };
    host.addLibraryFolder = [this](const std::string& p) {
        std::error_code ec;
        const std::filesystem::path dir = ::text::Utf8ToPath(p);
        std::filesystem::create_directories(dir, ec);
        if (libraryPath_.empty()) {
            pendingPluginFolders_.push_back(dir);
            return;
        }
        if (library_.AddFolder(dir)) {
            library_.Save(libraryPath_);
            if (started_) {
                RestartWatcher();
                StartLibraryScan();
            }
        }
    };
    host.pickFolder = [this](const std::string& title) {
        auto paths = PickPaths(hwnd_, true, false, nullptr, nullptr, ::text::Utf8ToWide(title).c_str());
        return paths.empty() ? std::string() : ::text::PathToUtf8(paths[0]);
    };
    plugins_.SetHost(host);
    plugins_.Discover({exeDir_ / L"plugins", UserPluginsDir()});
    plugins_.EnableSaved(enabled);
    RebuildPluginRows();
    SyncPages();
}

void App::RebuildPluginRows() {
    pluginRows_.clear();
    std::vector<bool> on;
    int enabled = 0;
    for (const auto& p : plugins_.Plugins()) {
        pluginRows_.push_back({::text::Utf8ToWide(p.name), ::text::Utf8ToWide(p.version),
                               ::text::Utf8ToWide(p.description), ::text::Utf8ToWide(p.error)});
        on.push_back(p.enabled);
        enabled += p.enabled ? 1 : 0;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"включено %d из %d", enabled, static_cast<int>(pluginRows_.size()));
    pluginCountText_ = buf;
    view_.SetPlugins(on, Now());
    SyncPages();
    Invalidate();
}

void App::TogglePlugin(int index) {
    const auto& list = plugins_.Plugins();
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    const pm::PluginInfo& p = list[static_cast<size_t>(index)];
    if (!p.enabled) {
        // Плагин - чужой код внутри плеера: включение только с явного согласия
        const std::wstring q = L"Включить плагин «" + ::text::Utf8ToWide(p.name) +
                               L"»?\n\nПлагин работает внутри PlayerMoon с правами вашей учётной записи. "
                               L"Включайте только плагины, которым доверяете.";
        if (MessageBoxW(hwnd_, q.c_str(), L"Плагины", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    const bool wasEnabled = p.enabled;
    const std::string name = p.name;
    if (!plugins_.SetEnabled(static_cast<size_t>(index), !wasEnabled) && !wasEnabled)
        view_.ShowToast("Плагин «" + name + "» не запустился", Now());
    RebuildPluginRows();
    MarkDirty();
}

bool App::EnableDownloaderPlugin() {
    const auto& list = plugins_.Plugins();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].id != "downloader") continue;
        if (list[i].enabled) return list[i].loaded;
        // Штатный загрузчик лежит рядом с exe и ставится вместе с плеером: нажатие «Скачать» и есть согласие.
        // Чужая копия из пользовательской папки включается только через обычный вопрос.
        std::error_code ec;
        const auto bundled = std::filesystem::weakly_canonical(exeDir_ / L"plugins", ec);
        const auto dir = std::filesystem::weakly_canonical(list[i].dir, ec);
        if (!ec && dir.parent_path() == bundled) {
            if (!plugins_.SetEnabled(i, true)) view_.ShowToast("Загрузчик не запустился", Now());
            RebuildPluginRows();
            MarkDirty();
        } else {
            TogglePlugin(static_cast<int>(i));
        }
        return plugins_.Plugins()[i].enabled && plugins_.Plugins()[i].loaded;
    }
    return false;
}

void App::PrefillDownload() {
    if (!homeMode_ || home_.model.page != view::HomeModel::Downloads || !downloadEdit_) return;
    if (GetWindowTextLengthW(downloadEdit_) > 0) return;
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == prefillClipboard_) return;
    prefillClipboard_ = sequence;
    std::string text = ClipboardText(hwnd_);
    const size_t at = text.find("http");
    if (at == std::string::npos || (text.compare(at, 7, "http://") != 0 && text.compare(at, 8, "https://") != 0))
        return;
    text = text.substr(at, text.find_first_of(" \t\r\n\"'<>", at) - at);
    SetWindowTextW(downloadEdit_, ::text::Utf8ToWide(text).c_str());
    SendMessageW(downloadEdit_, EM_SETSEL, 0, -1);
    workspaceNotice_ = L"Ссылка из буфера обмена уже в поле - нажмите «Скачать».";
    Invalidate();
}

void App::OpenPluginsFolder() {
    const std::filesystem::path dir = UserPluginsDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    pluginsRefreshPending_ = true;
}

void App::NotifyPluginsTrack() {
    const pm::NowPlaying& np = player_.Current();
    if (np.index < 0 || !player_.GetState().hasTrack) {
        plugins_.NotifyTrack(nullptr);
        return;
    }
    // Строки живут до конца вызова - как требует интерфейс плагинов
    const std::string path = ::text::PathToUtf8(np.path);
    PmTrack t{path.c_str(), np.title.c_str(), np.tags.artist.c_str(), np.tags.album.c_str(),
              player_.GetState().duration};
    plugins_.NotifyTrack(&t);
}

}  // namespace ui
