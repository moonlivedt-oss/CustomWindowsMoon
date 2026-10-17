// Раздел «Темы» - «тема в деле»: слева витрина выбранной (или примеряемой) темы, справа - как она ляжет
// (кусочек студии, рамка и заголовок окна, сообщение), внизу лента тем доком. Набросок -
// art/overview2_sketch.html, Т1. Примерка - картинка растворяется с наездом, цвета перетекают; выбор - волной.
#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "core/files.h"
#include "core/fx.h"
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

const wchar_t* ThemeTagline(const std::wstring& key) { return Tagline(key); }

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

// Картинка темы со сменой: прежняя под ней, новая растворяется поверх с
// медленным наездом. zoom - с какого увеличения наезжает новая, scale - общий
// масштаб (включение экрана в «Обзоре»).
bool Studio::ThemeFade(PicFade &f, const ThemeInfo &t, const RectF &r,
                       float radius, float zoom, float scale) {
  if (f.key != t.key) {
    if (!f.key.empty()) {
      f.prev = f.cur;
      f.at = clock_;
    }
    f.key = t.key;
  }
  f.cur = t;
  double age = clock_ - f.at;
  bool game = fx::Game();
  float fade = game ? 1 : (float)EaseOut(age / 0.45),
        move = game ? 1 : (float)EaseOut(age / 1.4);
  GraphicsState st = ui.G().Save();
  ui.G().SetClip(ui.Round(r, radius), CombineModeIntersect);
  auto draw = [&](const ThemeInfo &th, float s, float a) {
    GraphicsState s2 = ui.G().Save();
    float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    ui.G().TranslateTransform(cx, cy);
    ui.G().ScaleTransform(s, s);
    ui.G().TranslateTransform(-cx, -cy);
    ThemePicture(th, r, 0, a);
    ui.G().Restore(s2);
  };
  bool fading = fade < 1 && f.prev.key;
  if (fading)
    draw(f.prev, scale, 1);
  draw(t, scale * (1 + (zoom - 1) * (1 - move)), fading ? fade : 1);
  ui.G().Restore(st);
  bool moving = move < 1 && f.prev.key;
  if (moving)
    ui.Invalidate();
  return moving;
}

// Подписи витрины: метка, имя, настроение и палитра; alpha - для растворения
// при смене темы. Своя палитра (не примерка): квадраты цветов - кнопки выбора
// цвета.
void Studio::HeroText(const RectF &hero, const ThemeInfo &t, bool trying,
                      float alpha) {
  if (alpha <= 0.01f)
    return;
  auto A = [alpha](Color c) { return WithAlpha(c, (BYTE)(c.GetA() * alpha)); };
  float px = hero.X + ui.S(24), py = hero.Y + ui.S(20);
  const wchar_t *label = trying ? L"Примерка" : L"Сейчас";
  float lw = ui.Measure(label, 10.5f, true) + ui.S(22);
  ui.Chip(RectF(px, py, lw, ui.S(24)), label,
          A(ToColor(t.ac, trying ? 90 : 235)),
          A(trying ? kOnImage : MixColor(ToColor(t.bg), Color(0, 0, 0), 0.3)));
  ui.Text(t.name, RectF(px, py + ui.S(32), hero.Width * 0.7f, ui.S(42)),
          A(ToColor(t.fg)), 25, true);
  ui.Text(Tagline(t.key), RectF(px, py + ui.S(72), hero.Width * 0.7f, ui.S(22)),
          A(ToColor(t.muted)), 12.5f);

  // палитра: квадрат, под ним роль и код цвета
  bool own = !trying && !wcscmp(t.key, L"custom");
  struct {
    const wchar_t *role;
    COLORREF c;
    COLORREF *edit;
  } sw[] = {{L"Акцент", t.ac, &cfg.customAccent},
            {L"Второй", t.glow, &cfg.customAccent2},
            {L"Кромка", t.gold, &cfg.customGold},
            {L"Фон", t.bg, &cfg.customBg}};
  float d = ui.S(44), slot = ui.S(58);
  float sy = hero.Y + hero.Height - ui.S(20) - ui.S(34) - d;
  for (int i = 0; i < 4; ++i) {
    RectF dr(px + i * slot, sy, d, d);
    if (own && alpha > 0.99f) {
      int id = ID_CUSTOM + i;
      if (ui.Click(id, dr)) {
        COLORREF *target = sw[i].edit;
        Defer([this, target] {
          COLORREF v = *target;
          if (!PickColor(v))
            return;
          Change();
          *target = v;
          Save();
        });
      }
      ui.Tip(id, L"Кликни, чтобы выбрать цвет");
      float hov = ui.Anim(id, ui.Hot(id), 16);
      dr.Y -= ui.S(4) * hov;
    }
    ui.Fill(dr, A(ToColor(sw[i].c)), ui.S(12));
    ui.Stroke(dr, A(Color(70, 255, 255, 255)), ui.S(12), ui.S(2));
    ui.Text(sw[i].role,
            RectF(px + i * slot - ui.S(6), sy + d + ui.S(3), d + ui.S(12),
                  ui.S(15)),
            A(ToColor(t.muted)), 9.5f, false, ui::Center);
    ui.Text(Hex(sw[i].c),
            RectF(px + i * slot - ui.S(8), sy + d + ui.S(17), d + ui.S(16),
                  ui.S(15)),
            A(ToColor(t.muted)), 9.5f, false, ui::Center);
  }
  if (own)
    ui.Text(L"щёлкни по цвету, чтобы поменять",
            RectF(px + 4 * slot + ui.S(6), sy + ui.S(12), ui.S(220), ui.S(20)),
            A(ToColor(t.muted)), 11);
}

// Витрина: выбранная тема или та, на которую сейчас наведено (примерка).
// Картинка растворяется в новую с медленным наездом, подписи - тоже: прежние
// гаснут, новые проступают и чуть поднимаются.
void Studio::ThemeHero(const RectF &hero, const ThemeInfo &t, bool trying) {
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
  float lin = fx::Game() ? 1 : (float)Clamp01((now - heroAt_) / 380.0),
        k = (float)EaseOut(lin);
  bool fading = k < 1 && heroPrev_.key;
  if (fading)
    ui.Invalidate();

  float rad = ui.S(16);
  Reveal(hero);
  ThemeFade(heroPic_, t, hero, rad, 1.07f);
  Color bg = fading ? MixColor(ToColor(heroPrev_.bg), ToColor(t.bg), k)
                    : ToColor(t.bg);
  Scrim(hero, rad, RectF(hero.X, hero.Y, hero.Width * 0.75f, hero.Height),
        WithAlpha(bg, 235), WithAlpha(bg, 0), true);
  Scrim(hero, rad,
        RectF(hero.X, hero.Y + hero.Height * 0.58f, hero.Width,
              hero.Height * 0.42f),
        WithAlpha(bg, 0), WithAlpha(bg, 215), false);
  ui.Stroke(hero, Color(40, 255, 255, 255), rad);
  if (!fading) {
    HeroText(hero, t, trying, 1);
    return;
  }
  HeroText(hero, heroPrev_, heroPrevTrying_, std::max(0.0f, 1 - 2 * lin));
  float in = (float)EaseOut(std::max(0.0f, 2 * lin - 1));
  RectF moved(hero.X, hero.Y + ui.S(8) * (1 - in), hero.Width, hero.Height);
  HeroText(moved, t, trying, in);
}

// Как тема ляжет: кусочек студии, рамка и заголовок активного окна,
// карточка-сообщение. Цвета - студии, а она при примерке уже в цветах
// наведённой темы и перетекает в них.
void Studio::ThemeInAction(const RectF &r, const ThemeInfo &t) {
  const UiColors &c = ui.C();
  std::wstring dir = ExeDir() + L"\\icons\\";
  float capH = ui.S(20), msgH = ui.S(58),
        fragH = std::min(ui.S(150), r.Height * 0.36f);
  auto cap = [&](const wchar_t *s, float x, float y) {
    ui.Text(s, RectF(x, y, r.Width, capH), c.faint, 11);
  };
  auto pbtn = [&](const wchar_t *s, float right, float cy) {
    float w = ui.Measure(s, 11.5f, true) + ui.S(28);
    RectF b(right - w, cy - ui.S(15), w, ui.S(30));
    ui.Fill(b, c.accent, ui.S(8));
    ui.Text(s, b, c.onAccent, 11.5f, true, ui::Center);
    return b;
  };
  auto sbtn = [&](const wchar_t *s, float x, float cy) {
    float w = ui.Measure(s, 11.5f) + ui.S(24);
    RectF b(x, cy - ui.S(14), w, ui.S(28));
    ui.Fill(b, c.cardHi, ui.S(8));
    ui.Stroke(b, c.border, ui.S(8));
    ui.Text(s, b, c.text, 11.5f, false, ui::Center);
    return w;
  };

  // студия: полоска разделов и строка сектора
  cap(L"студия", r.X + ui.S(14), r.Y);
  RectF f1(r.X, r.Y + capH, r.Width, fragH);
  Reveal(f1);
  ui.Fill(f1, c.card, ui.S(14));
  {
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(ui.Round(f1, ui.S(14)), CombineModeIntersect);
    ui.Fill(RectF(f1.X, f1.Y, ui.S(44), f1.Height), c.rail);
    ui.G().Restore(st);
  }
  ui.Stroke(f1, c.border, ui.S(14));
  const wchar_t *rail[] = {L"rail-home.png", L"rail-rings.png",
                           L"rail-desk.png"};
  for (int i = 0; i < 3; ++i)
    ui.Icon(dir + rail[i],
            RectF(f1.X + ui.S(10), f1.Y + ui.S(12) + i * ui.S(34), ui.S(24),
                  ui.S(24)));
  ui.Fill(RectF(f1.X, f1.Y + ui.S(47), ui.S(3), ui.S(22)), c.accent,
          ui.S(1.5f));
  float x = f1.X + ui.S(60), right = f1.X + f1.Width - ui.S(14);
  float row = (f1.Height - ui.S(24)) / 3, y = f1.Y + ui.S(12) + row / 2;
  ui.Icon(dir + L"projects.png", RectF(x, y - ui.S(16), ui.S(32), ui.S(32)));
  ui.Text(L"Проекты", RectF(x + ui.S(42), y - ui.S(17), ui.S(200), ui.S(20)),
          c.text, 13, true);
  ui.Text(L"левый край, сектор 1 из 4",
          RectF(x + ui.S(42), y + ui.S(1), ui.S(220), ui.S(18)), c.muted,
          10.5f);
  pbtn(L"Открыть", right, y);
  y += row;
  RectF sw(x, y - ui.S(11), ui.S(40), ui.S(22));
  ui.Fill(sw, c.accent, ui.S(11));
  ui.Fill(RectF(sw.X + ui.S(21), sw.Y + ui.S(4), ui.S(14), ui.S(14)),
          c.onAccent, ui.S(7));
  ui.Text(L"Второе кольцо из папки",
          RectF(x + ui.S(50), y - ui.S(10), ui.S(220), ui.S(20)), c.text,
          11.5f);
  float cw = ui.Measure(L"папка", 10.5f, true) + ui.S(16);
  ui.Chip(RectF(right - cw, y - ui.S(10), cw, ui.S(20)), L"папка",
          MixColor(c.card, c.accent, 0.22f), c.accent);
  y += row;
  float bx = x;
  bx += sbtn(L"Папка...", bx, y) + ui.S(8);
  sbtn(L"Файл...", bx, y);
  ui.Text(L"18 во втором кольце",
          RectF(right - ui.S(200), y - ui.S(10), ui.S(200), ui.S(20)), c.gold,
          10.5f, false, ui::Right);

  // окно: рамка и заголовок в цветах темы, внутри - чужая программа
  float msgY = r.Y + r.Height - msgH;
  float wy = f1.Y + f1.Height + ui.S(18) + capH,
        wh = msgY - capH - ui.S(14) - wy;
  if (wh > ui.S(60)) {
    cap(L"рамка и заголовок активного окна", r.X + ui.S(14), wy - capH);
    RectF wr(r.X + ui.S(14), wy, r.Width - ui.S(28), wh);
    Reveal(wr);
    float rad = ui.S(10);
    for (int k = 4; k >= 1; --k)
      ui.Fill(Grow(wr, k * ui.S(4.5f)), WithAlpha(c.accent, (BYTE)(24 - k * 4)),
              rad + k * ui.S(4.5f));
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(ui.Round(wr, rad), CombineModeIntersect);
    float tb = ui.S(28);
    ui.Fill(wr, Color(255, 243, 244, 246));
    ui.Fill(RectF(wr.X, wr.Y, wr.Width, tb), c.cardHi);
    float lw[] = {0.8f, 0.62f, 0.7f, 0.45f};
    for (int i = 0; i < 4; ++i) {
      float ly = wr.Y + tb + ui.S(12) + i * ui.S(14);
      if (ly + ui.S(7) > wr.Y + wr.Height - ui.S(6))
        break;
      ui.Fill(
          RectF(wr.X + ui.S(12), ly, (wr.Width - ui.S(24)) * lw[i], ui.S(7)),
          Color(255, 215, 218, 224), ui.S(3.5f));
    }
    ui.G().Restore(st);
    ui.Icon(dir + L"editor.png",
            RectF(wr.X + ui.S(10), wr.Y + ui.S(6), ui.S(16), ui.S(16)));
    ui.Text(L"Документ - Блокнот", RectF(wr.X + ui.S(34), wr.Y, wr.Width, tb),
            c.text, 11);
    ui.Text(L"-     \x25A1     \x00D7",
            RectF(wr.X, wr.Y, wr.Width - ui.S(12), tb), c.muted, 11, false,
            ui::Right);
    ui.Stroke(wr, c.accent, rad, ui.S(2));
  }

  // сообщение: карточка в рамке акцента
  cap(L"сообщение", r.X + ui.S(14), msgY - capH);
  RectF m(r.X, msgY, r.Width, msgH);
  Reveal(m);
  ui.Fill(m, c.card, ui.S(14));
  ui.Stroke(m, c.accent, ui.S(14), ui.S(1.4f));
  float my = m.Y + m.Height / 2;
  ui.Icon(dir + L"tools-clean.png",
          RectF(m.X + ui.S(14), my - ui.S(14), ui.S(28), ui.S(28)));
  ui.Text(L"Очистка готова",
          RectF(m.X + ui.S(52), my - ui.S(17), ui.S(220), ui.S(20)), c.text, 12,
          true);
  ui.Text(L"освобождено 2,4 ГБ",
          RectF(m.X + ui.S(52), my + ui.S(1), ui.S(220), ui.S(18)), c.muted,
          10.5f);
  pbtn(L"Открыть", m.X + m.Width - ui.S(14), my);
  (void)t;
}

// Лента тем доком: под курсором карточки крупнее, ближние - чуть меньше.
// Пейзажи, черта, «Из обоев» и «Своя палитра». Наведение - примерка, щелчок -
// новая тема волной от карточки.
void Studio::ThemeFilm(const RectF &r) {
  const UiColors &c = ui.C();
  const auto &themes = Themes();
  std::vector<int> order;
  for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < (int)themes.size(); ++i)
      if (OwnTheme(themes[i].key) == (pass == 1))
        order.push_back(i);
  int n = (int)order.size();
  float gap = ui.S(10), sep = ui.S(21);
  float cw = std::min(ui.S(118), (r.Width - gap * (n - 1) - sep) / n),
        ch = cw * 0.66f;
  float total = n * cw + (n - 1) * gap + sep, x0 = r.X + (r.Width - total) / 2,
        base = r.Y + r.Height;
  POINT ms = ui.Mouse();
  bool inside = ms.x >= r.X && ms.x <= r.X + r.Width &&
                ms.y >= base - ch * 1.5f - ui.S(10) && ms.y <= base + ui.S(4);
  ui.Hover(ID_THEME + 90, RectF(r.X, base - ch * 1.5f - ui.S(10), r.Width,
                                ch * 1.5f + ui.S(14)));
  ui.Track(ID_THEME + 90);
  auto shown = [&](const ThemeInfo &t) {
    Config v = cfg;
    v.theme = t.key;
    return CurrentTheme(v);
  };
  struct Card {
    int i;
    RectF r;
    float s;
  };
  std::vector<Card> cards;
  float x = x0;
  bool moving = false;
  for (int k = 0; k < n; ++k) {
    if (k > 0 && OwnTheme(themes[order[k]].key) &&
        !OwnTheme(themes[order[k - 1]].key)) {
      ui.Fill(RectF(x + (sep - gap) / 2 - 0.5f, base - ch - ui.S(4), 1,
                    ch - ui.S(10)),
              c.border);
      x += sep;
    }
    RectF cr(x, base - ch, cw, ch);
    x += cw + gap;
    int i = order[k], id = ID_THEME + i;
    const ThemeInfo &t = themes[i];
    bool on = cfg.theme == t.key;
    Reveal(cr);
    float want = 0;
    if (inside) {
      float d = std::fabs(ms.x - (cr.X + cr.Width / 2));
      want = std::max(0.0f, 1 - d / ui.S(170));
    }
    float &s = filmDock_[std::min(k, 15)];
    if (fx::Game() || std::fabs(want - s) < 0.004f)
      s = want;
    else {
      s += (want - s) * std::min(1.0f, ldt_ * 16);
      moving = true;
    }
    float sc = 1 + 0.38f * s;
    RectF hit(cr.X + cw / 2 - cw * sc / 2, base - ch * sc, cw * sc, ch * sc);
    if (ui.Click(id, hit) && !on)
      ThemeWave(t.key, PointF(hit.X + hit.Width / 2, hit.Y + hit.Height / 2));
    ui.Track(id);
    if (ui.Hot(id) && !on)
      hoverThemeNext_ = t.key;
    cards.push_back({i, cr, sc});
  }
  if (moving)
    ui.Invalidate();
  // крупные поверх мелких
  std::stable_sort(cards.begin(), cards.end(),
                   [](const Card &a, const Card &b) { return a.s < b.s; });
  for (const Card &cd : cards) {
    const ThemeInfo t = shown(themes[cd.i]);
    bool on = cfg.theme == t.key;
    const RectF &cr = cd.r;
    GraphicsState st = ui.G().Save();
    float ox = cr.X + cr.Width / 2, oy = cr.Y + cr.Height;
    ui.G().TranslateTransform(ox, oy);
    ui.G().ScaleTransform(cd.s, cd.s);
    ui.G().TranslateTransform(-ox, -oy);
    float rad = ui.S(11);
    if (cd.s > 1.01f)
      for (int k = 3; k >= 1; --k)
        ui.Fill(RectF(cr.X - k * ui.S(2), cr.Y + ui.S(6),
                      cr.Width + k * ui.S(4), cr.Height),
                Color((BYTE)(24 * (cd.s - 1) / 0.38f), 0, 0, 0),
                rad + k * ui.S(2));
    ThemePicture(t, cr, rad, 1);
    Scrim(cr, rad,
          RectF(cr.X, cr.Y + cr.Height * 0.4f, cr.Width, cr.Height * 0.6f),
          Color(0, 0, 0, 0), Color(170, 0, 0, 0), false);
    // палитра полоской внизу, над ней имя
    RectF pal(cr.X + ui.S(7), cr.Y + cr.Height - ui.S(11), cr.Width - ui.S(14),
              ui.S(5));
    COLORREF pc[] = {t.ac, t.glow, t.gold, t.bg};
    {
      GraphicsState s2 = ui.G().Save();
      ui.G().SetClip(ui.Round(pal, ui.S(2.5f)), CombineModeIntersect);
      for (int k = 0; k < 4; ++k)
        ui.Fill(RectF(pal.X + pal.Width * k / 4, pal.Y, pal.Width / 4 + 1,
                      pal.Height),
                ToColor(pc[k]));
      ui.G().Restore(s2);
    }
    RectF nr(cr.X + ui.S(7), cr.Y + cr.Height - ui.S(31), cr.Width - ui.S(12),
             ui.S(18));
    ui.Text(t.name, RectF(nr.X, nr.Y + ui.S(1), nr.Width, nr.Height),
            Color(150, 0, 0, 0), 10.5f, true);
    ui.Text(t.name, nr, kOnImage, 10.5f, true);
    if (on) {
      ui.Stroke(cr, c.accent, rad, ui.S(2));
      float bd = ui.S(18);
      RectF br(cr.X + cr.Width - bd - ui.S(5), cr.Y + ui.S(5), bd, bd);
      SolidBrush bb(c.accent);
      ui.G().FillEllipse(&bb, br);
      Pen tick(c.onAccent, ui.S(1.8f));
      tick.SetStartCap(LineCapRound);
      tick.SetEndCap(LineCapRound);
      tick.SetLineJoin(LineJoinRound);
      PointF pts[] = {{br.X + bd * 0.27f, br.Y + bd * 0.52f},
                      {br.X + bd * 0.44f, br.Y + bd * 0.68f},
                      {br.X + bd * 0.74f, br.Y + bd * 0.34f}};
      ui.G().DrawLines(&tick, pts, 3);
    } else {
      ui.Stroke(cr, Color((BYTE)(26 + 60 * (cd.s - 1) / 0.38f), 255, 255, 255),
                rad);
    }
    ui.G().Restore(st);
  }
}

void Studio::ThemesPage(const RectF &r) {
  LaneTick();
  RectF in = Inset(r, ui.S(28), ui.S(20));
  Header(in, L"Темы",
         L"Наведи, чтобы примерить, щёлкни, чтобы выбрать. Справа - как тема "
         L"ляжет: студия, окна, сообщения.");
  RectF a(in.X, in.Y + ui.S(66), in.Width, in.Height - ui.S(66));
  const auto &themes = Themes();
  int n = (int)themes.size();
  float cw = std::min(ui.S(118), (a.Width - ui.S(10) * (n - 1) - ui.S(21)) / n);
  float filmH = cw * 0.66f * 1.38f + ui.S(8);
  RectF film(a.X, a.Y + a.Height - filmH, a.Width, filmH);
  float heroW = std::round(a.Width * 0.58f),
        heroH = a.Height - filmH - ui.S(18);
  RectF hero(a.X, a.Y, heroW, heroH);

  Config shownCfg = cfg;
  if (!hoverTheme_.empty())
    shownCfg.theme = hoverTheme_;
  ThemeInfo t = CurrentTheme(shownCfg);
  ThemeHero(hero, t, !hoverTheme_.empty());
  float fx0 = hero.X + hero.Width + ui.S(20);
  ThemeInAction(RectF(fx0, a.Y, a.X + a.Width - fx0, heroH), t);
  ThemeFilm(film);
}

} // namespace studio
