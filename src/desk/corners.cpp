#include "desk/corners.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/fx.h"
#include "core/llmouse.h"
#include "core/macro.h"
#include "core/config.h"
#include "core/night.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/volume.h"
#include "desk/winfx.h"
#include "gfx/draw.h"
#include "gfx/hex.h"

using namespace Gdiplus;

namespace corners {

namespace {

constexpr wchar_t kClass[] = L"MoonRadialCorner";
constexpr UINT WM_WHEEL = WM_APP + 1, WM_BRIGHT = WM_APP + 2;
constexpr int kSize = 180;          // окно свечения, до масштаба
constexpr int kTol = 1;             // сколько пикселей от края ещё считается углом
constexpr int kWheelTol = 3;        // для колеса угол чуть шире: рука с мышью дрожит
constexpr int kRearm = 48;          // после срабатывания курсор должен отойти на столько
constexpr ULONGLONG kDouble = 450;  // второй толчок должен прийти за столько мс
constexpr ULONGLONG kLevelShow = 1300;

HotCorners* g_self = nullptr;

const Action kActions[] = {
    {L"", L"Ничего"},
    {L"desktop", L"Показать рабочий стол"},
    {L"tasks", L"Все окна"},
    {L"windows", L"Кольцо окон"},
    {L"start", L"Меню Пуск"},
    {L"screen_off", L"Выключить экран"},
    {L"lock", L"Заблокировать"},
    {L"screenshot", L"Снимок области"},
    {L"play", L"Пауза и продолжить"},
    {L"next", L"Следующий трек"},
    {L"mute", L"Выключить звук"},
    {L"pin", L"Окно поверх всех"},
    {L"boost", L"Освободить память"},
    {L"net", L"Кольцо сети"},
    {L"tools", L"Кольцо утилит"},
    {L"minecraft", L"Кольцо Minecraft"},
    {L"clip", L"Кольцо буфера"},
    {L"layout", L"Кольцо раскладки окон"},
    {L"sets", L"Рабочие наборы"},
};

const Action kWheel[] = {
    {L"", L"Ничего"},
    {L"brightness", L"Яркость"},
    {L"volume", L"Громкость"},
    {L"track", L"Треки"},
};

void Keys(const std::vector<WORD>& vk) {
    std::vector<INPUT> in;
    for (WORD k : vk) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = k;
        in.push_back(i);
    }
    for (auto it = vk.rbegin(); it != vk.rend(); ++it) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = *it;
        i.ki.dwFlags = KEYEVENTF_KEYUP;
        in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void CALLBACK ScreenOff(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
}

bool OffScreen(LONG x, LONG y) { return !MonitorFromPoint({x, y}, MONITOR_DEFAULTTONULL); }

}  // namespace

bool FullscreenOn(HMONITOR mon, const RECT& mr) {
    HWND fg = GetForegroundWindow();
    if (!fg || MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != mon) return false;
    wchar_t cls[64];
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return false;
    // развёрнутое окно при скрытой панели задач тоже закрывает весь экран, но это не игра;
    // игры и видео на весь экран не «развёрнуты» и обычно без рамки
    if (IsZoomed(fg)) return false;
    LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION && (style & WS_THICKFRAME)) return false;
    RECT r;
    GetWindowRect(fg, &r);
    return r.left <= mr.left && r.top <= mr.top && r.right >= mr.right && r.bottom >= mr.bottom;
}

const Action* Actions(int& count) {
    count = (int)(sizeof(kActions) / sizeof(kActions[0]));
    return kActions;
}

const Action* WheelActions(int& count) {
    count = (int)(sizeof(kWheel) / sizeof(kWheel[0]));
    return kWheel;
}

std::wstring KeyName(UINT vk) { return macro::KeyName(vk); }

std::wstring ActionName(const std::wstring& v) {
    for (const Action& a : kActions)
        if (v == a.key) return a.name;
    for (const Action& a : kWheel)
        if (v == a.key) return a.name;
    if (v.rfind(L"macro:", 0) == 0) return macro::Describe(v);
    if (v.rfind(L"keys:", 0) == 0) {
        std::wstring out;
        for (auto& t : Split(v.substr(5), L'+')) {
            if (!out.empty()) out += L" + ";
            std::wstring n = t;
            if (!n.empty()) n[0] = towupper(n[0]);
            if (n == L"Pgup") n = L"PgUp";
            if (n == L"Pgdn") n = L"PgDn";
            if (n == L"Prtsc") n = L"PrtSc";
            out += n;
        }
        return out;
    }
    std::wstring f = ExpandPath(v);
    size_t s = f.find_last_of(L"\\/");
    std::wstring n = s == std::wstring::npos ? f : f.substr(s + 1);
    size_t dot = n.find_last_of(L'.');
    return dot != std::wstring::npos && dot > 0 ? n.substr(0, dot) : n;
}

void (*g_windowsRing)() = nullptr;

void SetWindowsRing(void (*show)()) { g_windowsRing = show; }

void (*g_lock)() = nullptr;

void SetLock(void (*lock)()) { g_lock = lock; }

bool (*g_screenOff)() = nullptr;

void SetScreenOff(bool (*off)()) { g_screenOff = off; }

void (*g_boost)() = nullptr;

void SetBoost(void (*boost)()) { g_boost = boost; }

void (*g_net)() = nullptr;

void SetNet(void (*net)()) { g_net = net; }

void (*g_tools)() = nullptr;

void SetTools(void (*tools)()) { g_tools = tools; }

void (*g_minecraft)() = nullptr;

void SetMinecraft(void (*mc)()) { g_minecraft = mc; }

void (*g_clip)() = nullptr;

void SetClip(void (*clip)()) { g_clip = clip; }

void (*g_layout)() = nullptr;

void SetLayout(void (*layout)()) { g_layout = layout; }

void (*g_sets)() = nullptr;

void SetSets(void (*sets)()) { g_sets = sets; }

void Run(const std::wstring& a) {
    if (a.empty()) return;
    if (a == L"sets") {
        if (g_sets) g_sets();
        return;
    }
    if (a == L"layout") {
        if (g_layout) g_layout();
        return;
    }
    if (a == L"clip") {
        if (g_clip) g_clip();
        return;
    }
    if (a == L"windows") {
        if (g_windowsRing) g_windowsRing();
        return;
    }
    if (a == L"desktop") return Keys({VK_LWIN, 'D'});
    if (a == L"tasks") return Keys({VK_LWIN, VK_TAB});
    if (a == L"start") return Keys({VK_LWIN});
    if (a == L"boost") {
        if (g_boost) g_boost();
        return;
    }
    if (a == L"net") {
        if (g_net) g_net();
        return;
    }
    if (a == L"tools") {
        if (g_tools) g_tools();
        return;
    }
    if (a == L"minecraft") {
        if (g_minecraft) g_minecraft();
        return;
    }
    if (a == L"screen_off") {
        if (g_screenOff && g_screenOff()) return;  // портал схлопнется в угол и погасит экран сам
        SetTimer(nullptr, 0, 600, ScreenOff);      // пусть мышь успокоится, иначе экран сразу проснётся
        return;
    }
    if (a == L"lock") {
        if (g_lock)
            g_lock();  // с затемнением в картинку блокировки
        else
            LockWorkStation();
        return;
    }
    if (a == L"screenshot") return Keys({VK_LWIN, VK_SHIFT, 'S'});
    if (a == L"play") return Keys({VK_MEDIA_PLAY_PAUSE});
    if (a == L"next") return Keys({VK_MEDIA_NEXT_TRACK});
    if (a == L"mute") return Keys({VK_VOLUME_MUTE});
    if (a == L"pin") return PinForegroundWindow();
    if (macro::Is(a)) return macro::Run(a);  // сочетание или макрос: в фоне, когда отпустят модификаторы
    std::wstring f = ExpandPath(a);
    ShellExecuteW(nullptr, nullptr, f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void HotCorners::Create(HINSTANCE inst) {
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    bright_.Start(wnd_, WM_BRIGHT);
    // колесо нужно поймать раньше окна под курсором, поэтому перехват мыши всей системы;
    // ставится, только когда колесу в каком-нибудь углу что-то назначено (Tick)
    llmouse::Set(llmouse::CORNERS, OnMouse);
}

void HotCorners::Destroy() {
    llmouse::Need(llmouse::CORNERS, false);
    bright_.Stop();
}

LRESULT CALLBACK HotCorners::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_WHEEL && g_self) {
        g_self->Wheel((int)wp, (int)lp);
        return 0;
    }
    if (msg == WM_BRIGHT && g_self) {
        int lvl = (int)(INT_PTR)wp;
        g_self->ShowLevel(g_self->levelCorner_, lvl, lvl < 0 ? L"Яркость не меняется" : L"Яркость");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool HotCorners::OnMouse(WPARAM wp, const MSLLHOOKSTRUCT& m) {
    // поток перехвата (core/llmouse): только быстрая проверка, сама работа уходит сообщением,
    // иначе Windows снимет перехват. Настройки отсюда не читаются (их меняет основной поток) - только снимок.
    if (wp != WM_MOUSEWHEEL || !g_self) return false;
    POINT at;
    RECT mon;
    HMONITOR hm;
    int c = g_self->CornerAt(m.pt, kWheelTol, &at, &mon, &hm);
    if (c < 0 || !(g_self->wheelMask_ >> c & 1)) return false;
    if (g_self->skipFullscreen_ && FullscreenOn(hm, mon)) return false;
    PostMessageW(g_self->wnd_, WM_WHEEL, (WPARAM)c, (LPARAM)(short)HIWORD(m.mouseData));
    return true;
}

int HotCorners::CornerAt(POINT pt, int tol, POINT* at, RECT* monRect, HMONITOR* hmon) const {
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& r = mi.rcMonitor;
    *hmon = mon;
    *monRect = r;
    bool atL = pt.x <= r.left + tol, atR = pt.x >= r.right - 1 - tol;
    bool atT = pt.y <= r.top + tol, atB = pt.y >= r.bottom - 1 - tol;
    if (!(atL || atR) || !(atT || atB)) return -1;
    *at = {atL ? r.left : r.right - 1, atT ? r.top : r.bottom - 1};
    // угол настоящий, только если и сбоку, и сверху/снизу нет другого монитора
    if (!OffScreen(at->x + (atL ? -1 : 1), at->y) || !OffScreen(at->x, at->y + (atT ? -1 : 1))) return -1;
    return atT ? (atL ? TL : TR) : (atL ? BL : BR);
}

void HotCorners::Wheel(int c, int delta) {
    if (!cfg_ || c < 0 || c >= kCount) return;
    POINT pt;
    GetCursorPos(&pt);
    POINT at;
    RECT mon;
    HMONITOR hm;
    if (CornerAt(pt, kWheelTol, &at, &mon, &hm) == c) {
        at_ = at;
        mon_ = mon;
    }
    // покрутили колесо - задержка в этом углу уже не срабатывает, даже если опрос ещё не заметил угол
    wheelCorner_ = c;
    wheelAt_ = GetTickCount64();
    if (corner_ == c) fired_ = true;
    color_ = CurrentTheme(*cfg_).ac;
    float notches = delta / (float)WHEEL_DELTA;
    const std::wstring& a = cfg_->cornerWheel[c];
    if (a == L"volume") {
        float lvl = vol::AddMaster(0.02f * notches);
        ShowLevel(c, lvl < 0 ? -1 : (int)std::lround(lvl * 100), L"Громкость");
    } else if (a == L"brightness") {
        levelCorner_ = c;
        bright_.Add(hm, (int)std::lround(5 * notches));
    } else if (a == L"track") {
        Keys({(WORD)(notches > 0 ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PREV_TRACK)});
        ShowLevel(c, -1, notches > 0 ? L"Следующий трек" : L"Предыдущий трек");
    }
}

void HotCorners::ShowLevel(int c, int level, const wchar_t* label) {
    if (c < 0) return;
    levelCorner_ = c;
    level_ = level;
    levelLabel_ = label;
    levelUntil_ = GetTickCount64() + kLevelShow;
    if (levelShow_ <= 0) levelShow_ = 0.01f;
    Paint();
}

void HotCorners::Tick(const Config& cfg, bool blocked) {
    cfg_ = &cfg;
    int mask = 0;
    for (int i = 0; i < kCount; ++i)
        if (!cfg.cornerWheel[i].empty()) mask |= 1 << i;
    skipFullscreen_ = cfg.skipFullscreen;
    if (wheelMask_.exchange(mask) != mask) llmouse::Need(llmouse::CORNERS, mask != 0);
    POINT pt, at{};
    GetCursorPos(&pt);
    RECT r;
    HMONITOR mon;
    int c = CornerAt(pt, kTol, &at, &r, &mon);
    bool any = c >= 0 && (!cfg.corners[c].empty() || !cfg.corners2[c].empty());
    if (!any) c = -1;
    bool buttons =
        (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
    if (blocked || buttons || (c >= 0 && cfg.skipFullscreen && FullscreenOn(mon, r))) c = -1;

    // после срабатывания угол «залипает», пока курсор не отойдёт: дрожание мыши не повторит действие
    if (fired_ && corner_ >= 0 && std::abs(pt.x - at_.x) < kRearm && std::abs(pt.y - at_.y) < kRearm) c = corner_;

    ULONGLONG now = GetTickCount64();
    if (c != corner_) {
        if (corner_ >= 0) {
            lastCorner_ = corner_;
            lastLeave_ = now;
            lastFired_ = fired_;
        }
        corner_ = c;
        since_ = now;
        fired_ = false;
        if (c >= 0) {
            at_ = at;
            mon_ = r;
            color_ = CurrentTheme(cfg).ac;
            // второй быстрый толчок в тот же угол
            if (c == lastCorner_ && !lastFired_ && now - lastLeave_ < kDouble && !cfg.corners2[c].empty()) {
                fired_ = true;
                flash_ = 1;
                progress_ = 0;
                Run(cfg.corners2[c]);
            }
        }
    }

    if (corner_ >= 0 && corner_ == wheelCorner_ && now - wheelAt_ < 1500) fired_ = true;
    if (corner_ >= 0 && !fired_ && !cfg.corners[corner_].empty()) {
        progress_ = std::min(1.0f, (now - since_) / (float)std::max(50, cfg.cornerDelay));
        if (progress_ >= 1) {
            fired_ = true;
            flash_ = 1;
            Run(cfg.corners[corner_]);
        }
    } else {
        progress_ = std::max(0.0f, progress_ - fx::Step(0.12f));
    }
    flash_ = std::max(0.0f, flash_ - fx::Step(0.06f));
    if (levelShow_ > 0) {
        levelShow_ = now < levelUntil_ ? std::min(1.0f, levelShow_ + fx::Step(0.2f))
                                       : std::max(0.0f, levelShow_ - fx::Step(0.08f));
    }

    if (corner_ >= 0 || progress_ > 0 || flash_ > 0 || levelShow_ > 0) fx::Active();  // курсор в углу или свечение
    if (progress_ <= 0 && flash_ <= 0 && levelShow_ <= 0) {
        Hide();
        return;
    }
    Paint();
}

void HotCorners::Hide() {
    if (wnd_ && IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_HIDE);
}

void HotCorners::Paint() {
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(MonitorFromPoint(at_, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpi, &dy);
    float k = dpi / 96.0f;
    int s = (int)(kSize * k);
    bool left = at_.x == mon_.left, top = at_.y == mon_.top;
    int x = left ? mon_.left : mon_.right - s, y = top ? mon_.top : mon_.bottom - s;
    float cx = left ? 0.0f : (float)s, cy = top ? 0.0f : (float)s;  // угол внутри окна
    float dx = left ? 1.0f : -1.0f, dy2 = top ? 1.0f : -1.0f;

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), s, -s, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap bmp(s, s, s * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        float e = (float)EaseOut(progress_);
        float lv = (float)EaseOut(levelShow_);
        Color ac(GetRValue(color_), GetGValue(color_), GetBValue(color_));
        float start = left ? (top ? 0.0f : 270.0f) : (top ? 90.0f : 180.0f);

        // мягкое свечение из угла (с сотами - вдвое слабее, под ними)
        bool hexes = cfg_ && cfg_->hexGlass;
        float grow = std::max({e, flash_, lv});
        float rad = s * (0.34f + 0.48f * grow);
        GraphicsPath path;
        path.AddEllipse(cx - rad, cy - rad, rad * 2, rad * 2);
        PathGradientBrush pg(&path);
        pg.SetCenterPoint(PointF(cx, cy));
        BYTE a = (BYTE)(std::min(255.0f, 120 * e + 110 * flash_ + 150 * lv) * night::Glow() * (hexes ? 0.5f : 1.0f));
        pg.SetCenterColor(Color(a, ac.GetR(), ac.GetG(), ac.GetB()));
        Color edge(0, ac.GetR(), ac.GetG(), ac.GetB());
        int n = 1;
        pg.SetSurroundColors(&edge, &n);
        g.FillEllipse(&pg, cx - rad, cy - rad, rad * 2, rad * 2);

        // веер сот из угла: пока курсор ждёт, соты загораются одна за другой от угла,
        // при срабатывании вспыхивают светлым
        if (hexes && grow > 0.01f) {
            double hs = 15 * k, front = s * (0.18 + 0.82 * grow);
            Color rimC = MixColor(ac, Color(255, 255, 255), 0.4 + 0.4 * flash_);
            float glowK = night::Glow();
            hex::ForEach(0, 0, s, s, hs, [&](const hex::Cell& c) {
                double dc = std::hypot(c.x - cx, c.y - cy);
                double lit = Clamp01((front - dc) / (hs * 1.6));
                double fall = std::pow(std::max(0.0, 1 - dc / (s * 1.02)), 1.2);
                double al = lit * fall * glowK;
                if (al < 0.02) return;
                // сота у самого фронта чуть больше: «встаёт» на место
                double pop = 0.80 + 0.12 * std::exp(-std::pow((front - dc) / hs, 2));
                GraphicsPath hp;
                hex::Path(hp, c.x, c.y, hs, pop);
                SolidBrush fill(WithAlpha(MixColor(ac, Color(255, 255, 255), 0.5 * flash_),
                                          (BYTE)std::min(255.0, (110 + 90 * flash_) * al)));
                g.FillPath(&fill, &hp);
                Pen edge(WithAlpha(rimC, (BYTE)std::min(255.0, 235 * al)), 1.3f * k);
                g.DrawPath(&edge, &hp);
            });
        }

        // дуга ожидания: заполняется четверть круга
        if (!fired_ && progress_ > 0 && lv <= 0) {
            float ar = s * 0.24f;
            Pen track(Color((BYTE)(60 * e), 255, 255, 255), 4.0f * k);
            Pen arc(Color((BYTE)(230 * e), 255, 255, 255), 5.0f * k);
            arc.SetStartCap(LineCapRound);
            arc.SetEndCap(LineCapRound);
            g.DrawArc(&track, cx - ar, cy - ar, ar * 2, ar * 2, start, 90);
            g.DrawArc(&arc, cx - ar, cy - ar, ar * 2, ar * 2, start, 90 * progress_);
        }

        // пока курсор ждёт - рядом проступает, что сейчас случится: «Кольцо сети», «Показать рабочий стол»
        if (!fired_ && progress_ > 0 && lv <= 0 && cfg_ && corner_ >= 0 && !cfg_->corners[corner_].empty()) {
            std::wstring name = ActionName(cfg_->corners[corner_]);
            Font f(Family(), 12.5f * k, FontStyleBold, UnitPixel);
            StringFormat sf;
            sf.SetAlignment(left ? StringAlignmentNear : StringAlignmentFar);
            sf.SetLineAlignment(StringAlignmentCenter);
            float bw = s * 0.62f, bh = s * 0.14f;
            RectF box(left ? cx + s * 0.30f : cx - s * 0.30f - bw, cy + dy2 * s * 0.42f - bh / 2, bw, bh);
            // тёмная плашка под текстом: на свечении и панели задач иначе не прочитать
            RectF tb;
            g.MeasureString(name.c_str(), -1, &f, PointF(0, 0), &tb);
            float pw = std::min(bw, tb.Width + 14 * k), ph = tb.Height + 4 * k;
            RectF pill(left ? box.X - 7 * k : box.GetRight() - pw + 7 * k, box.Y + (bh - ph) / 2, pw, ph);
            GraphicsPath pp;
            RoundRectPath(pp, pill, ph / 2);
            SolidBrush pillB(Color((BYTE)(205 * e), 16, 14, 20));
            g.FillPath(&pillB, &pp);
            Pen pillE(Color((BYTE)(110 * e), ac.GetR(), ac.GetG(), ac.GetB()), 1.0f * k);
            g.DrawPath(&pillE, &pp);
            SolidBrush text(Color((BYTE)(250 * e), 255, 255, 255));
            g.DrawString(name.c_str(), -1, &f, box, &sf, &text);
        }

        // уровень после колеса: дуга и число
        if (lv > 0) {
            float ar = s * 0.36f;
            if (level_ >= 0) {
                Pen track(Color((BYTE)(70 * lv), 255, 255, 255), 5.0f * k);
                Pen arc(Color((BYTE)(240 * lv), 255, 255, 255), 6.0f * k);
                arc.SetStartCap(LineCapRound);
                arc.SetEndCap(LineCapRound);
                // заполняется от края экрана по горизонтали к краю по вертикали
                g.DrawArc(&track, cx - ar, cy - ar, ar * 2, ar * 2, start, 90);
                if (level_ > 0) g.DrawArc(&arc, cx - ar, cy - ar, ar * 2, ar * 2, start, 90.0f * level_ / 100);
            }
            FontFamily fam(L"Segoe UI");
            Font big(Family(), 22 * k, FontStyleRegular, UnitPixel);
            Font small(&fam, 10.5f * k, FontStyleRegular, UnitPixel);
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            float tx = cx + dx * s * 0.17f, ty = cy + dy2 * s * 0.17f;
            RectF box(tx - s * 0.2f, ty - s * 0.12f, s * 0.4f, s * 0.24f);
            std::wstring num = level_ >= 0 ? std::to_wstring(level_) : L"";
            SolidBrush shadowB(Color((BYTE)(110 * lv), 0, 0, 0)), textB(Color((BYTE)(255 * lv), 255, 255, 255));
            SolidBrush subB(Color((BYTE)(220 * lv), 255, 255, 255));
            RectF lab(cx + dx * s * 0.46f - s * 0.3f, cy + dy2 * s * 0.44f - s * 0.08f, s * 0.6f, s * 0.16f);
            if (!num.empty()) {
                RectF sh = box;
                sh.Offset(0, 1.5f * k);
                g.DrawString(num.c_str(), -1, &big, sh, &sf, &shadowB);
                g.DrawString(num.c_str(), -1, &big, box, &sf, &textB);
            } else {
                lab = RectF(cx + dx * s * 0.3f - s * 0.3f, cy + dy2 * s * 0.22f - s * 0.1f, s * 0.6f, s * 0.2f);
            }
            RectF lsh = lab;
            lsh.Offset(0, 1 * k);
            g.DrawString(levelLabel_.c_str(), -1, &small, lsh, &sf, &shadowB);
            g.DrawString(levelLabel_.c_str(), -1, &small, lab, &sf, &subB);
        }
    }
    POINT src{0, 0}, dst{x, y};
    SIZE sz{s, s};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

}  // namespace corners
