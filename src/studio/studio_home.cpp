// Раздел «Обзор / Разделы» - карта экрана: маленький монитор, на котором видно, где программа живёт.
// Кольца - ручки у краёв (значки секторов выезжают), углы - уголки акцента, тема - обои и окно с рамкой
// (миниатюры тем в окне: примерка и смена волной), панель задач - сеть, игры, автозапуск; под монитором -
// диски, PATH и утилиты. Набросок - art/sections_sketch.html, направление 1. Кадры - только пока что-то
// движется; сломанный путь дышит редкими кадрами; в игровом режиме всё сразу на местах.
#include <windows.h>

#include <algorithm>
#include <cmath>

#include "app/autostart.h"
#include "core/files.h"
#include "core/fx.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/corners.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;

namespace studio {

std::function<std::wstring()> g_netStatus;

namespace {

// предметы карты: рамки попадания и их анимации (+50)
constexpr int kEdge = ID_HOME_TILE;        // +край
constexpr int kCorner = ID_HOME_TILE + 2;  // +угол
constexpr int kThumb = ID_HOME_TILE + 6;   // +миниатюра
constexpr int kTask = ID_HOME_TILE + 14;   // +пункт панели задач
constexpr int kShelf = ID_HOME_TILE + 17;  // +предмет полки
constexpr int kTip = ID_HOME_TILE + 49;    // подсказка (только анимация)

const wchar_t* kWhere[] = {L"слева вверху", L"справа вверху", L"слева внизу", L"справа внизу"};

// доля пути от delay до delay + dur (мс) на часах появления
float Span(double t, double delay, double dur) { return (float)Clamp01((t - delay) / dur); }

// пружина к цели мелкими шагами; true - ещё едет
bool Spring(Mover& m, float to, float dt, float k = 320, float d = 17) {
    if (fx::Game()) {
        m.x = to, m.vx = 0;
        return false;
    }
    for (float left = dt; left > 0;) {
        float h = std::min(left, 1 / 240.0f);
        left -= h;
        m.vx += ((to - m.x) * k - m.vx * d) * h;
        m.x += m.vx * h;
    }
    if (std::fabs(to - m.x) < 0.002f && std::fabs(m.vx) < 0.03f) {
        m.x = to, m.vx = 0;
        return false;
    }
    return true;
}

}  // namespace

// Нарисовать кусок прозрачным, увеличенным и сдвинутым: в свой холст, потом одной картинкой.
// Только пока он появляется; на месте - прямо в кадр.
void Studio::Fading(const RectF& r, float alpha, float scale, float dy, const std::function<void()>& draw) {
    if (alpha <= 0.004f) return;
    if (alpha >= 0.996f && std::fabs(scale - 1) < 0.002f && std::fabs(dy) < 0.3f) {
        draw();
        return;
    }
    float pad = ui.S(44);
    int bw = (int)std::ceil(r.Width + 2 * pad), bh = (int)std::ceil(r.Height + 2 * pad);
    Bitmap layer(bw, bh, PixelFormat32bppPARGB);
    {
        Graphics lg(&layer);
        lg.SetSmoothingMode(SmoothingModeAntiAlias);
        lg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        lg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        lg.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        lg.TranslateTransform(pad - r.X, pad - r.Y);
        Graphics* screen = ui.Redirect(&lg);
        draw();
        ui.Redirect(screen);
    }
    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
    ImageAttributes ia;
    ia.SetColorMatrix(&cm);
    float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2 + dy;
    RectF dst(cx - bw / 2.0f * scale, cy - bh / 2.0f * scale, bw * scale, bh * scale);
    ui.G().DrawImage(&layer, dst, 0, 0, (REAL)bw, (REAL)bh, UnitPixel, &ia);
}

// Метка-число: жёлтый кружок с ореолом; scale - появление и исчезновение.
void Studio::HomeBadge(float cx, float cy, int n, float scale) {
    if (scale <= 0.02f) return;
    std::wstring s = std::to_wstring(n);
    float h = ui.S(24) * scale, w = std::max(h, (ui.Measure(s, 12.5f, true) + ui.S(14)) * scale);
    RectF r(cx - w / 2, cy - h / 2, w, h);
    const UiColors& c = ui.C();
    ui.Fill(Grow(r, ui.S(4) * scale), WithAlpha(c.warn, 64), h / 2 + ui.S(4) * scale);
    ui.Fill(r, c.warn, h / 2);
    if (scale > 0.6f) ui.Text(s, r, Color(255, 23, 18, 10), 12.5f * scale, true, ui::Center);
}

// Подсказка у предмета карты: имя крупно, ниже - что делает; рисуется в конце страницы, поверх всего.
void Studio::HomeTipAt(const RectF& anchor, const std::wstring& title, const std::wstring& sub, bool warn, int place) {
    homeTip_.title = title;
    homeTip_.sub = sub;
    homeTip_.warn = warn;
    homeTip_.at = anchor;
    homeTip_.place = place;
    homeTip_.want = true;
}

// Ручка края: полоска акцента у кромки экрана; наведение - значки секторов выезжают из края.
// Пропавший путь - ручка жёлтая и тихо дышит, щелчок ищет папку; нашлась - ручка вспыхивает зелёным.
void Studio::HomeEdge(int side, const RectF& scr, double t) {
    const UiColors& c = ui.C();
    const std::vector<Item>& ring = Ring(side);
    float ew = ui.S(58), top = scr.Y + scr.Height * 0.22f, eh = scr.Height * 0.56f;
    RectF er(side ? scr.X + scr.Width - ew : scr.X, top, ew, eh);
    int id = kEdge + side;
    bool click = ui.Click(id, er), hot = ui.Hot(id);
    const Problem* bad = nullptr;
    for (const Problem& p : problems_)
        if (p.ring == side) {
            bad = &p;
            break;
        }

    // появление: ручка выезжает из-за края
    float in = Span(t, 900 + side * 120, 500);
    float shift = ui.S(40) * (1 - (float)EaseOutBack(in, 1.4)) * (side ? 1 : -1);
    bool intro = t > 1100 + side * 120 && t < 2000 + side * 120;

    // значки: каждый со своей задержкой, на закрытии уходят в обратном порядке
    int n = (int)ring.size();
    float gap = ui.S(8), is = ui.S(30);
    if (n > 0 && n * (is + gap) - gap > eh) is = std::max(ui.S(16), (eh + gap) / n - gap);
    int shown = std::min(n, (int)((eh + gap) / (is + gap)));
    float stagger = 0.125f, full = 1 + stagger * std::max(0, shown - 1);
    float want = hot || intro ? full : 0;
    float& open = homeOpen_[side];
    if (fx::Game())
        open = want;
    else if (open != want) {
        float step = ldt_ / 0.4f;
        open = want > open ? std::min(want, open + step) : std::max(want, open - step);
        ui.Invalidate();
    }
    float colH = shown * (is + gap) - gap, y0 = top + (eh - colH) / 2;
    for (int i = 0; i < shown; ++i) {
        float e = std::clamp(open - i * stagger, 0.0f, 1.0f);
        if (e <= 0.001f) continue;
        float dx = ui.S(40) * (1 - (float)EaseOutBack(e, 1.5)) * (side ? 1 : -1);
        RectF ir(er.X + (ew - is) / 2 + dx + shift, y0 + i * (is + gap), is, is);
        float a = std::min(1.0f, e / 0.75f) * in;
        bool broken = false;
        for (const Problem& p : problems_) broken |= p.ring == side && p.i == i;
        if (broken) {  // сломанный значок светится
            for (int k = 3; k >= 1; --k)
                ui.Fill(Grow(ir, k * ui.S(3)), WithAlpha(c.warn, (BYTE)(a * (hot ? 46 : 26))), is / 2 + k * ui.S(3));
        }
        // тень под значком, чтобы не терялся на светлых обоях
        ui.Fill(RectF(ir.X + ui.S(2), ir.Y + ui.S(4), ir.Width - ui.S(4), ir.Height - ui.S(2)),
                Color((BYTE)(70 * a), 0, 0, 0), is / 2);
        IconDrawn(IconFile(ring[i]), ring[i].full, ir, a);
    }

    // ручка
    float tw = ui.S(5), th = eh * 0.4f;
    RectF tab(side ? scr.X + scr.Width - tw : scr.X, top + (eh - th) / 2, tw, th);
    tab.X += shift;
    Color col = c.accent;
    float glow = 1, alpha = 1;
    if (bad) {
        col = c.warn;
        if (!fx::Game()) {
            double b = (1 - std::cos(2 * kPi * clock_ / 2.4)) / 2;  // дыхание: 2,4 с на вдох и выдох
            glow = 1 - 0.7f * (float)b;
            alpha = 1 - 0.3f * (float)b;
            RepaintIn(90);
        }
    }
    float heal = fx::Game() ? 0 : 1 - (float)Clamp01((clock_ - homeHealAt_[side]) / 1.0);
    if (heal > 0) {
        col = MixColor(col, Color(255, 120, 205, 160), heal);
        glow += 0.6f * heal;
        ui.Invalidate();
    }
    if (in > 0.01f) {
        for (int k = 3; k >= 1; --k)
            ui.Fill(Grow(tab, k * ui.S(3.5f) * glow), WithAlpha(col, (BYTE)(in * alpha * 34 * glow)),
                    tw / 2 + k * ui.S(3.5f) * glow);
        ui.Fill(tab, WithAlpha(col, (BYTE)(255 * in * alpha)), tw / 2);
    }

    if (hot) {
        std::wstring title =
            std::wstring(side ? L"Правый" : L"Левый") + L" край: " + Plural(n, L"сектор", L"сектора", L"секторов");
        std::wstring sub = L"Alt + курсор к краю";
        if (bad) {
            const Item& it = bad->j < 0 ? ring[bad->i] : ring[bad->i].children[bad->j];
            sub = L"«" + it.name +
                  (bad->kind == 0   ? L"»: путь пропал, щелчок - найти"
                   : bad->kind == 1 ? L"»: ничего не открывает"
                                    : L"»: раскрывается, но это не папка");
        }
        HomeTipAt(er, title, sub, bad != nullptr, side ? 1 : 0);
    }
    if (click) {
        if (bad && bad->kind == 0) {
            Problem p = *bad;
            size_t before = problems_.size();
            FixProblem(p);  // нашлась - Save перепроверит пути; нет - спросит папку
            if (problems_.size() < before) homeHealAt_[side] = clock_;
        } else {
            Defer([this, side] {
                ring_ = side;
                sel_ = 0;
                GoPage(PAGE_RINGS);
            });
        }
    }
}

// Уголки цвета акцента; наведение - что делает угол, щелчок - раздел «Углы».
void Studio::HomeCorners(const RectF& scr, float taskH, double t) {
    const UiColors& c = ui.C();
    float hs = ui.S(46);
    for (int k = 0; k < 4; ++k) {
        bool right = k & 1, bottom = k >= 2;
        float x = right ? scr.X + scr.Width - hs : scr.X, y = bottom ? scr.Y + scr.Height - taskH - hs : scr.Y;
        RectF hr(x, y, hs, hs);
        int id = kCorner + k;
        bool click = ui.Click(id, hr);
        float hov = ui.Anim(id + 50, ui.Hot(id), 18);
        bool set = !cfg.corners[k].empty();
        float in = Span(t, 1150 + k * 90, 400);
        if (in <= 0.01f) continue;
        float s = (float)EaseOutBack(in, 1.8) * 0.7f + 0.3f;
        // точка угла и плечи от неё; масштаб появления - от самой точки
        float ox = right ? hr.X + hr.Width - ui.S(6) : hr.X + ui.S(6);
        float oy = bottom ? hr.Y + hr.Height - ui.S(6) : hr.Y + ui.S(6);
        float arm = (ui.S(30) + ui.S(8) * hov) * s, rad = std::min(ui.S(14) * s, arm);
        float sx = right ? -1.0f : 1.0f, sy = bottom ? -1.0f : 1.0f;
        BYTE a = (BYTE)(255 * in * (set ? 0.85f + 0.15f * hov : 0.35f + 0.3f * hov));
        auto stroke = [&](Color col, float width) {
            Pen p(col, width);
            p.SetStartCap(LineCapRound);
            p.SetEndCap(LineCapRound);
            ui.G().DrawLine(&p, ox, oy + sy * arm, ox, oy + sy * rad);
            ui.G().DrawLine(&p, ox + sx * rad, oy, ox + sx * arm, oy);
            RectF ar(right ? ox - 2 * rad : ox, bottom ? oy - 2 * rad : oy, 2 * rad, 2 * rad);
            float start = right ? (bottom ? 0.0f : 270.0f) : (bottom ? 90.0f : 180.0f);
            ui.G().DrawArc(&p, ar, start, 90);
        };
        if (set) stroke(WithAlpha(c.accent, (BYTE)(a * 0.22f)), ui.S(9));
        stroke(WithAlpha(c.accent, a), ui.S(3));
        if (ui.Hot(id))
            HomeTipAt(hr, set ? corners::ActionName(cfg.corners[k]) : std::wstring(L"Ничего не назначено"),
                      std::wstring(L"курсор в угол ") + kWhere[k], false, right ? 1 : 0);
        if (click) Defer([this] { GoPage(PAGE_DESK); });
    }
}

// Окно посреди экрана - это тема: рамка акцента и есть рамка окон. Имя, настроение, четыре цвета и шесть
// миниатюр: наведение - вся студия примеряет тему, щелчок - новая тема расходится волной от миниатюры.
RectF Studio::HomeWindowRect(const RectF& scr, float taskH) const {
    float ww = std::min(ui.S(420), scr.Width - ui.S(200)), wh = std::min(ui.S(210), scr.Height - taskH - ui.S(40));
    return RectF(scr.X + (scr.Width - ww) / 2, scr.Y + (scr.Height - taskH - wh) / 2, ww, wh);
}

void Studio::HomeWindow(const RectF& scr, float taskH, double t) {
    const UiColors& c = ui.C();
    const auto& themes = Themes();
    RectF wr = HomeWindowRect(scr, taskH);
    float in = Span(t, 1400, 560);
    if (in <= 0.004f) return;

    // шесть пейзажей; если выбран другой - он вместо шестого
    std::vector<int> pick;
    int cur = -1;
    for (int i = 0; i < (int)themes.size(); ++i) {
        if (cfg.theme == themes[i].key) cur = i;
        if (pick.size() < 6 && wcscmp(themes[i].key, L"wallpaper") && wcscmp(themes[i].key, L"custom"))
            pick.push_back(i);
    }
    if (cur >= 0 && std::find(pick.begin(), pick.end(), cur) == pick.end() && !pick.empty()) pick.back() = cur;
    ThemeInfo tt = ShownTheme();

    float lift = 1 - (float)EaseOutBack(in, 1.3);
    Fading(wr, std::min(1.0f, in * 1.6f), 0.94f + 0.06f * std::min(1.0f, (float)EaseOutBack(in, 1.3)), ui.S(24) * lift,
           [&] {
               float rad = ui.S(10);
               if (!homeHaloBaked_) {  // тень и свечение акцента вокруг рамки (на месте - в картинке монитора)
                   Halo(wr, rad, Color(120, 0, 0, 0), ui.S(14), ui.S(14));
                   Halo(wr, rad, WithAlpha(c.accent, 130), ui.S(14));
               }
               ui.Fill(wr, WithAlpha(c.card, 238), rad);
               float tbh = ui.S(30);
               SolidBrush tb(c.cardHi);
               ui.G().FillPath(&tb, ui.Round(RectF(wr.X, wr.Y, wr.Width, tbh), rad, true, false));
               ui.Icon(ExeDir() + L"\\icons\\hub.png", RectF(wr.X + ui.S(10), wr.Y + ui.S(7), ui.S(16), ui.S(16)));
               ui.Text(L"Тема", RectF(wr.X + ui.S(34), wr.Y, ui.S(120), tbh), c.text, 12);
               ui.Text(L"-     \x25A1     \x00D7", RectF(wr.X, wr.Y, wr.Width - ui.S(12), tbh), c.muted, 11.5f, false,
                       ui::Right);
               ui.Stroke(wr, c.accent, rad, ui.S(2));

               float px = wr.X + ui.S(16), py = wr.Y + tbh + ui.S(12), pw = wr.Width - ui.S(32);
               ui.Text(tt.name, RectF(px, py, pw, ui.S(30)), c.text, 19, true);
               ui.Text(ThemeTagline(tt.key), RectF(px, py + ui.S(30), pw, ui.S(18)), c.muted, 11.5f);
               COLORREF pal[] = {tt.ac, tt.glow, tt.gold, tt.bg};
               for (int k = 0; k < 4; ++k) {
                   RectF sr(px + k * ui.S(28), py + ui.S(56), ui.S(22), ui.S(22));
                   ui.Fill(sr, ToColor(pal[k]), ui.S(6));
                   ui.Stroke(sr, Color(52, 255, 255, 255), ui.S(6));
               }
               // миниатюры: под курсором растут от нижнего края пружиной
               float th = ui.S(36), tw = ui.S(54), gap = ui.S(7);
               float ty = std::min(py + ui.S(90), wr.Y + wr.Height - th - ui.S(12));
               int hotI = -1;
               for (int i = 0; i < (int)pick.size(); ++i) {
                   RectF tr(px + i * (tw + gap), ty, tw, th);
                   if (tr.X + tr.Width > wr.X + wr.Width - ui.S(8)) break;
                   int id = kThumb + i;
                   const ThemeInfo& ti = themes[pick[i]];
                   bool on = cfg.theme == ti.key;
                   if (ui.Click(id, tr) && !on) ThemeWave(ti.key, PointF(tr.X + tr.Width / 2, tr.Y + tr.Height / 2));
                   if (ui.Hot(id)) {
                       hotI = i;
                       if (!on) hoverThemeNext_ = ti.key;
                   }
                   if (Spring(homeThumbs_[i], ui.Hot(id) ? 1.0f : 0.0f, ldt_)) ui.Invalidate();
               }
               // наведённая поверх соседей
               auto thumb = [&](int i) {
                   const ThemeInfo& ti = themes[pick[i]];
                   float s = 1 + 0.18f * homeThumbs_[i].x;
                   RectF base(px + i * (tw + gap), ty, tw, th);
                   if (base.X + base.Width > wr.X + wr.Width - ui.S(8)) return;
                   RectF r2(base.X + tw / 2 - tw * s / 2, ty + th - th * s, tw * s, th * s);
                   ThemePicture(ThemeAs(ti.key), r2, ui.S(7), 1);
                   bool on = cfg.theme == ti.key;
                   ui.Stroke(r2, on ? c.accent : Color(40, 255, 255, 255), ui.S(7), on ? ui.S(2) : 1);
               };
               for (int i = 0; i < (int)pick.size(); ++i)
                   if (i != hotI) thumb(i);
               if (hotI >= 0) thumb(hotI);
           });
}

// Панель задач: сеть с задержкой, игровой режим и автозапуск - щелчком; выключенный автозапуск - с меткой.
void Studio::HomeTaskbar(const RectF& scr, float taskH, double t) {
    const UiColors& c = ui.C();
    float in = Span(t, 1250, 450);
    float dy = taskH * 1.05f * (1 - (float)EaseOutBack(in, 1.2));
    if (in <= 0.001f) return;
    RectF bar(scr.X, scr.Y + scr.Height - taskH + dy, scr.Width, taskH);
    SolidBrush barFill(WithAlpha(c.win, 214));
    ui.G().FillPath(&barFill, ui.Round(bar, ui.S(8), false, true));  // низ экрана скруглён
    ui.Fill(RectF(bar.X, bar.Y, bar.Width, 1), Color(22, 255, 255, 255));

    // сеть: «Интернет есть | Швеция | 190 мс» - в панели задержка, в подсказке остальное
    std::wstring net = g_netStatus ? g_netStatus() : L"";
    std::vector<std::wstring> parts;
    for (size_t at = 0; at <= net.size();) {
        size_t bar2 = net.find(L" | ", at);
        std::wstring p = net.substr(at, bar2 == std::wstring::npos ? std::wstring::npos : bar2 - at);
        if (!p.empty()) parts.push_back(p);
        if (bar2 == std::wstring::npos) break;
        at = bar2 + 3;
    }
    std::wstring ms;
    for (const std::wstring& p : parts)
        if (p.find(L"мс") != std::wstring::npos) ms = p;
    bool netBad = net.rfind(L"Нет", 0) == 0;
    bool game = GameStatus().rfind(L"включён", 0) == 0;
    if (homeAutoWas_ && !homeAuto_) homeAutoAt_ = clock_;
    homeAutoWas_ = homeAuto_;

    struct Ti {
        const wchar_t* icon;
        std::wstring text;
        bool off;
    } items[] = {
        {L"net-globe.png",
         !ms.empty()     ? ms
         : parts.empty() ? std::wstring(L"сеть")
                         : parts[0],
         false},
        {L"game.png", game ? L"игры: вкл" : L"игры", !game},
        {L"app.png", homeAuto_ ? L"автозапуск" : L"автозапуск выкл", !homeAuto_},
    };
    float x = bar.X + ui.S(10), ih = ui.S(28);
    for (int k = 0; k < 3; ++k) {
        const Ti& ti = items[k];
        float w = ui.S(8) + ui.S(20) + ui.S(6) + ui.Measure(ti.text, 11.5f) + ui.S(10);
        RectF r(x, bar.Y + (taskH - ih) / 2, w, ih);
        x += w + ui.S(6);
        int id = kTask + k;
        bool click = ui.Click(id, r);
        float hov = ui.Anim(id + 50, ui.Hot(id), 16);
        if (hov > 0.01f) ui.Fill(r, WithAlpha(c.text, (BYTE)(31 * hov)), ui.S(7));
        float a = ti.off ? 0.45f : 1;
        IconDrawn(ExeDir() + L"\\icons\\" + ti.icon, L"", RectF(r.X + ui.S(8), r.Y + ui.S(4), ui.S(20), ui.S(20)), a);
        Color tc = k == 0 && netBad ? c.warn : c.text;
        ui.Text(ti.text, RectF(r.X + ui.S(34), r.Y, w - ui.S(34), ih), WithAlpha(tc, (BYTE)(255 * a)), 11.5f, k == 0);
        if (k == 2 && !homeAuto_) {
            float pop = fx::Game() ? 1 : (float)EaseOutBack(Clamp01((clock_ - homeAutoAt_) / 0.55), 1.9);
            if (pop < 1) ui.Invalidate();
            HomeBadge(r.X + r.Width - ui.S(2), r.Y, 1, pop);
        }
        if (ui.Hot(id)) {
            if (k == 0) {
                std::wstring sub;
                for (size_t i = 1; i < parts.size(); ++i)
                    if (parts[i] != ms) sub += (sub.empty() ? L"" : L", ") + parts[i];
                HomeTipAt(r, parts.empty() ? std::wstring(L"Сторож сети выключен") : parts[0],
                          sub.empty() ? std::wstring(L"щелчок - раздел «Сеть»") : sub, netBad, 2);
            } else if (k == 1) {
                HomeTipAt(r, L"Игровой режим", game ? L"щелчок - выключить" : L"щелчок - включить", false, 2);
            } else {
                HomeTipAt(r, L"Автозапуск", homeAuto_ ? L"щелчок - выключить" : L"выключен: щелчок - включить",
                          !homeAuto_, 2);
            }
        }
        if (!click) continue;
        if (k == 0) {
            Defer([this] { GoPage(PAGE_NET); });
        } else if (k == 1) {
            GameToggle();
            ui.Invalidate();
        } else {
            SetAutostart(!homeAuto_);
            homeAuto_ = AutostartOn();
            Log(homeAuto_ ? L"Автозапуск включён" : L"Автозапуск выключен");
        }
    }
    ui.Text(L"Alt + край - кольца", RectF(bar.X, bar.Y, bar.Width - ui.S(12), taskH), c.muted, 11, false, ui::Right);
}

// Полка под монитором: диски полосой, PATH с меткой и починкой, утилиты.
void Studio::HomeShelf(const RectF& r, double t) {
    const UiColors& c = ui.C();
    float in = Span(t, 1600, 500);
    Fading(r, in, 1, ui.S(20) * (1 - (float)EaseOutBack(in, 1.3)), [&] {
        ui.Fill(r, c.card, ui.S(14));
        ui.Stroke(r, c.border, ui.S(14));
        // диски
        std::wstring disks;
        bool warn = false;
        for (const tools::Volume& v : homeVols_) {
            disks += (disks.empty() ? L"" : L"   ") + v.root.substr(0, 2) + L" " + tools::Bytes(v.free);
            warn = warn || tools::DiskWarn(v);
        }
        if (disks.empty()) disks = L"диски";
        float used =
            homeVols_.empty() || !homeVols_[0].total ? 0 : 1 - (float)homeVols_[0].free / (float)homeVols_[0].total;
        std::wstring tool = L"Сервис, Утилиты";
        for (int k = 0; k < 4; ++k)
            if (cfg.corners[k] == L"tools") tool = std::wstring(L"кольцо по углу ") + kWhere[k];
        std::wstring pathSub =
            homePathBad_ ? Plural(homePathBad_, L"лишняя запись", L"лишние записи", L"лишних записей") : L"всё чисто";
        struct It {
            const wchar_t* icon;
            std::wstring title, sub;
            float w;
        } items[] = {
            {L"tools-disk.png", disks, L"", std::max(ui.Measure(disks, 12.5f, true), ui.S(120))},
            {L"tools-path.png", L"PATH", pathSub, std::max(ui.Measure(L"PATH", 12.5f, true), ui.Measure(pathSub, 11))},
            {L"terminal.png", L"Утилиты", tool, std::max(ui.Measure(L"Утилиты", 12.5f, true), ui.Measure(tool, 11))},
        };
        float sum = 0;
        for (const It& it : items) sum += ui.S(16) + ui.S(30) + ui.S(10) + it.w;
        float pad = ui.S(18), gap = std::max(ui.S(18), (r.Width - 2 * pad - sum) / 2);
        float x = r.X + pad, ih = ui.S(48);
        for (int k = 0; k < 3; ++k) {
            const It& it = items[k];
            float w = ui.S(16) + ui.S(30) + ui.S(10) + it.w;
            RectF ir(x, r.Y + (r.Height - ih) / 2, w, ih);
            x += w + gap;
            int id = kShelf + k;
            bool click = ui.Click(id, ir);
            float hov = ui.Anim(id + 50, ui.Hot(id), 16);
            if (hov > 0.01f) ui.Fill(ir, WithAlpha(c.cardHi, (BYTE)(255 * hov)), ui.S(9));
            ui.Icon(ExeDir() + L"\\icons\\" + it.icon, RectF(ir.X + ui.S(8), ir.Y + ui.S(9), ui.S(30), ui.S(30)));
            float tx = ir.X + ui.S(48);
            if (k == 0) {
                ui.Text(it.title, RectF(tx, ir.Y + ui.S(6), it.w + ui.S(4), ui.S(20)), c.text, 12.5f, true);
                RectF track(tx, ir.Y + ui.S(30), ui.S(120), ui.S(6));
                ui.Fill(track, c.cardHi, ui.S(3));
                float grow = fx::Game() ? 1 : (float)EaseOut(Span(t, 1300, 1000));
                if (grow * used > 0.005f)
                    ui.Fill(RectF(track.X, track.Y, track.Width * used * grow, track.Height), warn ? c.warn : c.accent2,
                            ui.S(3));
            } else {
                ui.Text(it.title, RectF(tx, ir.Y + ui.S(4), it.w + ui.S(4), ui.S(20)), c.text, 12.5f, true);
                bool bad = k == 1 && homePathBad_;
                ui.Text(it.sub, RectF(tx, ir.Y + ui.S(24), it.w + ui.S(4), ui.S(18)), bad ? c.warn : c.muted, 11);
            }
            if (k == 1) {  // метка PATH: появилась - выскочила, почистили - сжалась
                float bx = ir.X + ui.S(32), by = ir.Y + ui.S(2);
                if (homePathBad_) {
                    HomeBadge(bx, by, homePathBad_, 1);
                } else {
                    float gone = fx::Game() ? 1 : (float)Clamp01((clock_ - homePathAt_) / 0.35);
                    if (gone < 1) {
                        HomeBadge(bx, by, std::max(1, homePathGone_), 1 - (float)(gone * gone));
                        ui.Invalidate();
                    }
                }
            }
            if (ui.Hot(id)) {
                if (k == 0)
                    HomeTipAt(ir, warn ? L"Места мало" : L"Диски",
                              warn ? L"посмотри, что его занимает" : L"щелчок - что занимает место", warn, 2);
                else if (k == 1)
                    HomeTipAt(
                        ir, L"PATH",
                        homePathBad_ ? L"мёртвые папки и дубли: щелчок - почистить" : L"мёртвых папок и дублей нет",
                        homePathBad_ > 0, 2);
                else
                    HomeTipAt(ir, L"Утилиты", L"PATH, диски, автозагрузка, порты", false, 2);
            }
            if (!click) continue;
            if (k == 0) {
                Defer([this] { GoTools(1, L""); });
            } else if (k == 1 && homePathBad_) {
                int was = homePathBad_;
                tools::CleanUserPath([](const std::wstring&) {});
                homePathBad_ = 0;
                for (const tools::PathEntry& e : tools::ReadPath()) homePathBad_ += e.dead || e.dup;
                if (homePathBad_ < was) Log(L"PATH почищен: убрано " + std::to_wstring(was - homePathBad_));
                if (!homePathBad_) {
                    homePathGone_ = was;
                    homePathAt_ = clock_;
                } else {
                    Defer([this] { GoTools(0, L""); });  // остались записи системного PATH - чистить с правами
                }
            } else if (k == 1) {
                Defer([this] { GoTools(0, L""); });
            } else {
                Defer([this] { GoPage(PAGE_TOOLS); });
            }
        }
    });
}

void Studio::HomePage(const RectF& r) {
    LaneTick();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(in, L"Обзор", L"Программа на твоём экране: кольца у краёв, углы в углах, тема на обоях и рамке окна.");

    // данные: дешёвое - каждый кадр, PATH и диски - не чаще раза в 30 с
    ULONGLONG now = GetTickCount64();
    if (!homeAt_ || now - homeAt_ > 30000) {
        homeAt_ = now;
        homeVols_ = tools::Volumes();
        homePathBad_ = 0;
        for (const tools::PathEntry& e : tools::ReadPath()) homePathBad_ += e.dead || e.dup;
        homeAuto_ = AutostartOn();
    }

    // включение: экран загорается полосой, обои проявляются, выезжают ручки, загораются углы, появляется окно
    if (homeBoot_ < 0 || fx::Game()) homeBoot_ = fx::Game() ? clock_ - 100 : clock_;
    double t = (clock_ - homeBoot_) * 1000;
    if (t < 2300) ui.Invalidate();

    // монитор по середине: всё, что ниже заголовка, - монитор, подставка и полка
    float top = in.Y + ui.S(70), avail = in.Y + in.Height - top;
    float shelfH = ui.S(74), under = ui.S(38);
    float mh = std::min(ui.S(430), avail - under - shelfH - ui.S(4)), mw = std::min(ui.S(820), in.Width - ui.S(80));
    if (mw / mh > 1.91f)
        mw = mh * 1.91f;
    else
        mh = mw / 1.91f;
    float mx = in.X + (in.Width - mw) / 2, my = top + std::max(0.0f, (avail - mh - under - shelfH) / 3);
    RectF mon(mx, my, mw, mh);

    RectF stand(mx + mw / 2 - ui.S(60), my + mh - 1, ui.S(120), ui.S(26));
    RectF scr = Inset(mon, ui.S(12), ui.S(12));
    float rad = ui.S(8), taskH = ui.S(38);
    RectF wr = HomeWindowRect(scr, taskH);
    ThemeInfo shown = ShownTheme();
    float wall = Span(t, 350, 1400);

    // подставка, тень и рамка монитора
    auto chrome = [&] {
        LinearGradientBrush lg(Grow(stand, 1), Color(255, 26, 28, 36), Color(255, 13, 14, 18),
                               LinearGradientModeVertical);
        ui.G().FillPath(&lg, ui.Round(stand, ui.S(10), false, true));
        Halo(mon, ui.S(16), Color(130, 0, 0, 0), ui.S(18), ui.S(26));
        ui.Fill(mon, Color(255, 5, 6, 8), ui.S(16));
        ui.Stroke(mon, Color(255, 42, 46, 60), ui.S(16));
    };
    // обои - картинка темы (или примеряемой), новая растворяется с наездом; поверх затемнение
    auto wallpaper = [&] {
        if (wall < 1) ui.Fill(scr, Color(255, 0, 0, 0), rad);
        ThemeFade(homeWall_, shown, scr, rad, 1.06f, 1 + 0.12f * (1 - (float)EaseOut(wall)));
        LinearGradientBrush dim(Grow(scr, 1), Color(38, 0, 0, 0), Color(115, 0, 0, 0), LinearGradientModeVertical);
        ui.G().FillPath(&dim, ui.Round(scr, rad));
        if (wall < 1) ui.Fill(scr, Color((BYTE)(255 * (1 - EaseOut(wall))), 0, 0, 0), rad);
    };
    // всё устоялось - монитор с обоями, тенью и свечением окна одной непрозрачной картинкой: большие
    // полупрозрачные заливки стоили бо́льшую часть кадра, а меняются они только с темой и цветами
    homeHaloBaked_ = wall >= 1 && Span(t, 1400, 560) >= 1 && homeWall_.key == shown.key &&
                     (fx::Game() || clock_ - homeWall_.at >= 1.4);
    if (homeHaloBaked_) {
        RectF area = Grow(mon, ui.S(40));
        area.Height += ui.S(30);
        Baked(L"monitor", ArtKey(shown) + UiKey(ui.C()), area, ui.C().win, [&] {
            chrome();
            wallpaper();
            Halo(wr, ui.S(10), Color(120, 0, 0, 0), ui.S(14), ui.S(14));
            Halo(wr, ui.S(10), WithAlpha(ui.C().accent, 130), ui.S(14));
        });
    } else {
        chrome();
    }

    // экран режется прямоугольником (обрезка по контуру в GDI+ дорогая), скругляют углы сами обои и затемнение
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(scr, CombineModeIntersect);
    if (!homeHaloBaked_) wallpaper();

    homeTip_.want = false;
    HomeEdge(0, scr, t);
    HomeEdge(1, scr, t);
    HomeCorners(scr, taskH, t);
    HomeWindow(scr, taskH, t);
    HomeTaskbar(scr, taskH, t);

    // полоса включения: тянется по ширине, потом раскрывается на весь экран и гаснет
    if (t < 900) {
        float p = (float)(t / 900);
        float sx = (float)EaseOut(std::min(1.0, p / 0.4)), open = p > 0.4f ? (float)EaseOut((p - 0.4f) / 0.6f) : 0;
        float lh = ui.S(2) * (1 + 139 * open), lw = scr.Width * sx;
        BYTE a = (BYTE)(255 * (1 - open));
        RectF line(scr.X + (scr.Width - lw) / 2, scr.Y + scr.Height / 2 - lh / 2, lw, lh);
        for (int k = 3; k >= 1; --k)
            ui.Fill(Grow(line, k * ui.S(6)), Color((BYTE)(a * 0.12f), 255, 255, 255), k * ui.S(6));
        ui.Fill(line, Color(a, 255, 255, 255));
    }
    ui.G().Restore(st);

    HomeShelf(RectF(mx + ui.S(40), my + mh + under, mw - ui.S(80), shelfH), t);

    HomeTipDraw(r);  // последней: поверх монитора и полки
}

// Подсказка предмета карты: всплывает и тает; держится в странице r.
void Studio::HomeTipDraw(const RectF& r) {
    const UiColors& c = ui.C();
    HomeTip& tip = homeTip_;
    tip.a = ui.Anim(kTip + 50, tip.want, 16);
    if (tip.a > 0.01f && !tip.title.empty()) {
        float tw = std::max(ui.Measure(tip.title, 12.5f, true), ui.Measure(tip.sub, 11.5f)) + ui.S(24);
        float th = ui.S(50);
        const RectF& at = tip.at;
        float x = tip.place == 0   ? at.X + at.Width + ui.S(10)
                  : tip.place == 1 ? at.X - tw - ui.S(10)
                                   : at.X + at.Width / 2 - tw / 2;
        float y = tip.place == 2 ? at.Y - th - ui.S(10) : at.Y + at.Height / 2 - th / 2;
        x = std::clamp(x, r.X + ui.S(6), r.X + r.Width - tw - ui.S(6));
        y = std::clamp(y, r.Y + ui.S(6), r.Y + r.Height - th - ui.S(6)) + ui.S(6) * (1 - tip.a);
        RectF br(x, y, tw, th);
        BYTE a = (BYTE)(255 * tip.a);
        ui.Fill(Grow(br, ui.S(3)), Color((BYTE)(40 * tip.a), 0, 0, 0), ui.S(12));
        ui.Fill(br, WithAlpha(c.cardHi, a), ui.S(9));
        ui.Stroke(br, WithAlpha(c.border, a), ui.S(9));
        ui.Text(tip.title, RectF(x + ui.S(12), y + ui.S(7), tw, ui.S(20)), WithAlpha(c.text, a), 12.5f, true);
        ui.Text(tip.sub, RectF(x + ui.S(12), y + ui.S(27), tw, ui.S(18)), WithAlpha(tip.warn ? c.warn : c.muted, a),
                11.5f);
    }
}

}  // namespace studio

void SetEditorNetStatus(std::function<std::wstring()> f) { studio::g_netStatus = std::move(f); }
