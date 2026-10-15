#include "desk/deskclick.h"

#include <oleacc.h>

#include <cstdlib>

#include "core/llmouse.h"

namespace {

constexpr wchar_t kClass[] = L"MoonRadialDeskClick";
constexpr UINT WM_DESK_DOUBLE = WM_APP + 1;
DeskClick* g_self = nullptr;

}  // namespace

void DeskClick::Create(HINSTANCE inst) {
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    llmouse::Set(llmouse::DESK_CLICK, OnMouse);
}

void DeskClick::SetEnabled(bool on) { llmouse::Need(llmouse::DESK_CLICK, on && wnd_); }

void DeskClick::Destroy() { llmouse::Need(llmouse::DESK_CLICK, false); }

// Под точкой стол: сам проводник (Progman, WorkerW) или окно живых обоев внутри него.
bool DeskClick::IsDesktop(POINT pt) {
    HWND w = WindowFromPoint(pt);
    if (!w) return false;
    HWND root = GetAncestor(w, GA_ROOT);
    wchar_t cls[64] = {}, own[64] = {};
    GetClassNameW(root, cls, 64);
    GetClassNameW(w, own, 64);
    if (wcscmp(cls, L"Progman") && wcscmp(cls, L"WorkerW")) return false;
    return true;
}

bool DeskClick::OnMouse(WPARAM wp, const MSLLHOOKSTRUCT& m) {
    if (wp != WM_LBUTTONDOWN || !g_self) return false;
    int dx = GetSystemMetrics(SM_CXDOUBLECLK), dy = GetSystemMetrics(SM_CYDOUBLECLK);
    bool quick = g_self->lastTime_ && m.time - g_self->lastTime_ <= GetDoubleClickTime();
    bool close = std::abs(m.pt.x - g_self->lastPt_.x) <= dx && std::abs(m.pt.y - g_self->lastPt_.y) <= dy;
    if (quick && close && IsDesktop(m.pt)) {
        g_self->lastTime_ = 0;  // третий клик не считается новым двойным
        PostMessageW(g_self->wnd_, WM_DESK_DOUBLE, 0, MAKELPARAM(m.pt.x, m.pt.y));
    } else {
        g_self->lastTime_ = m.time;
        g_self->lastPt_ = m.pt;
    }
    return false;  // клики не перехватываются: проводник получает их как обычно
}

LRESULT CALLBACK DeskClick::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESK_DOUBLE && g_self && g_self->onDouble) {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        // двойной клик по значку открывает сам значок, кольцо не нужно
        IAccessible* acc = nullptr;
        VARIANT child;
        VariantInit(&child);
        bool onIcon = false;
        if (SUCCEEDED(AccessibleObjectFromPoint(pt, &acc, &child)) && acc) {
            VARIANT role;
            VariantInit(&role);
            if (SUCCEEDED(acc->get_accRole(child, &role)) && role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_LISTITEM)
                onIcon = true;
            acc->Release();
        }
        VariantClear(&child);
        if (!onIcon) g_self->onDouble(pt);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
