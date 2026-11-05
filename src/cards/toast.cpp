#include "cards/toast.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/fx.h"
#include "core/mem.h"
#include "desktop/share.h"
#include "core/night.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

std::vector<Toast*>& All() {
    static std::vector<Toast*> all;
    return all;
}
ULONGLONG g_order = 0;

// плавное приближение к цели за время tau (мс), не зависит от частоты кадров
inline float Approach(float v, float target, double dt, double tau) {
    return target + (v - target) * (float)std::exp(-dt / tau);
}

double Lum(const Color& c) { return (0.299 * c.GetR() + 0.587 * c.GetG() + 0.114 * c.GetB()) / 255; }

}  // namespace

// ---------------- окно ----------------

void Toast::CreateWnd(HINSTANCE inst, const wchar_t* cls, const wchar_t* title) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, cls, title, WS_POPUP, 0,
                           0, 1, 1, nullptr, nullptr, inst, this);
    All().push_back(this);
}

void Toast::Destroy() {
    auto& all = All();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (wnd_) DestroyWindow(wnd_);
    wnd_ = nullptr;
}

void Toast::Open(int holdMs, bool onCursor) {
    menuOpen_ = false;  // новое содержимое: меню относилось к прежнему
    menuNote_.clear();
    menuDoneAt_ = 0;
    POINT pt{0, 0};
    if (onCursor) GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, onCursor ? MONITOR_DEFAULTTONEAREST : MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    bool moved = !EqualRect(&work_, &mi.rcWork) || k_ != dx / 96.0;
    work_ = mi.rcWork;
    k_ = dx / 96.0;
    if (appear_ <= 0.001f || moved) {
        // новая карточка встаёт над уже открытыми
        order_ = ++g_order;
        yInit_ = false;
        appear_ = appearV_ = 0;
        dragX_ = 0;
        hInit_ = false;
        if (LiquidMotion()) {
            liqX_ = kW + 60;
            liqV_ = 0;
            s_ = 1;
            sv_ = 0;
            tether_ = 1;
            tetherT_ = 0;
            splashed_ = false;
            drops_.clear();
            revealAt_ = 0;
        }
    } else if (LiquidMotion()) {
        if (!calmUpdate_) Kick(0.5f);  // уже на экране: вздрагивает вместо повторного въезда
        if (!open_) revealAt_ = 0;     // ушла и тут же вернулась с новым: содержимое появляется заново
    } else {
        pulse_ = 1;  // уже на экране: короткий толчок вместо повторного въезда
    }
    calmUpdate_ = false;
    if (!open_) last_ = GetTickCount64();
    open_ = true;
    hold_ = left_ = holdMs > 0 ? holdMs : 0;
    dirty_ = true;
    fx::Wake();  // въезд идёт от тактов опроса
}

void Toast::Dismiss() {
    if (!open_) return;
    open_ = false;
    byHand_ = true;
    Closed();
}

void Toast::Hold(int holdMs) {
    if (!open_) return;
    hold_ = left_ = holdMs > 0 ? holdMs : 0;
    if (!LiquidMotion()) pulse_ = 1;
    dirty_ = true;
}

float Toast::Reveal(float delay, float dur) const {
    if (!LiquidMotion() || fx::Game()) return 1;
    if (!revealAt_) return 0;
    double t = (GetTickCount64() - revealAt_) / fx::Dur(1000) - delay;
    return (float)Clamp01(t / dur);
}

// Пружины считаются мелкими шагами: кадры приходят неровно, а карточка не должна дёргаться.
void Toast::StepLiquid(double dtMs, bool& anim) {
    float K = k();
    if (fx::Game()) {  // в игре - сразу на месте, без брызг
        liqX_ = open_ ? 0 : kW + 70;
        liqV_ = sv_ = 0;
        s_ = 1;
        tether_ = 0;
        drops_.clear();
        appear_ = open_ ? 1.0f : 0.0f;
        h_ = hT_;
        anim = true;
        return;
    }
    double dt = dtMs / fx::Dur(1000);
    float tx = open_ ? 0 : kW + 70;
    bool drag = dragging_ && open_;
    if (drag) wasDrag_ = true;
    int n = std::max(1, (int)std::ceil(dt / 0.004));
    float h = (float)(dt / n), dragTo = dragX_ / K;
    for (int i = 0; i < n; ++i) {
        if (drag) {
            liqV_ = (dragTo - liqX_) / std::max(h, 0.001f) * 0.05f + liqV_ * 0.95f;
            liqX_ = dragTo;
        } else {
            float kk = open_ ? 150.f : 230.f, c = open_ ? 14.f : 18.f;
            liqV_ += (kk * (tx - liqX_) - c * liqV_) * h;
            liqX_ += liqV_ * h;
        }
        float st = drag ? 1 + std::min(0.34f, liqX_ / 620) : 1 + std::min(0.52f, std::fabs(liqV_) / 1500);
        sv_ += (430 * (st - s_) - 13 * sv_) * h;
        s_ += sv_ * h;
    }
    if (!drag) {
        dragX_ = 0;
        if (wasDrag_) {  // отпустили, а карточка осталась: дрожит на месте
            wasDrag_ = false;
            if (open_) Kick(-0.6f);
        }
    } else {
        tether_ = 1;
    }
    if (open_ && !splashed_ && liqV_ > 0 && liqX_ < 40) {  // удар: брызги улетают и втягиваются обратно
        splashed_ = true;
        Kick(-1.1f);
        for (int i = 0; i < 5; ++i) {
            float r1 = rand() / (float)RAND_MAX, r2 = rand() / (float)RAND_MAX, r3 = rand() / (float)RAND_MAX;
            drops_.push_back({0, 0.62f + r1 * 0.25f, 0.12f + 0.76f * i / 4, 34 + r2 * 46, 7 + r3 * 7,
                              (i - 2) * (6 + r1 * 7)});
        }
    }
    if (open_ && !revealAt_ && liqX_ < kW * 0.3f) revealAt_ = GetTickCount64();
    if (open_ && !drag && splashed_) {  // устоялась - перемычка рвётся
        tetherT_ += (float)dt;
        if (tetherT_ > 0.28f) tether_ = std::max(0.0f, tether_ - (float)dt * 4);
    }
    if (!open_) tether_ = 1;
    for (Drop& d : drops_) d.t += (float)(dt / d.life);
    drops_.erase(std::remove_if(drops_.begin(), drops_.end(), [](const Drop& d) { return d.t >= 1; }), drops_.end());

    if (open_ && !drag && std::fabs(liqX_) < 0.2f && std::fabs(liqV_) < 2) liqX_ = liqV_ = 0;
    if (std::fabs(s_ - 1) < 0.002f && std::fabs(sv_) < 0.02f) s_ = 1, sv_ = 0;
    float gone = kW + 40;
    appear_ = !open_ && liqX_ >= gone ? 0 : std::clamp(1 - liqX_ / gone, 0.001f, 1.0f);
    bool neck = open_ && tether_ > 0 && (drag || splashed_);
    bool revealing = revealAt_ && GetTickCount64() - revealAt_ < fx::Dur(1100);
    anim |= liqX_ != tx || liqV_ != 0 || s_ != 1 || neck || !open_ || !drops_.empty() || revealing;
    if (hot_ != H_NONE && hold_ > 0) anim = true;  // рябь на полоске времени
    if (std::fabs(h_ - hT_) > 0.2f) {
        h_ = Approach(h_, hT_, dtMs, 75);
        anim = true;
    } else {
        h_ = hT_;
    }
}

// Вязкая часть силуэта: перемычка и брызги сливаются с карточкой через размытие и порог. Сама карточка рисуется
// поверх чётко, поэтому от этого слоя остаются только плавные перетяжки, и в покое его просто нет.
void Toast::PaintGoo(int W, int H, int clipX, Color fill) {
    float K = k();
    RectF body = Body();
    float cx = body.X + body.Width / 2 + liqX_ * K, cy = body.Y + body.Height / 2;
    float cw = body.Width * s_, ch = body.Height / s_;
    float right = cx + cw / 2, gap = clipX - right;
    float nh = tether_ * std::max(0.0f, ch * 0.58f - gap * 0.1f);
    bool neck = gap > 0 && nh > 2 * K;
    if (!neck && drops_.empty()) return;
    Bitmap mask(W, H, PixelFormat32bppPARGB);
    {
        Graphics g(&mask);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        SolidBrush white(Color(255, 255, 255, 255));
        GraphicsPath p;
        RoundRectPath(p, RectF(cx - cw / 2, cy - ch / 2, cw, ch), 16 * K);
        g.FillPath(&white, &p);
        if (neck) {
            GraphicsPath np;
            RoundRectPath(np, RectF(right - 14 * K, cy - nh / 2, gap + 40 * K, nh), nh / 2);
            g.FillPath(&white, &np);
        }
        float left = cx - cw / 2;
        for (const Drop& d : drops_) {
            float pp = (float)std::sin(kPi * d.t), r = d.r * (1 - 0.35f * d.t) * K;
            float dx = left + 6 * K - d.amp * pp * K, dy = body.Y + body.Height * d.y + d.dy * pp * K;
            g.FillEllipse(&white, dx - r, dy - r, 2 * r, 2 * r);
        }
    }
    goo_.resize((size_t)W * H);
    gooTmp_.resize((size_t)W * H);
    {
        BitmapData d;
        Rect r(0, 0, W, H);
        if (mask.LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &d) != Ok) return;
        for (int y = 0; y < H; ++y) {
            const uint32_t* row = (const uint32_t*)((BYTE*)d.Scan0 + (size_t)y * d.Stride);
            for (int x = 0; x < W; ++x) goo_[(size_t)y * W + x] = (uint8_t)(row[x] >> 24);
        }
        mask.UnlockBits(&d);
    }
    // два прохода скользящего среднего по каждой оси - почти гауссово размытие
    int R = std::max(2, (int)std::lround(5 * K)), N = R * 2 + 1;
    auto pass = [&](const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, bool horiz) {
        int len = horiz ? W : H, lines = horiz ? H : W;
        size_t step = horiz ? 1 : (size_t)W, next = horiz ? (size_t)W : 1;
        for (int l = 0; l < lines; ++l) {
            const uint8_t* s0 = src.data() + l * next;
            uint8_t* d0 = dst.data() + l * next;
            int sum = 0;
            for (int i = -R; i <= R; ++i) sum += s0[std::clamp(i, 0, len - 1) * step];
            for (int i = 0; i < len; ++i) {
                d0[i * step] = (uint8_t)(sum / N);
                sum += s0[std::min(i + R + 1, len - 1) * step] - s0[std::max(i - R, 0) * step];
            }
        }
    };
    pass(goo_, gooTmp_, true);
    pass(gooTmp_, goo_, false);
    pass(goo_, gooTmp_, true);
    pass(gooTmp_, goo_, false);
    int fa = fill.GetA(), fr = fill.GetR() * fa / 255, fg = fill.GetG() * fa / 255, fb = fill.GetB() * fa / 255;
    uint32_t* out = (uint32_t*)bits_;
    int xEnd = std::min(W, clipX);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < xEnd; ++x) {
            int v = goo_[(size_t)y * W + x];
            if (v < 104) continue;  // порог: ниже него пусто, чуть выше - сглаженный край
            int a = v >= 127 ? 255 : (v - 104) * 255 / 23;
            out[(size_t)y * W + x] = ((uint32_t)(fa * a / 255) << 24) | ((uint32_t)(fr * a / 255) << 16) |
                                     ((uint32_t)(fg * a / 255) << 8) | (uint32_t)(fb * a / 255);
        }
    }
}

// ---------- меню в карточке ----------

namespace {
const int kMenuMinutes[4] = {5, 10, 20, 60};
std::wstring MinutesLabel(int m) { return m == 60 ? L"1 час" : std::to_wstring(m) + L" мин"; }
}  // namespace

void Toast::OpenMenu() {
    if (!HasMenu() || !open_) return;
    menuOpen_ = true;
    menuNote_.clear();
    menuDoneAt_ = 0;
    menuAwaySince_ = 0;
    dirty_ = true;
}

void Toast::CloseMenu() {
    menuOpen_ = false;
    menuNote_.clear();
    menuDoneAt_ = 0;
    dirty_ = true;
}

float Toast::MenuHeight() const { return MenuRepeat() ? 14 + 30 + 8 + 30 + 12 + 30 + 14 : 14 + 30 + 12 + 30 + 14; }

RectF Toast::DotsRect() const {
    RectF body = Body();
    return RectF(body.GetRight() - 64 * k(), body.Y + 8 * k(), 28 * k(), 28 * k());
}

RectF Toast::MenuRect(int id) const {
    float K = k();
    RectF body = Body();
    float x = body.X + 16 * K, y = body.Y + 14 * K, right = body.GetRight() - 16 * K;
    if (id >= H_M_LATER && id < H_M_LATER + 8) {
        int row = id >= H_M_REPEAT ? 1 : 0, i = (id - H_M_LATER) % 4;
        return RectF(x + (124 + i * 56) * K, y + row * 38 * K, 50 * K, 30 * K);
    }
    float bottom = y + (MenuRepeat() ? 38 + 30 + 12 : 30 + 12) * K;
    if (id == H_M_CANCEL) return RectF(right - 76 * K, bottom, 76 * K, 30 * K);
    if (id == H_M_CLOSE) return RectF(right - (76 + 6 + 96) * K, bottom, 96 * K, 30 * K);
    if (id == H_M_MUTE) return RectF(x, bottom, 160 * K, 30 * K);
    return RectF();
}

void Toast::MenuClick(int id) {
    if (!menuNote_.empty()) return;  // итог уже показан
    if (id == H_M_CANCEL) {
        CloseMenu();
    } else if (id == H_M_CLOSE) {
        CloseMenu();
        Dismiss();
    } else if (id == H_M_MUTE && MenuMute()) {
        MenuMuteKind();
        CloseMenu();
        Dismiss();
    } else if (id >= H_M_LATER && id < H_M_LATER + 8) {
        bool repeat = id >= H_M_REPEAT;
        if (repeat && !MenuRepeat()) return;
        int m = kMenuMinutes[(id - H_M_LATER) % 4];
        if (repeat && MenuRepeatActive() == m) {  // ещё раз по той же кнопке - выключить
            MenuChoose(0, true);
            menuNote_ = L"Повтор выключен";
            menuDismiss_ = false;
        } else {
            MenuChoose(m, repeat);
            menuNote_ = (repeat ? L"Буду напоминать каждые " : L"Напомню через ") + MinutesLabel(m);
            menuDismiss_ = true;
        }
        menuDoneAt_ = GetTickCount64() + (ULONGLONG)fx::Dur(850);
        Kick(0.5f);
    }
}

void Toast::PaintMenu(Graphics& g, Color fill, float radius) {
    float K = k();
    RectF body = Body();
    DrawFaded(g, Grow(body, 2 * K), menuT_, [&](Graphics& gm) {
        GraphicsPath path;
        RoundRectPath(path, body, radius);
        SolidBrush cover(WithAlpha(fill, 255));
        gm.FillPath(&cover, &path);
        Pen rim(Color(36, 255, 255, 255), 1 * K);
        gm.DrawPath(&rim, &path);
        Color fg = ToColor(theme_.fg), muted = MixColor(ToColor(theme_.muted), fg, 0.3);
        FontFamily fam(L"Segoe UI");
        Font fL(&fam, 12.5f * K, FontStyleRegular, UnitPixel);
        StringFormat sf;
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        if (!menuNote_.empty()) {  // выбрали: короткое подтверждение вместо кнопок
            Font fN(Family(), 14.5f * K, FontStyleRegular, UnitPixel);
            StringFormat cf;
            cf.SetAlignment(StringAlignmentCenter);
            cf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush nb(fg);
            gm.DrawString(menuNote_.c_str(), -1, &fN, body, &cf, &nb);
            return;
        }
        SolidBrush lb(muted);
        float x = body.X + 16 * K;
        auto row = [&](int first, const wchar_t* label, int active) {
            RectF r0 = MenuRect(first);
            gm.DrawString(label, -1, &fL, RectF(x, r0.Y, 122 * K, r0.Height), &sf, &lb);
            for (int i = 0; i < 4; ++i)
                DrawButton(gm, MenuRect(first + i), MinutesLabel(kMenuMinutes[i]), kMenuMinutes[i] == active, first + i);
        };
        row(H_M_LATER, L"Напомнить через", 0);
        if (MenuRepeat()) row(H_M_REPEAT, L"Повторять каждые", MenuRepeatActive());
        if (MenuMute()) DrawButton(gm, MenuRect(H_M_MUTE), L"Не показывать такие", false, H_M_MUTE);
        DrawButton(gm, MenuRect(H_M_CLOSE), L"Закрыть", false, H_M_CLOSE);
        DrawButton(gm, MenuRect(H_M_CANCEL), L"Назад", false, H_M_CANCEL);
    });
}

bool Toast::SaveFrame(const std::wstring& path) const {
    if (!memDC_ || !bits_) return false;
    const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    Bitmap frame(dibW_, dibH_, PixelFormat32bppARGB);
    Graphics g(&frame);
    g.Clear(Color(255, 46, 42, 50));
    Bitmap src(dibW_, dibH_, dibW_ * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
    g.DrawImage(&src, 0, 0);
    return frame.Save(path.c_str(), &png, nullptr) == Ok;
}

// ---------------- стопка ----------------

float Toast::Presence() const { return std::clamp(open_ ? std::max(appear_, 0.35f) : appear_, 0.0f, 1.0f); }

int Toast::TargetY() const {
    float y = 14 * k();
    for (const Toast* o : All())
        if (o != this && o->order_ < order_ && (o->open_ || o->appear_ > 0) && EqualRect(&o->work_, &work_))
            y += (o->h_ + 10) * o->k() * o->Presence();
    return (int)std::lround(y);
}

// ---------------- кадр ----------------

void Toast::Tick() {
    ULONGLONG now = GetTickCount64();
    double dt = std::min<double>(100, (double)(now - last_));
    last_ = now;
    if (wnd_) Idle(now);
    if (!wnd_ || (!open_ && appear_ <= 0)) return;
    fx::Active();  // карточка на экране: таймер ухода и пружина идут от тактов опроса

    bool anim = false;
    // таймер стоит, пока на карточку смотрят или тянут её
    if (open_ && hold_ > 0 && hot_ == H_NONE && !dragging_ && !menuOpen_) {
        left_ -= dt;
        if (left_ <= 0) {
            open_ = false;
            byHand_ = false;
            Closed();
        }
    }
    // въезд - пружина с лёгким перелётом, уход - быстрое растворение
    bool liquid = LiquidMotion();
    float before = appear_;
    if (liquid) {
        StepLiquid(dt, anim);
    } else if (open_) {
        const double w = 2 * 3.14159265 / 0.46, z = 0.62;
        for (double t = dt / 1000; t > 0; t -= 0.008) {
            double h = std::min(0.008, t);
            appearV_ += (float)((w * w * (1 - appear_) - 2 * z * w * appearV_) * h);
            appear_ += (float)(appearV_ * h);
        }
        if (std::fabs(1 - appear_) < 0.0015f && std::fabs(appearV_) < 0.01f) appear_ = 1, appearV_ = 0;
    } else {
        appearV_ = 0;
        appear_ = std::max(0.0f, appear_ - (float)(dt / 190));
    }
    anim |= appear_ != before;
    for (int i = 0; i < kMaxHits; ++i) {
        float t = i == hot_ ? 1.0f : 0.0f;
        if (hover_[i] == t) continue;
        hover_[i] = std::fabs(hover_[i] - t) < 0.01f ? t : Approach(hover_[i], t, dt, i == hot_ ? 45 : 90);
        anim = true;
    }
    if (pulse_ > 0) pulse_ = std::max(0.0f, pulse_ - (float)(dt / 420)), anim = true;
    if (!liquid && !dragging_ && open_ && dragX_ > 0) {  // не добросили - пружиной назад
        dragX_ = dragX_ < 0.5f ? 0 : Approach(dragX_, 0, dt, 60);
        anim = true;
    }
    if (!liquid && !open_ && dragX_ > 0) dragX_ += (float)(dt * 2.2 * k()), anim = true;  // брошенная улетает дальше
    anim |= Animate(dt);
    {
        float mt = menuOpen_ ? 1.0f : 0.0f;
        if (menuT_ != mt) {
            menuT_ = std::fabs(menuT_ - mt) < 0.01f ? mt : Approach(menuT_, mt, dt, 60);
            anim = true;
        }
        if (menuOpen_ && menuDoneAt_ && now >= menuDoneAt_) {  // подтверждение показано
            bool go = menuDismiss_;
            CloseMenu();
            if (go) Dismiss();
            anim = true;
        } else if (menuOpen_ && !menuDoneAt_) {  // курсор ушёл надолго - меню закрывается, таймер идёт дальше
            if (hot_ != H_NONE) {
                menuAwaySince_ = 0;
            } else if (!menuAwaySince_) {
                menuAwaySince_ = now;
            } else if (now - menuAwaySince_ > 6000) {
                CloseMenu();
                anim = true;
            }
        }
    }
    float ty = (float)TargetY();
    if (!yInit_) y_ = ty, yInit_ = true;
    if (std::fabs(y_ - ty) > 0.4f)
        y_ = Approach(y_, ty, dt, 85), anim = true;
    else
        y_ = ty;

    if (appear_ <= 0 && !open_) {
        if (IsWindowVisible(wnd_)) {
            ShowWindow(wnd_, SW_HIDE);
            mem::TrimSoon();
        }
        dragX_ = 0;
        return;
    }
    // полоска таймера двигается медленно: хватает кадра в 80 мс
    bool timer = open_ && hold_ > 0 && now - lastTimerFrame_ >= 80;
    if (anim || dirty_ || Busy() || timer) Render();
}

RectF Toast::Body() const {
    float m = kM * k();
    return RectF(LiquidMotion() ? (kM + kLiqL) * k() : m, m, kW * k(), h_ * k());
}

Color Toast::Tone() const { return ToColor(theme_.ac); }

void Toast::Render() {
    lastTimerFrame_ = GetTickCount64();
    float K = k();
    if (dirty_) {
        static Bitmap* probe = new Bitmap(1, 1, PixelFormat32bppPARGB);
        Graphics mg(probe);
        mg.SetTextRenderingHint(TextRenderingHintAntiAlias);
        hT_ = std::max(64.0f, BodyHeight(mg));
        if (menuOpen_) hT_ = std::max(hT_, MenuHeight());
        if (!LiquidMotion() || !hInit_) h_ = hT_;
        hInit_ = true;
    }
    dirty_ = false;
    bool liquid = LiquidMotion();
    int W = (int)std::ceil((kW + kM * 2 + (liquid ? kLiqL : 0)) * K), H = (int)std::ceil((h_ + kM * 2) * K);
    HDC scr = GetDC(nullptr);
    if (!memDC_ || W != dibW_ || H != dibH_) {
        if (memDC_) {
            SelectObject(memDC_, oldBmp_);
            DeleteObject(dib_);
            DeleteDC(memDC_);
        }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
        memDC_ = CreateCompatibleDC(scr);
        dib_ = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        oldBmp_ = SelectObject(memDC_, dib_);
        dibW_ = W, dibH_ = H;
    }
    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), bg = ToColor(theme_.bg), tone = Tone();
        RectF body = Body();
        float rad = 16 * K;
        Color cardBase = night::On() ? MixColor(bg, Color(0, 0, 0), 0.35) : bg;
        Color cardFill = WithAlpha(MixColor(cardBase, Color(0, 0, 0), 0.12), 250);
        if (liquid) {
            // окно стоит на месте, движется рисунок; за край рабочей области не вылезает (там может быть монитор)
            int winX = work_.right - W + (int)(kM * K) - (int)(14 * K);
            int clipX = (int)work_.right - winX;
            PaintGoo(W, H, clipX, cardFill);
            g.SetClip(RectF(0, 0, (REAL)clipX, (REAL)H));
            float cx = body.X + body.Width / 2, cy = body.Y + body.Height / 2;
            g.TranslateTransform(cx + liqX_ * K, cy);
            g.ScaleTransform(s_, 1 / s_);
            g.TranslateTransform(-cx, -cy);
            rad = (16 + 46 * std::min(1.0f, std::fabs(s_ - 1) * 2.3f)) * K;
        }

        // тень: мягкие слои, гуще книзу
        for (int i = 7; i >= 1; --i) {
            GraphicsPath sp;
            RoundRectPath(sp,
                          RectF(body.X - i * 1.6f * K, body.Y - i * 1.0f * K + 7 * K, body.Width + i * 3.2f * K,
                                body.Height + i * 2.4f * K),
                          rad + i * 1.6f * K);
            SolidBrush sb(Color((BYTE)(7 + (7 - i) * 3), 0, 0, 0));
            g.FillPath(&sb, &sp);
        }
        GraphicsPath path;
        RoundRectPath(path, body, rad);
        SolidBrush fill(cardFill);
        g.FillPath(&fill, &path);
        {
            GraphicsState st = g.Save();
            g.SetClip(&path, CombineModeIntersect);
            // свет знака слева и блик сверху
            LinearGradientBrush glow(PointF(body.X - 1, 0), PointF(body.X + body.Width * 0.55f + 1, 0),
                                     WithAlpha(tone, 34), WithAlpha(tone, 0));
            g.FillRectangle(&glow, RectF(body.X, body.Y, body.Width * 0.55f, body.Height));
            LinearGradientBrush shine(PointF(0, body.Y - 1), PointF(0, body.Y + 36 * K + 1), Color(18, 255, 255, 255),
                                      Color(0, 255, 255, 255));
            g.FillRectangle(&shine, RectF(body.X, body.Y, body.Width, 36 * K));
            // полоска тона слева
            if (!liquid) {
                SolidBrush strip(tone);
                g.FillRectangle(&strip, RectF(body.X, body.Y, 3.5f * K, body.Height));
            }
            // время до ухода или бегущий блик работы
            float bh = 2.5f * K, by = body.GetBottom() - bh;
            if (Busy()) {
                double t = (GetTickCount64() % 1300) / 1300.0;
                float sw = body.Width * 0.32f, sx = body.X - sw + (float)(t * (body.Width + sw));
                LinearGradientBrush run(PointF(sx - 1, 0), PointF(sx + sw + 1, 0), WithAlpha(tone, 0),
                                        WithAlpha(tone, 0));
                Color cs[3] = {WithAlpha(tone, 0), WithAlpha(tone, 230), WithAlpha(tone, 0)};
                REAL ps[3] = {0, 0.5f, 1};
                run.SetInterpolationColors(cs, ps, 3);
                g.FillRectangle(&run, RectF(sx, by, sw, bh));
            } else if (open_ && hold_ > 0) {
                float f = (float)std::clamp(left_ / hold_, 0.0, 1.0);
                float ra = Reveal(0.3f, 0.2f);
                SolidBrush track(Color((BYTE)(16 * ra), 255, 255, 255));
                g.FillRectangle(&track, RectF(body.X, by, body.Width, bh));
                if (liquid) {  // линия жидкости с каплей на конце; на паузе по ней идёт рябь
                    float ripple = hot_ != H_NONE ? 1 + 0.5f * (float)std::sin(GetTickCount64() / 160.0) : 1;
                    float lh = 3 * K * ripple, ly = body.GetBottom() - lh, hx = body.X + body.Width * f;
                    SolidBrush lb(WithAlpha(tone, (BYTE)(235 * ra)));
                    g.FillRectangle(&lb, RectF(body.X, ly, body.Width * f, lh));
                    g.FillEllipse(&lb, hx - 4 * K, body.GetBottom() - 3.5f * K - 3.5f * K * ripple, 8 * K, 7 * K * ripple);
                } else {
                    SolidBrush bar(WithAlpha(tone, hot_ != H_NONE ? 110 : 190));
                    g.FillRectangle(&bar, RectF(body.X, by, body.Width * f, bh));
                }
            }
            g.Restore(st);
        }
        Pen rim(Color(36, 255, 255, 255), 1 * K);
        g.DrawPath(&rim, &path);
        if (pulse_ > 0) {  // обновили открытую карточку - кромка вспыхивает тоном
            Pen pp(WithAlpha(tone, (BYTE)(200 * pulse_)), 1.6f * K);
            g.DrawPath(&pp, &path);
        }

        Paint(g, body);

        // закрыть: виден ярче, когда курсор на карточке
        RectF cr(body.GetRight() - 34 * K, body.Y + 10 * K, 24 * K, 24 * K);
        float onCard = hot_ != H_NONE ? 1.0f : 0.0f;
        if (hover_[H_CLOSE] > 0) {
            SolidBrush hb(Color((BYTE)(34 * hover_[H_CLOSE]), 255, 255, 255));
            g.FillEllipse(&hb, cr);
        }
        Pen xp(MixColor(muted, fg, std::max(hover_[H_CLOSE], onCard * 0.3f),
                        (BYTE)((150 + 105 * onCard) * Reveal(0.3f, 0.2f))),
               1.5f * K);
        xp.SetStartCap(LineCapRound);
        xp.SetEndCap(LineCapRound);
        float c = 4.5f * K, cx = cr.X + cr.Width / 2, cy = cr.Y + cr.Height / 2;
        g.DrawLine(&xp, cx - c, cy - c, cx + c, cy + c);
        g.DrawLine(&xp, cx - c, cy + c, cx + c, cy - c);
        if (HasMenu()) {  // кнопка меню рядом с крестиком
            RectF dr = DotsRect();
            if (hover_[H_DOTS] > 0) {
                SolidBrush hb(Color((BYTE)(34 * hover_[H_DOTS]), 255, 255, 255));
                g.FillEllipse(&hb, RectF(dr.X + 2 * K, dr.Y + 2 * K, 24 * K, 24 * K));
            }
            SolidBrush db(MixColor(muted, fg, std::max(hover_[H_DOTS], onCard * 0.3f),
                                   (BYTE)((150 + 105 * onCard) * Reveal(0.3f, 0.2f))));
            float dcx = dr.X + dr.Width / 2, dcy = dr.Y + dr.Height / 2, dot = 1.6f * K;
            for (int i = -1; i <= 1; ++i) g.FillEllipse(&db, dcx + i * 5 * K - dot, dcy - dot, dot * 2, dot * 2);
        }
        if (menuT_ > 0) PaintMenu(g, cardFill, rad);
    }
    float a = std::clamp(appear_, 0.0f, 1.0f);
    float slide = liquid ? 0 : (1 - appear_) * 64 * K + dragX_;  // перелёт пружины даёт лёгкий заход влево
    LONG x = work_.right - W + (LONG)(kM * K) - (LONG)(14 * K) + (LONG)std::lround(slide);
    LONG y = work_.bottom - H + (LONG)(kM * K) - (LONG)std::lround(y_);
    float fade = dragX_ > 0 ? std::max(0.0f, 1 - dragX_ / (220 * K)) : 1.0f;
    POINT at{x, y}, src{0, 0};
    SIZE sz{W, H};
    if (liquid) a = 1, fade = 1;  // жидкая карточка не растворяется: она вытекает и затекает
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(255 * std::min(1.0f, a * 1.5f) * fade), AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &at, &sz, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) {
        share::Cover(wnd_);  // в показе экрана карточку видите только вы
        ShowWindow(wnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
}

// ---------------- помощники рисования ----------------

void Toast::DrawGlyph(Graphics& g, float cx, float cy, Glyph glyph, Color c) const {
    if (glyph == G_NONE) return;
    float K = k(), r = 17 * K;
    SolidBrush disc(WithAlpha(c, 46));
    g.FillEllipse(&disc, cx - r, cy - r, 2 * r, 2 * r);
    Pen ring(WithAlpha(c, 110), 1 * K);
    g.DrawEllipse(&ring, cx - r, cy - r, 2 * r, 2 * r);
    Pen p(MixColor(c, Color(255, 255, 255), 0.35), 2.1f * K);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    SolidBrush dot(MixColor(c, Color(255, 255, 255), 0.35));
    float s = 6 * K;
    switch (glyph) {
        case G_OK: {
            PointF pts[3] = {{cx - s, cy + 0.2f * K}, {cx - s * 0.3f, cy + s * 0.7f}, {cx + s, cy - s * 0.7f}};
            g.DrawLines(&p, pts, 3);
            break;
        }
        case G_FAIL:
            g.DrawLine(&p, cx - s * 0.8f, cy - s * 0.8f, cx + s * 0.8f, cy + s * 0.8f);
            g.DrawLine(&p, cx - s * 0.8f, cy + s * 0.8f, cx + s * 0.8f, cy - s * 0.8f);
            break;
        case G_WARN:
        case G_INFO: {
            bool warn = glyph == G_WARN;
            float top = warn ? cy - s : cy - s * 0.15f, bot = warn ? cy + s * 0.25f : cy + s;
            g.DrawLine(&p, cx, top, cx, bot);
            float dy = warn ? cy + s * 0.95f : cy - s * 0.95f, dr = 1.35f * K;
            g.FillEllipse(&dot, cx - dr, dy - dr, 2 * dr, 2 * dr);
            break;
        }
        case G_ASK: {
            g.DrawArc(&p, cx - s * 0.75f, cy - s * 1.15f, s * 1.5f, s * 1.3f, 180, 230);
            g.DrawLine(&p, cx + s * 0.18f, cy - s * 0.05f, cx, cy + s * 0.35f);
            float dr = 1.35f * K;
            g.FillEllipse(&dot, cx - dr, cy + s * 0.95f - dr, 2 * dr, 2 * dr);
            break;
        }
        case G_BUSY: {
            double a = (GetTickCount64() % 1000) / 1000.0 * 360;
            Pen faint(WithAlpha(c, 70), 2.1f * K);
            g.DrawEllipse(&faint, cx - s, cy - s, 2 * s, 2 * s);
            g.DrawArc(&p, cx - s, cy - s, 2 * s, 2 * s, (REAL)a, 100);
            break;
        }
        case G_NET: {
            Pen thin(MixColor(c, Color(255, 255, 255), 0.35), 1.6f * K);
            g.DrawEllipse(&thin, cx - s, cy - s, 2 * s, 2 * s);
            g.DrawEllipse(&thin, cx - s * 0.42f, cy - s, s * 0.84f, 2 * s);
            g.DrawLine(&thin, cx - s, cy, cx + s, cy);
            break;
        }
        case G_RESTORE: {
            g.DrawArc(&p, cx - s, cy - s, 2 * s, 2 * s, 200, 290);
            float ax = cx + s * (float)std::cos(Rad(200)), ay = cy + s * (float)std::sin(Rad(200));
            g.DrawLine(&p, ax, ay, ax - 0.5f * K, ay - 4.2f * K);
            g.DrawLine(&p, ax, ay, ax + 4.0f * K, ay - 0.8f * K);
            break;
        }
        default:
            break;
    }
}

float Toast::ButtonWidth(Graphics& g, const std::wstring& label) const {
    Font f(Family(), 12.5f * k(), FontStyleRegular, UnitPixel);
    RectF b;
    g.MeasureString(label.c_str(), -1, &f, PointF(0, 0), StringFormat::GenericTypographic(), &b);
    return std::max(84 * k(), b.Width + 34 * k());
}

void Toast::DrawButton(Graphics& g, const RectF& rr, const std::wstring& label, bool primary, int id) const {
    float K = k(), hv = HoverOf(id);
    RectF r = rr;
    if (Pressed(id)) r = Inset(r, 1.2f * K, 1.0f * K);
    GraphicsPath bp;
    RoundRectPath(bp, r, r.Height / 2);
    Color ac = ButtonTone(), fg = ToColor(theme_.fg);
    if (primary) {
        LinearGradientBrush bb(PointF(0, r.Y), PointF(0, r.GetBottom()),
                               MixColor(ac, Color(255, 255, 255), 0.10 + hv * 0.14),
                               MixColor(ac, Color(0, 0, 0), 0.08 - hv * 0.04));
        g.FillPath(&bb, &bp);
        Pen hi(Color((BYTE)(50 + 40 * hv), 255, 255, 255), 1 * K);
        g.DrawPath(&hi, &bp);
    } else {
        SolidBrush bb(Color((BYTE)(20 + 26 * hv), 255, 255, 255));
        g.FillPath(&bb, &bp);
        Pen bpn(Color((BYTE)(44 + 30 * hv), 255, 255, 255), 1 * K);
        g.DrawPath(&bpn, &bp);
    }
    Font f(Family(), 12.5f * K, FontStyleRegular, UnitPixel);
    SolidBrush lb(primary ? (Lum(ac) > 0.72 ? Color(255, 24, 20, 28) : Color(255, 255, 255, 255)) : fg);
    StringFormat cf;
    cf.SetAlignment(StringAlignmentCenter);
    cf.SetLineAlignment(StringAlignmentCenter);
    cf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(label.c_str(), -1, &f, r, &cf, &lb);
}

float Toast::TextHeight(Graphics& g, const std::wstring& s, const Font& f, float width) const {
    if (s.empty()) return 0;
    RectF b;
    g.MeasureString(s.c_str(), -1, &f, RectF(0, 0, width, 1000), nullptr, &b);
    return b.Height;
}

// ---------------- мышь ----------------

int Toast::HitAt(POINT p) const {
    PointF pt((REAL)p.x - liqX_ * k(), (REAL)p.y);
    RectF body = Body();
    if (menuOpen_) {
        if (menuNote_.empty()) {
            for (int id : {H_M_CANCEL, H_M_CLOSE, H_M_MUTE})
                if ((id != H_M_MUTE || MenuMute()) && MenuRect(id).Contains(pt)) return id;
            for (int id = H_M_LATER; id < H_M_LATER + (MenuRepeat() ? 8 : 4); ++id)
                if (MenuRect(id).Contains(pt)) return id;
        }
        return body.Contains(pt) ? H_BODY : H_NONE;
    }
    if (HasMenu() && DotsRect().Contains(pt)) return H_DOTS;
    RectF cr(body.GetRight() - 36 * k(), body.Y + 8 * k(), 28 * k(), 28 * k());
    if (cr.Contains(pt)) return H_CLOSE;
    int h = HitContent(pt);
    if (h != H_NONE) return h;
    return body.Contains(pt) ? H_BODY : H_NONE;
}

LRESULT CALLBACK Toast::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* self = (Toast*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self && self->wnd_ == h) return self->Handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT Toast::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    auto close = [this] { Dismiss(); };
    if (Message(msg, wp, lp)) return 0;
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // не отнимать фокус у игры
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, dragging_                          ? IDC_SIZEWE
                                           : hot_ == H_BODY || hot_ == H_NONE ? IDC_ARROW
                                                                              : IDC_HAND));
            return TRUE;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, wnd_, 0};
                TrackMouseEvent(&t);
                tracking_ = true;
            }
            if (down_ != H_NONE && (wp & MK_LBUTTON) && open_ && !menuOpen_) {
                POINT s;
                GetCursorPos(&s);
                float dx = s.x - dragStart_;
                if (!dragging_ && dx > 8 * k()) dragging_ = true;
                if (dragging_) {
                    dragX_ = std::max(0.0f, dx);
                    return 0;
                }
            }
            int h = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            hot_ = h;
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!dragging_) hot_ = H_NONE;
            return 0;
        case WM_LBUTTONDOWN: {
            down_ = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            POINT s;
            GetCursorPos(&s);
            dragStart_ = (float)s.x;
            SetCapture(wnd_);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int d = down_;
            down_ = H_NONE;
            if (dragging_) {
                dragging_ = false;
                if (dragX_ > 90 * k()) close();  // бросили вправо
                return 0;
            }
            int h = HitAt({(short)LOWORD(lp), (short)HIWORD(lp)});
            if (h != d || h == H_NONE) return 0;
            if (menuOpen_)
                MenuClick(h);
            else if (h == H_DOTS)
                OpenMenu();
            else if (h == H_CLOSE)
                close();
            else
                Click(h);
            return 0;
        }
        case WM_RBUTTONUP:
            if (menuOpen_)
                CloseMenu();
            else if (HasMenu())
                OpenMenu();
            else
                close();
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging_) dragging_ = false;
            return 0;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}
