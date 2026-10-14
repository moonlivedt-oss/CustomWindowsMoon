// Раздел «Темы»: большая витрина выбранной (или примеряемой) темы и галерея карточек-пейзажей.
#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "core/files.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;

namespace studio {

namespace {

// Настроение темы одной строкой - подпись в витрине.
const wchar_t* Tagline(const std::wstring& key) {
    static const struct {
        const wchar_t* key;
        const wchar_t* text;
    } kLines[] = {
        {L"crimson", L"Грозовое небо, багряные цветы и сталь"},
        {L"sunset", L"Тёплый вечер над спокойной водой"},
        {L"moon", L"Сиреневые горы под луной"},
        {L"mocha", L"Пастель на тёплом графите"},
        {L"tokyo", L"Ночной город в неоновой синеве"},
        {L"dracula", L"Фиолетовый сумрак и розовая луна"},
        {L"rosepine", L"Пудровый рассвет над озером"},
        {L"nord", L"Ледяной фьорд и северный свет"},
        {L"gruvbox", L"Осенние холмы в ретро-тонах"},
        {L"onedark", L"Тихая гавань в синих сумерках"},
        {L"forest", L"Хвойный туман и мох"},
        {L"wallpaper", L"Акцент сам берётся из твоих обоев"},
        {L"custom", L"Четыре цвета, которые выберешь сам"},
    };
    for (const auto& l : kLines)
        if (key == l.key) return l.text;
    return L"";
}

bool OwnTheme(const wchar_t* key) { return !wcscmp(key, L"wallpaper") || !wcscmp(key, L"custom"); }

std::wstring Hex(COLORREF c) {
    wchar_t b[8];
    swprintf(b, 8, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

RectF Grow(const RectF& r, float d) { return RectF(r.X - d, r.Y - d, r.Width + 2 * d, r.Height + 2 * d); }

const Color kOnImage(255, 250, 248, 252);

}  // namespace

// Картинка темы: пейзаж из папки themes, обои (перечитываются, когда файл обоев меняется) или ничего.
Bitmap* Studio::ThemeArt(const ThemeInfo& t, int maxSide) {
    if (!wcscmp(t.key, L"wallpaper")) {
        wchar_t wp[MAX_PATH] = {};
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
        if (!wp[0]) return nullptr;
        WIN32_FILE_ATTRIBUTE_DATA d{};
        GetFileAttributesExW(wp, GetFileExInfoStandard, &d);
        // время записи в ключе: Wallpaper Engine перезаписывает тот же файл
        wchar_t stamp[32];
        swprintf(stamp, 32, L"?%08lX%08lX", d.ftLastWriteTime.dwHighDateTime, d.ftLastWriteTime.dwLowDateTime);
        return Img(wp, maxSide, stamp);
    }
    if (!t.image[0]) return nullptr;
    return Img(ExeDir() + L"\\themes\\" + t.image, maxSide);
}

// Пейзаж (или градиент палитры, если картинки нет) на всю рамку со скруглением; alpha 0..1 - для растворения.
void Studio::ThemePicture(const ThemeInfo& t, const RectF& r, float radius, float alpha) {
    if (Bitmap* b = ThemeArt(t, 960)) {
        ui.Picture(b, r, radius, true, (BYTE)(255 * alpha));
        return;
    }
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(ui.Round(r, radius), CombineModeIntersect);
    BYTE a = (BYTE)(255 * alpha);
    LinearGradientBrush lg(Grow(r, 1), ToColor(t.bg, a), ToColor(t.ac, a), LinearGradientModeForwardDiagonal);
    Color cols[] = {ToColor(t.bg, a), MixColor(ToColor(t.bg), ToColor(t.glow), 0.55, a), ToColor(t.ac, a)};
    REAL pos[] = {0, 0.55f, 1};
    lg.SetInterpolationColors(cols, pos, 3);
    ui.G().FillRectangle(&lg, Grow(r, 1));
    // мягкое пятно «кромки» в углу, чтобы градиент не выглядел плоским
    GraphicsPath glow;
    RectF spot(r.X + r.Width * 0.55f, r.Y - r.Height * 0.35f, r.Width * 0.7f, r.Height * 0.9f);
    glow.AddEllipse(spot);
    PathGradientBrush pg(&glow);
    pg.SetCenterColor(ToColor(t.gold, (BYTE)(120 * alpha)));
    Color edge = ToColor(t.gold, 0);
    INT n = 1;
    pg.SetSurroundColors(&edge, &n);
    ui.G().FillPath(&pg, &glow);
    ui.G().Restore(st);
}

// Затемнение поверх картинки: снизу вверх (под подписью) или слева направо (под текстом витрины).
void Studio::Scrim(const RectF& clip, float radius, const RectF& band, Color from, Color to, bool horizontal) {
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(ui.Round(clip, radius), CombineModeIntersect);
    LinearGradientBrush lg(Grow(band, 1), from, to,
                           horizontal ? LinearGradientModeHorizontal : LinearGradientModeVertical);
    ui.G().FillRectangle(&lg, band);
    ui.G().Restore(st);
}

// Подписи витрины: метка, имя, настроение и палитра; alpha - для растворения при смене темы.
void Studio::HeroText(const RectF& hero, const ThemeInfo& t, bool trying, float alpha) {
    if (alpha <= 0.01f) return;
    auto A = [alpha](Color c) { return WithAlpha(c, (BYTE)(c.GetA() * alpha)); };
    float px = hero.X + ui.S(26), py = hero.Y + ui.S(22);
    const wchar_t* label = trying ? L"Примерка" : L"Сейчас";
    float lw = ui.Measure(label, 10.5f, true) + ui.S(22);
    ui.Chip(RectF(px, py, lw, ui.S(24)), label, A(ToColor(t.ac, trying ? 70 : 235)),
            A(trying ? kOnImage : MixColor(ToColor(t.bg), Color(0, 0, 0), 0.3)));
    ui.Text(t.name, RectF(px, py + ui.S(32), hero.Width * 0.6f, ui.S(42)), A(ToColor(t.fg)), 25, true);
    ui.Text(Tagline(t.key), RectF(px, py + ui.S(72), hero.Width * 0.6f, ui.S(22)), A(ToColor(t.muted)), 12.5f);

    // палитра: кружок, роль и код цвета; в узкой витрине - только кружки
    struct {
        const wchar_t* role;
        COLORREF c;
    } sw[] = {{L"Акцент", t.ac}, {L"Второй", t.glow}, {L"Кромка", t.gold}, {L"Фон", t.bg}};
    float slot = ui.S(104), d = ui.S(26);
    bool text = slot * 4 < hero.Width * 0.7f;
    float sy = hero.Y + hero.Height - ui.S(24) - d;
    for (int i = 0; i < 4; ++i) {
        float sx = px + i * (text ? slot : d + ui.S(10));
        RectF dr(sx, sy, d, d);
        SolidBrush b(A(ToColor(sw[i].c)));
        Pen ring(A(Color(90, 255, 255, 255)), ui.S(1.5f));
        ui.G().FillEllipse(&b, dr);
        ui.G().DrawEllipse(&ring, dr);
        if (!text) continue;
        ui.Text(sw[i].role, RectF(sx + d + ui.S(8), sy - ui.S(3), slot - d, ui.S(16)), A(ToColor(t.muted)), 10);
        ui.Text(Hex(sw[i].c), RectF(sx + d + ui.S(8), sy + ui.S(12), slot - d, ui.S(17)), A(ToColor(t.fg)), 11, true);
    }
}

// Витрина: выбранная тема или та, на которую сейчас наведено (примерка).
// При смене прежняя картинка растворяется в новую, подписи - тоже.
void Studio::ThemeHero(const RectF& hero, const ThemeInfo& t, bool trying) {
    ULONGLONG now = GetTickCount64();
    if (heroKey_ != t.key) {
        if (!heroKey_.empty()) {
            heroPrev_ = heroCur_;
            heroPrevTrying_ = heroTrying_;
            heroAt_ = now;
        }
        heroKey_ = t.key;
    }
    heroCur_ = t;
    heroTrying_ = trying;
    float lin = (float)Clamp01((now - heroAt_) / 380.0), k = (float)EaseOut(lin);
    bool fading = k < 1 && heroPrev_.key;
    if (fading) ui.Invalidate();

    float rad = ui.S(18);
    if (fading) ThemePicture(heroPrev_, hero, rad, 1);
    ThemePicture(t, hero, rad, fading ? k : 1);
    Color bg = fading ? MixColor(ToColor(heroPrev_.bg), ToColor(t.bg), k) : ToColor(t.bg);
    Scrim(hero, rad, RectF(hero.X, hero.Y, hero.Width * 0.72f, hero.Height), WithAlpha(bg, 235), WithAlpha(bg, 0),
          true);
    Scrim(hero, rad, RectF(hero.X, hero.Y + hero.Height * 0.55f, hero.Width, hero.Height * 0.45f), Color(0, 0, 0, 0),
          Color(110, 0, 0, 0), false);
    ui.Stroke(hero, Color(40, 255, 255, 255), rad);
    // подписи не накладываются: прежние гаснут в первой половине, новые проступают во второй и чуть поднимаются
    if (!fading) {
        HeroText(hero, t, trying, 1);
        return;
    }
    HeroText(hero, heroPrev_, heroPrevTrying_, std::max(0.0f, 1 - 2 * lin));
    float in = (float)EaseOut(std::max(0.0f, 2 * lin - 1));
    RectF moved(hero.X, hero.Y + ui.S(8) * (1 - in), hero.Width, hero.Height);
    HeroText(moved, t, trying, in);
}

// Карточка темы: пейзаж на всю карточку, имя и палитра поверх затемнения снизу.
// Наведение приподнимает карточку и подсвечивает её цветом темы; выбранная - в рамке с галочкой.
void Studio::ThemeCard(int index, const ThemeInfo& t, const RectF& cr, const RectF& view) {
    const UiColors& c = ui.C();
    int id = ID_THEME + index;
    bool on = cfg.theme == t.key;
    RectF vis = Intersect(cr, view);
    if (vis.Height > 1 && ui.Click(id, vis) && !on) {
        Change();
        cfg.theme = t.key;
        ApplyAccent();
        Log(std::wstring(L"Тема: ") + t.name);
        Save();
        on = true;
    }
    bool hot = ui.Hot(id);
    if (hot && !on) hoverThemeNext_ = t.key;
    float a = ui.Anim(id, hot);
    float rad = ui.S(14);
    RectF r(cr.X, cr.Y - ui.S(3) * a, cr.Width, cr.Height);

    // свечение под карточкой цветом её акцента
    if (a > 0.01f || on) {
        float glow = on ? std::max(a, 0.55f) : a;
        for (int k = 4; k >= 1; --k)
            ui.Fill(Grow(RectF(r.X, r.Y + ui.S(5), r.Width, r.Height), k * ui.S(2.5f)),
                    ToColor(t.ac, (BYTE)(glow * 16)), rad + k * ui.S(2.5f));
    }
    ThemePicture(t, r, rad, 1);
    Scrim(r, rad, RectF(r.X, r.Y + r.Height * 0.42f, r.Width, r.Height * 0.58f), Color(0, 0, 0, 0),
          Color((BYTE)(175 + 30 * a), 0, 0, 0), false);

    // палитра справа внизу: кружки внахлёст
    float d = ui.S(13), step = ui.S(9.5f);
    COLORREF dots[] = {t.ac, t.glow, t.gold, t.bg};
    float dx = r.X + r.Width - ui.S(14) - d - step * 3, dy = r.Y + r.Height - ui.S(14) - d;
    for (int k = 0; k < 4; ++k) {
        RectF dr(dx + k * step, dy, d, d);
        SolidBrush b(ToColor(dots[k]));
        Pen p(Color(160, 0, 0, 0), ui.S(1.5f));
        ui.G().FillEllipse(&b, dr);
        ui.G().DrawEllipse(&p, dr);
    }
    RectF nr(r.X + ui.S(14), r.Y + r.Height - ui.S(40), dx - r.X - ui.S(20), ui.S(32));
    ui.Text(t.name, RectF(nr.X, nr.Y + ui.S(1), nr.Width, nr.Height), Color(120, 0, 0, 0), 13.5f, true);
    ui.Text(t.name, nr, kOnImage, 13.5f, true);

    if (on) {
        ui.Stroke(r, c.accent, rad, ui.S(2.5f));
        // значок «выбрана»: кружок акцента с галочкой
        float bd = ui.S(24);
        RectF br(r.X + r.Width - bd - ui.S(10), r.Y + ui.S(10), bd, bd);
        SolidBrush bb(c.accent);
        ui.G().FillEllipse(&bb, br);
        Pen tick(c.onAccent, ui.S(2.2f));
        tick.SetStartCap(LineCapRound);
        tick.SetEndCap(LineCapRound);
        tick.SetLineJoin(LineJoinRound);
        PointF pts[] = {{br.X + bd * 0.28f, br.Y + bd * 0.52f},
                        {br.X + bd * 0.44f, br.Y + bd * 0.67f},
                        {br.X + bd * 0.73f, br.Y + bd * 0.36f}};
        ui.G().DrawLines(&tick, pts, 3);
    } else {
        ui.Stroke(r, Color((BYTE)(26 + 70 * a), 255, 255, 255), rad);
    }
}

void Studio::ThemesPage(const RectF& r) {
    const UiColors& c = ui.C();
    RectF in = Inset(r, ui.S(28), ui.S(20));
    Header(in, L"Темы", L"Наведи, чтобы примерить, кликни, чтобы выбрать. Тема красит кольца, стол и эту студию.");
    RectF view(in.X - ui.S(4), in.Y + ui.S(66), in.Width + ui.S(8), in.Height - ui.S(66));
    float off = ui.Scroll(ID_PAGE_SCROLL, view, pageContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    // поля под свечение и подъём карточек, чтобы их не срезал край прокрутки
    float x = view.X + ui.S(10), w = view.Width - ui.S(20), y = view.Y + ui.S(6) - off, y0 = y - ui.S(6);

    const auto& themes = Themes();
    Config shownCfg = cfg;
    if (!hoverTheme_.empty()) shownCfg.theme = hoverTheme_;
    float heroH = std::clamp(w * 0.27f, ui.S(184), ui.S(250));
    ThemeHero(RectF(x, y, w, heroH), CurrentTheme(shownCfg), !hoverTheme_.empty());
    y += heroH + ui.S(26);

    float gap = ui.S(16);
    int cols = std::max(2, (int)((w + gap) / (ui.S(228) + gap)));
    float cw = (w - gap * (cols - 1)) / cols, ch = cw * 0.62f;
    // своя палитра и обои показываются с подставленными цветами
    auto shown = [&](const ThemeInfo& t) {
        Config v = cfg;
        v.theme = t.key;
        return CurrentTheme(v);
    };
    auto section = [&](const wchar_t* title, const wchar_t* note, bool own) {
        ui.Text(title, RectF(x, y, w, ui.S(24)), c.text, 14, true);
        float tw = ui.Measure(title, 14, true);
        ui.Text(note, RectF(x + tw + ui.S(12), y + ui.S(1), w - tw, ui.S(24)), c.faint, 11.5f);
        y += ui.S(36);
        int n = 0;
        for (int i = 0; i < (int)themes.size(); ++i) {
            if (OwnTheme(themes[i].key) != own) continue;
            RectF cr(x + (n % cols) * (cw + gap), y + (n / cols) * (ch + gap), cw, ch);
            ThemeCard(i, shown(themes[i]), cr, view);
            ++n;
        }
        y += (n + cols - 1) / cols * (ch + gap) + ui.S(14);
    };
    section(L"Пейзажи", L"палитра подобрана под картинку", false);
    section(L"Свои", L"из обоев или четыре своих цвета", true);

    if (cfg.theme == L"custom") {
        RectF card(x, y, w, ui.S(122));
        Card(card);
        ui.Text(L"Своя палитра", RectF(card.X + ui.S(18), card.Y + ui.S(12), w, ui.S(24)), c.text, 14, true);
        struct {
            COLORREF* v;
            const wchar_t* name;
        } sw[] = {{&cfg.customBg, L"Фон"},
                  {&cfg.customAccent, L"Акцент"},
                  {&cfg.customAccent2, L"Второй"},
                  {&cfg.customGold, L"Кромка"}};
        float sw0 = std::min(ui.S(150), (w - ui.S(36) - ui.S(30)) / 4);
        for (int k = 0; k < 4; ++k) {
            RectF sr(card.X + ui.S(18) + k * (sw0 + ui.S(10)), card.Y + ui.S(46), sw0, ui.S(58));
            int id = ID_CUSTOM + k;
            if (ui.Click(id, sr)) {
                COLORREF* target = sw[k].v;
                Defer([this, target] {
                    COLORREF v = *target;
                    if (!PickColor(v)) return;
                    Change();
                    *target = v;
                    Save();
                });
            }
            ui.Tip(id, L"Кликни, чтобы выбрать цвет");
            ui.Fill(sr, ui.Hot(id) ? c.cardHi : c.field, ui.S(10));
            ui.Stroke(sr, c.border, ui.S(10));
            ui.Fill(RectF(sr.X + ui.S(10), sr.Y + ui.S(13), ui.S(32), ui.S(32)), ToColor(*sw[k].v), ui.S(8));
            ui.Text(sw[k].name, RectF(sr.X + ui.S(52), sr.Y + ui.S(10), sr.Width - ui.S(56), ui.S(20)), c.text, 12.5f);
            ui.Text(Hex(*sw[k].v), RectF(sr.X + ui.S(52), sr.Y + ui.S(29), sr.Width - ui.S(56), ui.S(18)), c.muted, 11);
        }
        y += card.Height + ui.S(10);
    } else if (cfg.theme == L"wallpaper") {
        ui.Text(L"Цвет акцента сам подстраивается, когда меняются обои.", RectF(x, y, w, ui.S(22)), c.muted, 12);
        y += ui.S(30);
    }
    pageContentH_ = y - y0 + ui.S(10);
    ui.G().Restore(st);
}

}  // namespace studio
