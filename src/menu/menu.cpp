#include "menu/menu.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

#include "core/filedlg.h"
#include "core/files.h"
#include "core/launch.h"
#include "core/perf.h"
#include "core/text.h"
#include "core/themes.h"
#include "core/winlist.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/sound.h"
#include "menu/menu_internal.h"

using namespace Gdiplus;
using namespace menu_detail;

namespace {
RadialMenu* g_self = nullptr;
}  // namespace

RadialMenu::Geo RadialMenu::MakeGeo(double k) {
    Geo g;
    g.k = k;
    g.rHub = 30 * k;
    g.r1 = 44 * k;
    g.r2 = 190 * k;
    g.r3 = 200 * k;
    g.r4 = 340 * k;
    g.iconRoot = (int)(42 * k + 0.5);
    g.iconSub = (int)(34 * k + 0.5);
    g.font = (float)(12 * k);
    g.w = (int)(g.r4 + 30 * k);
    g.h = (int)(2 * (g.r4 + 72 * k));  // сверху и снизу место под плашки
    return g;
}

bool RadialMenu::Create(HINSTANCE inst) {
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"Radial Menu",
                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    drop_ = CreateMenuDrop(this);
    RegisterDragDrop(hwnd_, drop_);
    return true;
}

void RadialMenu::EnsureSurface() {
    if (dib_ && surfW_ == geo_.w && surfH_ == geo_.h) return;
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = geo_.w;
    bi.bmiHeader.biHeight = -geo_.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    memDC_ = CreateCompatibleDC(nullptr);
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    oldBmp_ = SelectObject(memDC_, dib_);
    surfW_ = geo_.w;
    surfH_ = geo_.h;
}

bool RadialMenu::Prewarm(const Config* cfg, UINT dpi, int budgetMs) {
    PERF_SCOPE("menu.Prewarm");
    if (warm_.empty()) {
        const Config* saved = cfg_;
        cfg_ = cfg;  // IconPng смотрит в настройки (общий значок папок)
        Geo g = MakeGeo(dpi / 96.0 * cfg->scale);
        for (const auto* ring : {&cfg->left, &cfg->right}) {
            for (const Item& it : *ring) {
                warm_.push_back({IconPng(it), it.full, g.iconRoot});
                for (const Item& c : it.children) warm_.push_back({IconPng(c), c.full, g.iconSub});
                if (it.expand) {
                    auto list = ListFolder(it.full, it.files);
                    for (size_t i = 0; i < list.size() && (int)i < cfg->maxChildren; ++i)
                        warm_.push_back({IconPng(list[i]), list[i].full, g.iconSub});
                }
            }
        }
        cfg_ = saved;
        warmPos_ = 0;
    }
    ULONGLONG start = GetTickCount64();
    while (warmPos_ < warm_.size() && GetTickCount64() - start < (ULONGLONG)budgetMs) {
        const WarmJob& j = warm_[warmPos_++];
        icons_.Get(j.png, j.shell, j.px);
    }
    return warmPos_ >= warm_.size();
}

void RadialMenu::Show(const Config* cfg, POINT cursor, const RECT& mon, bool leftEdge, UINT dpi, bool demo) {
    PERF_SCOPE("menu.Show");
    demo_ = demo;
    const auto& ring = cfg->RingFor(leftEdge);
    if (ring.empty()) return;
    cfg_ = cfg;
    ring_ = &ring;
    geo_ = MakeGeo(dpi / 96.0 * cfg->scale);
    EnsureSurface();
    left_ = leftEdge;
    full_ = false;
    winMode_ = false;

    // центр держим так, чтобы второе кольцо и плашки помещались по высоте экрана
    int margin = (int)(geo_.r4 + 60 * geo_.k);
    int y = cursor.y;
    int top = mon.top + margin, bottom = mon.bottom - margin;
    y = top <= bottom ? std::clamp(y, top, bottom) : (mon.top + mon.bottom) / 2;
    origin_ = {left_ ? mon.left : mon.right - geo_.w, y - geo_.h / 2};
    center_ = {left_ ? 0 : geo_.w, geo_.h / 2};
    Start();
}

void RadialMenu::ShowAround(const Config* cfg, POINT cursor, const RECT& work, UINT dpi) {
    PERF_SCOPE("menu.ShowAround");
    fullRing_ = cfg->left;
    fullRing_.insert(fullRing_.end(), cfg->right.begin(), cfg->right.end());
    if (fullRing_.empty()) return;
    demo_ = false;
    cfg_ = cfg;
    ring_ = &fullRing_;
    geo_ = MakeGeo(dpi / 96.0 * cfg->scale);
    winMode_ = false;
    PlaceAround(cursor, work);
    Start();
}

void RadialMenu::ShowWindows(const Config* cfg, POINT cursor, const RECT& work, UINT dpi) {
    PERF_SCOPE("menu.ShowWindows");
    winRing_ = winlist::Build(16);
    if (winRing_.empty()) return;
    demo_ = false;
    cfg_ = cfg;
    ring_ = &winRing_;
    geo_ = MakeGeo(dpi / 96.0 * cfg->scale);
    // середина отдана под миниатюру окна: ступица шире, кольца отодвинуты наружу
    double k = geo_.k;
    geo_.rHub = 112 * k;
    geo_.r1 = 124 * k;
    geo_.r2 = 232 * k;
    geo_.r3 = 242 * k;
    geo_.r4 = 372 * k;
    geo_.h = (int)(2 * (geo_.r4 + 72 * k));
    winMode_ = true;
    PlaceAround(cursor, work);
    Start();
}

void RadialMenu::PlaceAround(POINT cursor, const RECT& work) {
    geo_.w = geo_.h;  // квадрат: кольцо целиком, сверху и снизу место под плашки
    EnsureSurface();
    left_ = true;
    full_ = true;
    // центр - у курсора, но так, чтобы кольцо со вторым ярусом не вылезло за экран
    int half = geo_.w / 2, reach = (int)(geo_.r4 + 20 * geo_.k);
    int cx =
        std::clamp((int)cursor.x, (int)work.left + reach, std::max((int)work.left + reach, (int)work.right - reach));
    int cy =
        std::clamp((int)cursor.y, (int)work.top + reach, std::max((int)work.top + reach, (int)work.bottom - reach));
    origin_ = {cx - half, cy - half};
    center_ = {half, half};
}

void RadialMenu::Start() {
    const auto& ring = *ring_;
    mode_ = Mode::None;
    hoverRoot_ = hoverSub_ = subParent_ = pendingRoot_ = -1;
    hlRoot_.assign(ring.size(), 0.0);
    hlSub_.clear();
    subItems_.clear();
    subOwned_.clear();
    navStack_.clear();
    page_ = 0;
    pageCount_ = 1;
    query_.clear();
    searchPool_.clear();
    poolBuilt_ = false;
    kbSel_ = 0;
    lastHoverKey_.clear();
    dragging_ = false;
    std::fill(std::begin(swallowed_), std::end(swallowed_), false);
    shift_ = GetAsyncKeyState(VK_SHIFT) & 0x8000;
    ctrl_ = GetAsyncKeyState(VK_CONTROL) & 0x8000;
    // меню не забирает фокус, поэтому впереди всё ещё окно пользователя
    HWND fg = GetForegroundWindow();
    dialog_ = !demo_ && !winMode_ && filedlg::Is(fg) ? fg : nullptr;

    open_ = true;
    closing_ = false;
    altWasDown_ = GetAsyncKeyState(VK_MENU) & 0x8000;
    tOpen_ = tLast_ = GetTickCount64();
    subSlide_ = 0;

    CaptureBackdrop();  // до показа окна, чтобы в снимок не попало само меню
    EnsureHexGlass();
    Render();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(hwnd_, kTimer, 10, nullptr);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    keyHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    if (cfg_->sounds) Sound::Play(Sound::Open);
}

void RadialMenu::Close() {
    if (!open_ || closing_) return;
    closing_ = true;
    tClose_ = GetTickCount64();
}

void RadialMenu::StopHooks() {
    if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
    if (keyHook_) UnhookWindowsHookEx(keyHook_);
    mouseHook_ = keyHook_ = nullptr;
}

double RadialMenu::CloseMs() const { return fx::Dur(launching_ ? kLaunchMs : kCloseMs); }

void RadialMenu::Launch() {
    if (fx::Game()) {
        Hide();
        return;
    }
    launching_ = true;
    launchRoot_ = hoverRoot_;
    launchSub_ = HotSub();
    closing_ = true;
    tClose_ = GetTickCount64();
    // пока гаснет, клики уходят окну под ним
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
}

void RadialMenu::Hide() {
    if (!open_) return;
    open_ = closing_ = false;
    if (launching_) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
    launching_ = false;
    KillTimer(hwnd_, kTimer);
    StopHooks();
    LeaveShellTarget();
    DropThumb();
    dragging_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    blurBrush_.reset();
    blurBmp_.reset();
    searchPool_.clear();
    searchPool_.shrink_to_fit();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);  // в покое память не держим
}

void RadialMenu::Tick() {
    PERF_SCOPE("menu.Tick");
    ULONGLONG now = GetTickCount64();
    double dt = (now - tLast_) / 1000.0;
    tLast_ = now;
    if (closing_ && now - tClose_ >= CloseMs()) {
        Hide();
        return;
    }

    POINT pt;
    GetCursorPos(&pt);
    Hit h = HitTest(pt);
    if (!closing_) {
        // Перетаскивание без нажатых кнопок - значит, оно оборвалось, не дойдя до Drop или DragLeave.
        if (dragging_ && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000))
            OnDragLeave();
        // Во время перетаскивания наведение задаёт сама Windows (OnDragOver), иначе два источника спорят.
        if (!dragging_) UpdateHover(h, now);
        if (!demo_ && h.dist > Extent() + 170 * geo_.k) Close();  // курсор увели далеко - меню само гаснет

        bool alt = GetAsyncKeyState(VK_MENU) & 0x8000;
        bool mouseDown = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
        if (altWasDown_ && !alt && !dragging_ && !mouseDown && mode_ != Mode::Search &&
            (hoverSub_ >= 0 || hoverRoot_ >= 0)) {
            ActivateHover();  // отпустил Alt над сектором - открыть
            if (!open_) return;
        }
        altWasDown_ = alt;

        // перетаскивание: подержал файл над папкой - зашли в неё
        if (dragging_) {
            const Item* it = HoveredItem();
            bool canDrill = it && it->dir && !it->special &&
                            (hoverSub_ >= 0 || (hoverRoot_ >= 0 && !(*ring_)[hoverRoot_].HasRing()));
            std::wstring key = canDrill ? it->full : L"";
            if (key != dwellKey_) {
                dwellKey_ = key;
                dwellSince_ = now;
            } else if (!key.empty() && now - dwellSince_ > kDwellMs) {
                Item copy = *it;
                LeaveShellTarget();
                if (hoverSub_ < 0) mode_ = Mode::None;
                Drill(copy);
                dwellKey_.clear();
            }
        }
    }

    double rate = std::min(1.0, dt * 16), moved = 0;
    auto approach = [&](std::vector<double>& v, int hot) {
        for (size_t i = 0; i < v.size(); ++i) {
            double d = (((int)i == hot) - v[i]) * rate;
            v[i] += d;
            moved = std::max(moved, std::fabs(d));
        }
    };
    approach(hlRoot_, hoverRoot_);
    approach(hlSub_, HotSub());
    bool anim = closing_ || now - tOpen_ < fx::Dur(kOpenMs + kStaggerMs * hlRoot_.size() + 200) ||
                (mode_ != Mode::None && now - tSub_ < fx::Dur(kSubMs + kSubWaveMs * subItems_.size()) + 30);
    static std::wstring lastHint;
    double ha = 0;
    std::wstring hint = BottomHint(ha) + L"\x1" + TopHint();
    if (anim || moved > 0.001 || hint != lastHint) Render();
    lastHint = hint;
}

LRESULT CALLBACK RadialMenu::MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_self && g_self->open_) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN) {
            PostMessageW(g_self->hwnd_, WM_OUTSIDE_CLICK, 0, MAKELPARAM(m->pt.x, m->pt.y));
        } else if (wp == WM_MOUSEWHEEL && g_self->HitTest(m->pt).dist <= g_self->Extent() + 10) {
            PostMessageW(g_self->hwnd_, WM_MENU_WHEEL, (WPARAM)(short)HIWORD(m->mouseData), 0);
            return 1;  // колесо над меню не прокручивает окно под ним
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK RadialMenu::KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_self && g_self->open_ && !g_self->closing_ && !g_self->dragging_) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (g_self->OnRawKey(k->vkCode, k->scanCode, down)) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK RadialMenu::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_self && g_self->hwnd_ == h) return g_self->Handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT RadialMenu::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimer && open_) Tick();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MENU_KEY:
            OnKey((UINT)wp);
            return 0;
        case WM_MENU_CHAR:
            if (open_ && !closing_) OnChar((wchar_t)wp);
            return 0;
        case WM_MENU_WHEEL:
            if (open_ && !closing_) OnWheel((int)(short)wp);
            return 0;
        case WM_OUTSIDE_CLICK: {
            if (!open_ || closing_) return 0;
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            if (HitTest(p).dist > Extent() + 10) Close();
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!open_ || closing_) return 0;
            POINT p;
            GetCursorPos(&p);
            Hit hit = HitTest(p);
            UpdateHover(hit, GetTickCount64());
            if (hit.sub >= 0 || hit.root >= 0) {
                ActivateHover();
            } else if (hit.dist < geo_.r1) {
                if (!Back()) Close();  // клик по ступице: назад или закрыть
            }
            return 0;
        }
        case WM_RBUTTONUP:
            if (open_ && !closing_) OnRightClick();
            return 0;
        case WM_MBUTTONUP:
            if (open_ && !closing_ && winMode_) CloseHoveredWindow();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
