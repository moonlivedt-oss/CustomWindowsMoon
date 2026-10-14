#include "desk/frame.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

#include "core/config.h"
#include "core/fx.h"
#include "gfx/draw.h"

namespace {

constexpr wchar_t kClass[] = L"MoonRadialFrame";
constexpr UINT_PTR kAnimTimer = 1, kSweepTimer = 2, kRetryTimer = 3;
constexpr UINT WM_FRAME_FG = WM_APP + 1;
constexpr DWORD kBorder = 34, kCaption = 35, kText = 36;  // DWMWA_BORDER_COLOR, CAPTION_COLOR, TEXT_COLOR
constexpr DWORD kDefault = 0xFFFFFFFF;                    // DWMWA_COLOR_DEFAULT
constexpr double kFlashMs = 140, kSettleMs = 420, kFadeMs = 260;

WindowFrame* g_self = nullptr;

COLORREF Mix(COLORREF a, COLORREF b, double t) {
    t = Clamp01(t);
    auto ch = [&](int x, int y) { return (BYTE)std::lround(x + (y - x) * t); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

void Set(HWND w, DWORD attr, COLORREF c) {
    DWORD v = c;
    DwmSetWindowAttribute(w, attr, &v, sizeof(v));
}

}  // namespace

void WindowFrame::Create(HINSTANCE inst) {
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
}

void WindowFrame::Apply(const Config& cfg) {
    bool wasActive = Active();
    theme_ = CurrentTheme(cfg);
    // если что-то выключили - вернуть окнам их цвета, потом раскрасить заново по новым настройкам
    if (wasActive) ResetAll();
    frame_ = cfg.windowFrame;
    captions_ = cfg.windowCaptions;
    accent_ = theme_.ac;
    flash_ = Mix(theme_.ac, RGB(255, 255, 255), 0.45);  // вспышка при переключении
    idle_ = Mix(theme_.panel, theme_.muted, 0.35);      // сталь неактивных окон
    caption_ = Mix(theme_.bg, theme_.panel, 0.7);
    captionIdle_ = theme_.bg;
    text_ = theme_.fg;
    textIdle_ = Mix(theme_.muted, theme_.bg, 0.25);
    Hook(Active());
    if (Active()) {
        active_ = nullptr;
        Sweep();
        Foreground(GetForegroundWindow());
    }
}

void WindowFrame::Suspend(bool on) {
    if (on == suspended_) return;
    bool was = Active();
    suspended_ = on;
    if (was && !Active()) ResetAll();
    Hook(Active());
    if (Active()) {
        active_ = nullptr;
        Sweep();
        Foreground(GetForegroundWindow());
    }
}

void WindowFrame::Destroy() {
    Hook(false);
    ResetAll();
}

void WindowFrame::Hook(bool on) {
    if (on && !hook_) {
        hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnEvent, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        SetTimer(wnd_, kSweepTimer, 3000, nullptr);
    } else if (!on && hook_) {
        UnhookWinEvent(hook_);
        hook_ = nullptr;
        KillTimer(wnd_, kSweepTimer);
        KillTimer(wnd_, kAnimTimer);
        animating_ = false;
    }
}

void CALLBACK WindowFrame::OnEvent(HWINEVENTHOOK, DWORD, HWND w, LONG obj, LONG, DWORD, DWORD) {
    if (g_self && obj == OBJID_WINDOW) PostMessageW(g_self->wnd_, WM_FRAME_FG, (WPARAM)w, 0);
}

LRESULT CALLBACK WindowFrame::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_self && msg == WM_FRAME_FG) {
        g_self->Foreground((HWND)wp);
        return 0;
    }
    if (g_self && msg == WM_TIMER) {
        if (wp == kAnimTimer) g_self->Tick();
        if (wp == kSweepTimer) g_self->Sweep();
        if (wp == kRetryTimer) {
            KillTimer(h, kRetryTimer);
            g_self->Foreground(GetForegroundWindow());
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool WindowFrame::Eligible(HWND w) const {
    if (!w || !IsWindow(w) || !IsWindowVisible(w) || GetAncestor(w, GA_ROOT) != w) return false;
    LONG_PTR st = GetWindowLongPtrW(w, GWL_STYLE), ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    if (!(st & WS_THICKFRAME) && (st & WS_CAPTION) != WS_CAPTION) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcsncmp(cls, L"MoonRadial", 10) ||
        !wcscmp(cls, L"Windows.UI.Core.CoreWindow"))
        return false;
    return true;
}

void WindowFrame::Paint(HWND w, bool active, COLORREF border) {
    bool own = skip && skip(w);  // закреплённое окно: рамку держит WindowFx
    if (frame_ && !own) Set(w, kBorder, border);
    if (captions_) {
        Set(w, kCaption, active ? caption_ : captionIdle_);
        Set(w, kText, active ? text_ : textIdle_);
    }
    touched_.insert(w);
}

void WindowFrame::Sweep() {
    if (!Active()) return;
    for (auto it = touched_.begin(); it != touched_.end();) it = IsWindow(*it) ? std::next(it) : touched_.erase(it);
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            auto* self = reinterpret_cast<WindowFrame*>(lp);
            if (!self->touched_.count(w) && self->Eligible(w)) self->Paint(w, w == self->active_, self->idle_);
            return TRUE;
        },
        (LPARAM)this);
    HWND fg = GetForegroundWindow();
    if (fg && GetAncestor(fg, GA_ROOT) != active_) Foreground(fg);
}

void WindowFrame::Foreground(HWND w) {
    if (!Active()) return;
    w = w ? GetAncestor(w, GA_ROOT) : nullptr;
    if (w == active_) return;
    if (!Eligible(w)) {
        // новое окно становится передним раньше, чем видимым: посмотреть ещё раз чуть позже
        if (w && IsWindow(w) && !IsWindowVisible(w) && retries_++ < 8) SetTimer(wnd_, kRetryTimer, 80, nullptr);
        // впереди стол, Пуск или наше окно: прежнее активное просто гаснет
        if (active_ && IsWindow(active_)) {
            prev_ = active_;
            active_ = nullptr;
            t0_ = GetTickCount64();
            if (!animating_) SetTimer(wnd_, kAnimTimer, 15, nullptr);
            animating_ = true;
        }
        return;
    }
    retries_ = 0;
    if (active_ && active_ != w && IsWindow(active_)) {
        if (prev_ && prev_ != w && IsWindow(prev_)) Paint(prev_, false, idle_);  // не дождалось конца угасания
        prev_ = active_;
    }
    active_ = w;
    t0_ = GetTickCount64();
    Tick();
    if (animating_ || fx::Game()) return;
    animating_ = true;
    SetTimer(wnd_, kAnimTimer, 15, nullptr);
}

void WindowFrame::Tick() {
    double el = (double)(GetTickCount64() - t0_);
    double flash = fx::Dur(kFlashMs), settle = fx::Dur(kSettleMs), fade = fx::Dur(kFadeMs);
    if (active_ && IsWindow(active_)) {
        COLORREF c = el < flash ? Mix(idle_, flash_, EaseOut(el / flash))
                                : Mix(flash_, accent_, EaseOut((el - flash) / std::max(1.0, settle - flash)));
        Paint(active_, true, c);
    }
    if (prev_ && IsWindow(prev_)) {
        Paint(prev_, false, Mix(accent_, idle_, EaseOut(el / fade)));
        if (el >= fade) prev_ = nullptr;
    }
    if (el >= std::max(settle, fade)) {
        KillTimer(wnd_, kAnimTimer);
        animating_ = false;
        prev_ = nullptr;
    }
}

void WindowFrame::ResetAll() {
    KillTimer(wnd_, kAnimTimer);
    animating_ = false;
    for (HWND w : touched_) {
        if (!IsWindow(w)) continue;
        if (!(skip && skip(w))) Set(w, kBorder, kDefault);
        Set(w, kCaption, kDefault);
        Set(w, kText, kDefault);
    }
    touched_.clear();
    active_ = prev_ = nullptr;
}
