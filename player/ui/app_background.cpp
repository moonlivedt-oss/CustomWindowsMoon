// Работа в фоне: крестик прячет окно, музыка играет дальше. Значок в трее - настоящий выход.
// Карточку у нижнего края экрана показывает RadialMenu: плеер для неё просто источник музыки.
#include "ui/app.h"

#include <shellapi.h>

namespace ui {
namespace {
constexpr UINT kTrayId = 1;
enum TrayCommand : UINT { kTrayShow = 1, kTrayPlay, kTrayNext, kTrayQuit };
}  // namespace

void App::AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon =
        static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcscpy_s(data.szTip, L"PlayerMoon");
    Shell_NotifyIconW(NIM_ADD, &data);
    if (data.hIcon) DestroyIcon(data.hIcon);
}

void App::RemoveTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void App::HideToBackground() {
    if (hidden_ || closing_) return;
    SaveCurrentState();
    library_.Save(libraryPath_);
    if (introEnabled_ && MotionAllowed() && visible_ && !IsIconic(hwnd_) && IsWindowVisible(hwnd_)) {
        // То же закрытие, что и при выходе: окно сжимается вокруг луны; в конце kCloseDone вызовет FinishHide
        closing_ = true;
        hideWhenClosed_ = true;
        StartCloseAnimation();
        return;
    }
    FinishHide();
}

void App::FinishHide() {
    closing_ = false;
    hideWhenClosed_ = false;
    hidden_ = true;
    ShowWindow(hwnd_, SW_HIDE);
    ResetTickTimer();  // частота тиков у видимого и спрятанного окна разная
    Invalidate();
    if (!backgroundHintShown_) {
        backgroundHintShown_ = true;
        SaveHomePreferences();
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = kTrayId;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_NONE;
        wcscpy_s(data.szInfoTitle, L"PlayerMoon работает в фоне");
        wcscpy_s(data.szInfo, L"Окно открывается щелчком по этому значку. Выход - в его меню.");
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    if (showWhenHidden_) {
        showWhenHidden_ = false;
        ShowFromBackground();
    }
}

// Любой показ спрятанного окна (трей, второй запуск, RadialMenu) проходит через WM_SHOWWINDOW
void App::OnShownFromBackground() {
    hidden_ = false;
    ResetTickTimer();
    // Окно поднимается снизу. Сначала PlayEntrance: иначе на экране останется конец анимации ухода
    PlayWindowEntrance(WindowForm::Unknown, windowForm_, Entrance::Rise);
    Invalidate();
}

void App::ShowFromBackground() {
    if (closing_ && hideWhenClosed_) {  // ещё сжимается - покажем, когда спрячется
        showWhenHidden_ = true;
        return;
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    // Фоновому процессу Windows не даёт выйти вперёд. Окно поднимаем само, а фокус просим через очередь ввода активного
    // окна
    const bool topmost = topmostApplied_;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (!topmost) SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    const DWORD self = GetCurrentThreadId();
    const DWORD other = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = other && other != self && AttachThreadInput(self, other, TRUE);
    SetForegroundWindow(hwnd_);
    if (attached) AttachThreadInput(self, other, FALSE);
}

void App::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kTrayShow, L"Открыть PlayerMoon");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto state = player_.GetState();
    AppendMenuW(menu, MF_STRING | (state.hasTrack ? 0 : MF_GRAYED), kTrayPlay, state.playing ? L"Пауза" : L"Играть");
    AppendMenuW(menu, MF_STRING | (state.hasTrack ? 0 : MF_GRAYED), kTrayNext, L"Следующий трек");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayQuit, L"Выход");
    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(hwnd_);  // иначе меню не закроется щелчком мимо
    const UINT command =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (command) {
        case kTrayShow:
            ShowFromBackground();
            break;
        case kTrayPlay:
            TogglePlay();
            MarkDirty();
            break;
        case kTrayNext:
            Next();
            MarkDirty();
            break;
        case kTrayQuit:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

bool App::HandleBackgroundMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SHOWWINDOW && wp && hidden_) OnShownFromBackground();
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE) {  // Alt+F4 и «Закрыть окно» на панели задач
        HideToBackground();
        return true;
    }
    if (msg == kTrayMessage) {
        if (lp == WM_LBUTTONUP)
            ShowFromBackground();
        else if (lp == WM_RBUTTONUP)
            ShowTrayMenu();
        return true;
    }
    if (msg == trayCreatedMsg_ && trayCreatedMsg_) {  // проводник перезапустился
        AddTrayIcon();
        return true;
    }
    return false;
}
}  // namespace ui
