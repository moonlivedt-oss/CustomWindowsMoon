// Мышь и клавиатура: куда наведено, клавиши, открытие выбранного.
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

RadialMenu::Hit RadialMenu::HitTest(POINT s) const {
    Hit h;
    double dx = s.x - origin_.x - center_.x, dy = s.y - origin_.y - center_.y;
    h.dist = std::hypot(dx, dy);
    double a = std::atan2(dy, dx) * 180.0 / kPi;
    double rel = full_ ? a - 90.0 : left_ ? a : 180.0 - a;
    while (rel > 180) rel -= 360;
    while (rel <= -180) rel += 360;

    int n = (int)ring_->size();
    if (h.dist >= geo_.r1 && h.dist <= geo_.r2 && std::fabs(rel) <= Span()) {
        double step = 2 * Span() / n;
        h.root = std::clamp((int)((rel + Span()) / step), 0, n - 1);
    } else if (mode_ != Mode::None && h.dist > geo_.r2 && h.dist <= geo_.r4 + 12) {
        int m = (int)subItems_.size();
        double from = SubFrom(), step = SubStep();
        if (m > 0 && rel >= from && rel <= from + step * m) h.sub = std::clamp((int)((rel - from) / step), 0, m - 1);
    }
    return h;
}

void RadialMenu::UpdateHover(const Hit& h, ULONGLONG now) {
    int prevRoot = hoverRoot_, prevSub = hoverSub_;
    hoverRoot_ = mode_ == Mode::Search ? -1 : h.root;  // при поиске выбор идёт только по найденному
    hoverSub_ = h.sub;
    if (mode_ != Mode::Search) {
        if (h.root >= 0 && !(mode_ == Mode::Root && h.root == subParent_)) {
            if (h.root != pendingRoot_) {
                pendingRoot_ = h.root;
                pendingSince_ = now;
            }
            // зашли в папку - случайный проход курсора по первому кольцу её не сбрасывает
            if (navStack_.empty() || now - pendingSince_ > kSwitchDelay) SwitchRoot(h.root);
        } else {
            pendingRoot_ = -1;
        }
    }
    if ((hoverRoot_ != prevRoot || hoverSub_ != prevSub) && cfg_->sounds) {
        const Item* it = HoveredItem();
        std::wstring key = it ? it->full + it->name : L"";
        if (it && key != lastHoverKey_) Sound::Play(Sound::Hover);
        lastHoverKey_ = key;
    }
}

bool RadialMenu::OnRawKey(DWORD vk, DWORD scan, bool down) {
    if (vk >= 256) return false;
    bool isShift = vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT;
    bool isCtrl = vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL;
    if (!down) {
        if (isShift) shift_ = false;
        if (isCtrl) ctrl_ = false;
        if (swallowed_[vk]) {
            swallowed_[vk] = false;
            return true;
        }
        return false;
    }
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) return false;
    auto eat = [&]() {
        swallowed_[vk] = true;
        return true;
    };
    // Shift и Ctrl забираем себе: иначе Alt+Shift переключит раскладку, а Ctrl+Alt - что-нибудь ещё
    if (isShift) {
        shift_ = true;
        return eat();
    }
    if (isCtrl) {
        ctrl_ = true;
        return eat();
    }
    if (winMode_ && vk == VK_OEM_3) {  // повторное Alt + ` закрывает кольцо окон
        PostMessageW(hwnd_, WM_MENU_KEY, VK_ESCAPE, 0);
        return eat();
    }
    switch (vk) {
        case VK_ESCAPE:
        case VK_BACK:
        case VK_RETURN:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_TAB:
            PostMessageW(hwnd_, WM_MENU_KEY, vk, 0);
            return eat();
    }
    if (ctrl_) return false;
    BYTE ks[256] = {};
    if (shift_) ks[VK_SHIFT] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
    HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    wchar_t buf[4];
    int n = ToUnicodeEx(vk, scan, ks, buf, 4, 0x4, hkl);  // 0x4: не трогать состояние «мёртвых» клавиш
    if (n == 1 && buf[0] >= 0x20) {
        PostMessageW(hwnd_, WM_MENU_CHAR, buf[0], 0);
        return eat();
    }
    return false;
}

void RadialMenu::OnChar(wchar_t ch) {
    if (query_.empty() && ch == L' ') return;
    if (!poolBuilt_) BuildSearchPool();
    bool entering = mode_ != Mode::Search;
    query_ += ch;
    mode_ = Mode::Search;
    navStack_.clear();
    subParent_ = -1;
    kbSel_ = 0;
    RebuildSub();
    if (entering) {
        tSub_ = GetTickCount64();
        subSlide_ = 0;
    }
}

void RadialMenu::OnKey(UINT vk) {
    if (!open_ || closing_) return;
    switch (vk) {
        case VK_ESCAPE:
            if (!Back()) Close();
            break;
        case VK_BACK:
            if (!query_.empty()) {
                query_.pop_back();
                if (query_.empty())
                    Back();
                else
                    RebuildSub();
            } else {
                Back();
            }
            break;
        case VK_RETURN:
            ActivateHover();  // по копии: Activate закрывает меню и чистит списки, на которые указывает пункт
            break;
        case VK_UP:
        case VK_LEFT:
        case VK_DOWN:
        case VK_RIGHT:
        case VK_TAB:
            if (mode_ == Mode::Search && !subItems_.empty()) {
                int d = (vk == VK_UP || vk == VK_LEFT) ? -1 : 1;
                int m = (int)subItems_.size();
                kbSel_ = (kbSel_ + d + m) % m;
                if (cfg_->sounds) Sound::Play(Sound::Hover);
            } else if (pageCount_ > 1) {
                TurnPage(vk == VK_UP || vk == VK_LEFT ? -1 : 1);
            }
            break;
    }
}

void RadialMenu::OnRightClick() {
    POINT p;
    GetCursorPos(&p);
    Hit h = HitTest(p);
    if (h.sub >= 0 && subItems_[h.sub]->dir) {
        Drill(*subItems_[h.sub]);
    } else if (h.root >= 0 && (*ring_)[h.root].dir && !(*ring_)[h.root].HasRing()) {
        hoverRoot_ = h.root;
        mode_ = Mode::None;
        Drill((*ring_)[h.root]);
    } else if (!Back()) {
        Close();
    }
}

void RadialMenu::OnWheel(int delta) {
    if (pageCount_ > 1) {
        TurnPage(delta < 0 ? 1 : -1);
        return;
    }
    POINT p;
    GetCursorPos(&p);
    Hit h = HitTest(p);
    if (delta > 0 && h.sub >= 0 && subItems_[h.sub]->dir)
        Drill(*subItems_[h.sub]);
    else if (delta < 0)
        Back();
}

void RadialMenu::Activate(const Item& it) {
    if (it.special == 1) {
        TurnPage(1);
        return;
    }
    if (it.special == 2) {
        HWND w = it.win;
        StopHooks();
        UnlockForeground(hwnd_);  // право вывести чужое окно вперёд
        Launch();
        if (cfg_->sounds) Sound::Play(Sound::Select);
        winlist::Activate(w);
        return;
    }
    if (it.full.empty()) return;
    std::wstring full = it.full;
    bool dir = it.dir;
    bool sh = shift_, ct = ctrl_;
    if (ToDialog(it)) {
        HWND dlg = dialog_;
        StopHooks();
        Launch();  // фокус не трогаем: диалог и так впереди
        if (cfg_->sounds) Sound::Play(Sound::Select);
        filedlg::Go(dlg, full, dir);
        return;
    }
    StopHooks();  // иначе наш же перехватчик клавиш задержит Alt и фокус не отпустится
    UnlockForeground(hwnd_);
    Launch();
    if (cfg_->sounds) Sound::Play(Sound::Select);
    AllowSetForegroundWindow(ASFW_ANY);
    if (sh && ct) {
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + full + L"\"").c_str(), nullptr,
                      SW_SHOWNORMAL);
    } else if (sh) {
        ShellExecuteW(nullptr, nullptr, FindVsCode(cfg_).c_str(), (L"\"" + full + L"\"").c_str(), nullptr,
                      SW_SHOWNORMAL);
    } else if (ct) {
        OpenTerminal(cfg_, dir ? full : ParentDir(full));
    } else {
        ShellExecuteW(nullptr, nullptr, full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// Папки и документы уходят в диалог; программы и ярлыки по-прежнему запускаются.
bool RadialMenu::ToDialog(const Item& it) const {
    if (!dialog_ || shift_ || ctrl_ || it.special || it.full.empty() || !IsWindow(dialog_)) return false;
    if (it.dir) return true;
    std::wstring f = Lower(it.full);
    for (const wchar_t* ext : {L".exe", L".lnk", L".url", L".bat", L".cmd", L".appref-ms"}) {
        size_t n = wcslen(ext);
        if (f.size() > n && f.compare(f.size() - n, n, ext) == 0) return false;
    }
    return true;
}

void RadialMenu::CloseHoveredWindow() {
    const Item* it = HoveredItem();
    if (!it || it->special != 2) return;
    HWND gone = it->win;
    winlist::Close(gone);
    if (cfg_->sounds) Sound::Play(Sound::Back);
    // окно может закрываться не сразу (спросит про сохранение), поэтому убираем его из кольца сами
    for (size_t i = 0; i < winRing_.size(); ++i) {
        Item& r = winRing_[i];
        auto& ch = r.children;
        ch.erase(std::remove_if(ch.begin(), ch.end(), [&](const Item& c) { return c.win == gone; }), ch.end());
        if (!ch.empty()) {
            r.win = ch.front().win;
            if (ch.size() == 1) {  // осталось одно окно - второе кольцо больше не нужно
                std::wstring app = r.name;
                Item only = ch.front();
                r = std::move(only);
                r.name = app;
            }
        } else if (r.win == gone) {
            winRing_.erase(winRing_.begin() + i);
            break;
        }
    }
    if (winRing_.empty()) {
        Close();
        return;
    }
    mode_ = Mode::None;
    hoverRoot_ = hoverSub_ = subParent_ = pendingRoot_ = -1;
    hlRoot_.assign(winRing_.size(), 0.0);
    hlSub_.clear();
    subItems_.clear();
    subOwned_.clear();
    navStack_.clear();
    query_.clear();
    poolBuilt_ = false;
    lastHoverKey_.clear();
    Render();
}

void RadialMenu::ActivateHover() {
    if (const Item* it = HoveredItem()) {
        Item copy = *it;  // Activate закрывает меню и чистит списки
        Activate(copy);
    }
}
