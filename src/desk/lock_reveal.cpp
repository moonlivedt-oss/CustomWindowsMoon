// Переход между экраном блокировки и столом: окно поверх всего, таймеры, сон и пробуждение.
#include "desk/lockscreen.h"

#include <algorithm>
#include <cstring>

#include "core/config.h"
#include "core/files.h"
#include "core/fx.h"
#include "desk/lock_internal.h"
#include "desk/whreg.h"
#include "gfx/draw.h"
#include "gfx/sound.h"

namespace lockscreen {

using namespace detail;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialLockReveal";
constexpr UINT_PTR kTimer = 1, kWakeTimer = 2, kInputTimer = 3;
constexpr double kCurtainMs = 300, kFadeMs = 720, kFadeDelayMs = 120;
constexpr double kPortalMs = 1900, kPortalCloseMs = 950;

Reveal* g_reveal = nullptr;

}  // namespace

void Reveal::Create(HINSTANCE inst, HWND sessionWnd) {
    g_reveal = this;
    session_ = sessionWnd;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
}

void Reveal::Destroy() {
    Hide();
    if (wnd_) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

void Reveal::Apply(const Config& cfg) {
    enabled_ = cfg.lockReveal;
    portal_ = cfg.lockPortal;
    pattern_ = cfg.lockPattern;
    bootPortal_ = cfg.bootPortal;
    wakePortal_ = cfg.wakePortal;
    sounds_ = cfg.sounds;
    theme_ = CurrentTheme(cfg);
}

LRESULT CALLBACK Reveal::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_TIMER && wp == kTimer && g_reveal) {
        g_reveal->Tick();
        return 0;
    }
    if (msg == WM_TIMER && wp == kWakeTimer && g_reveal) {
        KillTimer(h, kWakeTimer);
        if (g_reveal->state_ == HOLD && g_reveal->waitWake_ && !g_reveal->locked_) g_reveal->Open();
        return 0;
    }
    if (msg == WM_TIMER && wp == kInputTimer && g_reveal) {
        // запасной путь: экран мог включиться без уведомления - смотрим, был ли ввод после того, как всё погасло
        LASTINPUTINFO li{sizeof(li)};
        auto& r = *g_reveal;
        if (r.state_ != HOLD || !r.waitWake_) {
            KillTimer(h, kInputTimer);
        } else if (GetLastInputInfo(&li) && li.dwTime > (DWORD)(r.holdAt_ + 1500) && !r.locked_) {
            KillTimer(h, kInputTimer);
            r.Open();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool Reveal::Prepare() {
    if (memDC_) return true;
    mon_ = PrimaryRect();
    int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    std::wstring file = OutputFile();
    if (!Exists(file)) {
        // своей картинки ещё нет - берём ту, что стоит на экране блокировки сейчас
        wchar_t wp[MAX_PATH] = {};
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
        file = wp;
    }
    std::vector<uint32_t> px;
    if (file.empty() || !LoadCover(file, W, H, px)) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    void* bits = nullptr;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_) {
        DeleteDC(memDC_);
        memDC_ = nullptr;
        return false;
    }
    old_ = SelectObject(memDC_, dib_);
    bits_ = (uint32_t*)bits;
    if (whreg::LogonCrisp()) {
        // экран входа показывает картинку как есть - копия такая же, иначе на входе будет скачок
        src_ = std::move(px);
        Plain();
        if (pattern_) BuildCells();
        return true;
    }
    // размытие как у экрана входа: уменьшить, размыть, растянуть, притемнить
    constexpr int kDown = 8;
    int sw = std::max(1, W / kDown), sh = std::max(1, H / kDown);
    std::vector<uint32_t> small((size_t)sw * sh);
    Downscale(px.data(), W, H, small.data(), sw, sh, kDown);
    BoxBlur(small.data(), sw, sh, 4);
    for (auto& p : small) {
        uint32_t r = (p >> 16 & 255) * 72 / 100, g = (p >> 8 & 255) * 72 / 100, b = (p & 255) * 72 / 100;
        p = 0xFF000000u | r << 16 | g << 8 | b;
    }
    src_.resize((size_t)W * H);
    UpscaleBilinear(small.data(), sw, sh, src_.data(), W, H, W);
    Plain();
    if (pattern_) BuildCells();
    return true;
}

void Reveal::Plain() { memcpy(bits_, src_.data(), src_.size() * 4); }
void Reveal::AimAtCursor() {
    POINT pt;
    GetCursorPos(&pt);
    int W = mon_.right - mon_.left, H = mon_.bottom - mon_.top;
    origin_ = {std::clamp((int)(pt.x - mon_.left), 0, W - 1), std::clamp((int)(pt.y - mon_.top), 0, H - 1)};
}

void Reveal::Push(BYTE alpha) {
    HDC scr = GetDC(nullptr);
    POINT pos{mon_.left, mon_.top}, src{0, 0};
    SIZE size{mon_.right - mon_.left, mon_.bottom - mon_.top};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &pos, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Reveal::Open() {
    KillTimer(wnd_, kWakeTimer);
    KillTimer(wnd_, kInputTimer);
    waitWake_ = false;
    AimAtCursor();
    state_ = FADE;
    sounded_ = false;
    t0_ = GetTickCount64();
    SetTimer(wnd_, kTimer, 15, nullptr);
}

void Reveal::Await() {
    state_ = HOLD;
    waitWake_ = true;
    holdAt_ = GetTickCount64();
    KillTimer(wnd_, kTimer);
    SetTimer(wnd_, kInputTimer, 400, nullptr);
}

void Reveal::Intro() {
    if (!enabled_ || !bootPortal_ || fx::Game() || state_ != IDLE || !Prepare()) return;
    AimAtCursor();
    state_ = INTRO;
    t0_ = GetTickCount64();
    Push(0);
    SetTimer(wnd_, kTimer, 15, nullptr);
}

bool Reveal::ScreenOff() {
    if (!enabled_ || !wakePortal_ || fx::Game() || state_ != IDLE || !Prepare()) return false;
    offAfter_ = true;
    state_ = CURTAIN;
    AimAtCursor();
    t0_ = GetTickCount64();
    if (portal_) Portal(1);
    Push(0);
    SetTimer(wnd_, kTimer, 15, nullptr);
    return true;
}

void Reveal::OnDisplay(bool on) {
    // экран включился: чуть подождать - вдруг вместе с ним придёт блокировка
    if (on && state_ == HOLD && waitWake_) SetTimer(wnd_, kWakeTimer, 600, nullptr);
}

void Reveal::OnPower(WPARAM event) {
    if (event == PBT_APMSUSPEND) {
        if (!enabled_ || !wakePortal_ || fx::Game() || state_ != IDLE || !Prepare()) return;
        Push(255);  // перед сном: после пробуждения стол откроется порталом
        Await();
    } else if (event == PBT_APMRESUMEAUTOMATIC || event == PBT_APMRESUMESUSPEND) {
        if (state_ == HOLD && waitWake_) SetTimer(wnd_, kWakeTimer, 900, nullptr);
    }
}

void Reveal::OnSession(WPARAM code) {
    if (code == WTS_SESSION_LOCK) locked_ = true;
    if (code == WTS_SESSION_UNLOCK) locked_ = false;
    if (code == WTS_SESSION_LOCK) {
        if (state_ == HOLD || !enabled_ || fx::Game()) return;
        KillTimer(wnd_, kTimer);
        if (!Prepare()) return;
        Push(255);  // за экраном блокировки: на стол его не видно
        state_ = HOLD;
    } else if (code == WTS_SESSION_UNLOCK) {
        if (state_ == HOLD) Open();
    }
}

void Reveal::LockWithCurtain() {
    if (!enabled_ || fx::Game() || !Prepare()) {
        LockWorkStation();
        return;
    }
    state_ = CURTAIN;
    AimAtCursor();
    t0_ = GetTickCount64();
    if (portal_) Portal(1);
    Push(0);
    SetTimer(wnd_, kTimer, 15, nullptr);
}

void Reveal::Demo() {
    if (state_ != IDLE) return;
    // картинку могли только что пересобрать
    Hide();
    if (!Prepare()) return;
    demo_ = true;
    state_ = CURTAIN;
    AimAtCursor();
    t0_ = GetTickCount64();
    if (portal_) Portal(1);
    Push(0);
    SetTimer(wnd_, kTimer, 15, nullptr);
}

// Под конец раскрытия остатки картинки по углам тают, чтобы не было резкого обрыва.
BYTE Reveal::PortalAlpha(double t) const {
    double op = Clamp01((t - 0.16) / 0.84);
    return (BYTE)(255 * (1 - Clamp01((op - 0.82) / 0.18)));
}

void Reveal::Tick() {
    double el = (double)(LONGLONG)(GetTickCount64() - t0_);  // t0_ бывает в будущем: пауза перед угасанием
    if (state_ == INTRO) {
        // картинка проступает поверх только что показанного стола, потом раскрывается
        double d = fx::Dur(280);
        Push((BYTE)(255 * EaseOut(el / d)));
        if (el >= d) Open();
        return;
    }
    if (state_ == CURTAIN) {
        double d = fx::Dur(portal_ ? kPortalCloseMs : kCurtainMs);
        if (portal_) {
            // портал схлопывается к месту аватара: та же сцена в обратную сторону
            double t = 1 - Clamp01(el / d);
            Portal(t);
            Push(PortalAlpha(t));
        } else {
            Push((BYTE)(255 * EaseOut(el / d)));
        }
        if (el >= d) Plain();
        if (el >= d && offAfter_) {
            offAfter_ = false;
            Await();
            PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
        } else if (el >= d && demo_) {
            demo_ = false;
            state_ = FADE;
            t0_ = GetTickCount64() + 700;  // подержать картинку, как будто вошли
        } else if (el >= d) {
            KillTimer(wnd_, kTimer);
            state_ = HOLD;
            if (!LockWorkStation()) {
                state_ = FADE;  // не заблокировалось - убрать занавес
                t0_ = GetTickCount64();
                SetTimer(wnd_, kTimer, 15, nullptr);
            }
        }
    } else if (state_ == FADE) {
        double d = fx::Dur(portal_ ? kPortalMs : kFadeMs), t = (el - kFadeDelayMs) / d;
        if (t >= 1) {
            Hide();
            return;
        }
        if (portal_) {
            if (t >= 0 && !sounded_) {
                sounded_ = true;
                if (sounds_) Sound::Play(Sound::Portal);
            }
            Portal(std::max(0.0, t));
            Push(PortalAlpha(std::max(0.0, t)));
        } else {
            Push((BYTE)(255 * (1 - EaseOut(std::max(0.0, t)))));
        }
    } else {
        KillTimer(wnd_, kTimer);
    }
}

void Reveal::Hide() {
    if (wnd_) {
        KillTimer(wnd_, kTimer);
        ShowWindow(wnd_, SW_HIDE);
    }
    state_ = IDLE;
    demo_ = false;
    offAfter_ = waitWake_ = false;
    if (wnd_) {
        KillTimer(wnd_, kWakeTimer);
        KillTimer(wnd_, kInputTimer);
    }
    sounded_ = false;
    if (memDC_) {
        SelectObject(memDC_, old_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
        dib_ = nullptr;
    }
    bits_ = nullptr;
    src_.clear();
    src_.shrink_to_fit();
    for (auto* v : {&cellX_, &cellY_, &cellJit_}) std::vector<float>().swap(*v);
    std::vector<uint16_t>().swap(cell_);
    std::vector<uint8_t>().swap(edge_);
    std::vector<int8_t>().swap(tex_);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

}  // namespace lockscreen
