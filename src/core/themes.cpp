#include "core/themes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace Gdiplus;

namespace {

constexpr COLORREF H(unsigned v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

}  // namespace

const std::vector<ThemeInfo>& Themes() {
    // key, имя, пейзаж, фон, панель, текст, приглушённый, акцент, второй акцент, кромка
    static const std::vector<ThemeInfo> list = {
        // под обои с багровым полем: графит грозового неба, багрянец цветов, сталь, серебро
        {L"crimson", L"Багровое поле", L"bg-crimson.jpg", H(0x12141C), H(0x1C1F2A), H(0xEEF0F5), H(0x9AA3B4),
         H(0xC8323F), H(0x8FA3BF), H(0xD9DEE6)},
        {L"sunset", L"Закат", L"bg-sunset.webp", H(0x1F1129), H(0x2E183A), H(0xFFF1E0), H(0xC9A9C0), H(0xFF7A6B),
         H(0xE8608C), H(0xFFD36E)},
        {L"moon", L"Лунная ночь", L"bg-default.webp", H(0x121433), H(0x1E1D4F), H(0xEDE9FF), H(0xA9A3D9), H(0xC3B6FF),
         H(0x9C8CF0), H(0xFFC857)},
        {L"mocha", L"Catppuccin", L"bg-mocha.webp", H(0x181825), H(0x1E1E2E), H(0xCDD6F4), H(0xA6ADC8), H(0xCBA6F7),
         H(0x89B4FA), H(0xF9E2AF)},
        {L"tokyo", L"Tokyo Night", L"bg-tokyo.webp", H(0x1A1B26), H(0x1F2335), H(0xC0CAF5), H(0xA9B1D6), H(0x7AA2F7),
         H(0xBB9AF7), H(0xE0AF68)},
        {L"dracula", L"Dracula", L"bg-dracula.webp", H(0x21222C), H(0x282A36), H(0xF8F8F2), H(0xBFC3D9), H(0xBD93F9),
         H(0xFF79C6), H(0xF1FA8C)},
        {L"rosepine", L"Rosé Pine", L"bg-rosepine.webp", H(0x191724), H(0x1F1D2E), H(0xE0DEF4), H(0xAAA6C8),
         H(0xEBBCBA), H(0xC4A7E7), H(0xF6C177)},
        {L"nord", L"Nord", L"bg-nord.webp", H(0x2E3440), H(0x353C4A), H(0xECEFF4), H(0xD8DEE9), H(0x88C0D0),
         H(0x81A1C1), H(0xEBCB8B)},
        {L"gruvbox", L"Gruvbox", L"bg-gruvbox.webp", H(0x1D2021), H(0x282828), H(0xEBDBB2), H(0xD5C4A1), H(0xFE8019),
         H(0xFB4934), H(0xFABD2F)},
        {L"onedark", L"One Dark", L"bg-onedark.webp", H(0x21252B), H(0x282C34), H(0xD7DAE0), H(0xABB2BF), H(0x61AFEF),
         H(0xC678DD), H(0xE5C07B)},
        {L"forest", L"Лес", L"bg-forest.webp", H(0x141D18), H(0x1A2620), H(0xDDE9E0), H(0xA9C2B1), H(0x7FD4A0),
         H(0xD8C97A), H(0xE6D27F)},
        {L"wallpaper", L"Из обоев", L"", H(0x14161D), H(0x1D2029), H(0xEEF0F5), H(0x9AA0AD), H(0xD7263D), H(0xD7263D),
         H(0xFFFFFF)},
        {L"custom", L"Своя палитра", L"", H(0x1B1420), H(0x271D2E), H(0xF4F0F6), H(0xB4A9BC), H(0xFF7A6B), H(0xE8608C),
         H(0xFFD36E)},
    };
    return list;
}

ThemeInfo CurrentTheme(const Config& c) {
    const auto& all = Themes();
    ThemeInfo t = all[0];
    for (const auto& it : all)
        if (c.theme == it.key) t = it;
    if (c.theme == L"wallpaper") {
        t.ac = t.glow = c.accent;
        Color a = ToColor(c.accent);
        Color light = MixColor(a, Color(255, 255, 255), 0.45);
        t.gold = RGB(light.GetR(), light.GetG(), light.GetB());
    } else if (c.theme == L"custom") {
        t.bg = c.customBg;
        Color bg = ToColor(c.customBg);
        Color panel = MixColor(bg, Color(255, 255, 255), 0.07);
        t.panel = RGB(panel.GetR(), panel.GetG(), panel.GetB());
        t.ac = c.customAccent;
        t.glow = c.customAccent2;
        t.gold = c.customGold;
    }
    if (c.dayTint) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        t = DayTint(t, c, st.wHour + st.wMinute / 60.0);
    }
    return t;
}

// Пресет «Красоты» по времени суток (И37): днём акцент ярче, вечером цвета теплеют, ночью (часы ночного режима)
// тёплые и приглушённые. Кромка колец немного берёт цвет обоев, когда акцент не из них. Переходы - по минутам:
// незаметно, видно только «к вечеру стало теплее».
ThemeInfo DayTint(ThemeInfo t, const Config& c, double hour) {
    auto ramp = [](double x, double a, double b) { return std::clamp((x - a) / (b - a), 0.0, 1.0); };
    double h = std::fmod(hour + 24, 24);
    double nightHours = std::fmod(c.nightTo - c.nightFrom + 24, 24);
    bool night = c.night && nightHours > 0 && std::fmod(h - c.nightFrom + 24, 24) < nightHours;
    // тепло: 0 днём; за 5 часов до ночи разгорается до 0,6; ночью 1; после ночи остывает за 3 часа
    double untilNight = std::fmod(c.nightFrom - h + 24, 24), afterNight = std::fmod(h - c.nightTo + 24, 24);
    double evening = 1 - ramp(untilNight, 0, 5), morning = 1 - ramp(afterNight, 0, 3);
    double warm = night ? 1.0 : 0.6 * std::max(evening, morning);
    double bright = night ? 0.0 : 1 - std::max(evening, morning);
    auto mix = [](COLORREF a, Color b, double k) {
        Color m = MixColor(ToColor(a), b, k);
        return RGB(m.GetR(), m.GetG(), m.GetB());
    };
    const Color amber(255, 172, 96), white(255, 255, 255), black(0, 0, 0);
    t.ac = mix(mix(t.ac, amber, 0.22 * warm), white, 0.08 * bright);
    t.glow = mix(mix(t.glow, amber, 0.30 * warm), white, 0.10 * bright);
    t.gold = mix(t.gold, amber, 0.35 * warm);
    if (night) t.bg = mix(t.bg, black, 0.18);
    if (c.theme != L"wallpaper" && c.accentAuto) t.gold = mix(t.gold, ToColor(c.accent), 0.25);
    return t;
}

RingColors RingPalette(const Config& c) {
    ThemeInfo t = CurrentTheme(c);
    RingColors r;
    r.base = ToColor(t.bg);
    r.baseSub = ToColor(t.panel);
    r.hotIn = ToColor(t.glow);
    r.hotOut = ToColor(t.ac);
    r.active = ToColor(t.glow);
    r.rim = ToColor(t.gold);
    r.border = c.theme == L"sunset" ? Color(255, 179, 138) : ToColor(t.muted);
    r.text = ToColor(t.fg);
    r.pillBg = ToColor(t.bg);
    r.pillEdge = ToColor(t.gold);
    r.hubRing = ToColor(t.gold);
    r.dot = ToColor(t.ac);
    r.seam = ToColor(t.ac);
    r.shadow = MixColor(ToColor(t.bg), Color(0, 0, 0), 0.5);
    return r;
}

UiColors UiPalette(const Config& c) {
    ThemeInfo t = CurrentTheme(c);
    Color bg = ToColor(t.bg), fg = ToColor(t.fg), black(0, 0, 0);
    UiColors u;
    u.win = MixColor(bg, black, 0.32);
    u.rail = MixColor(bg, black, 0.45);
    u.card = MixColor(bg, black, 0.12);
    u.cardHi = MixColor(ToColor(t.panel), fg, 0.04);
    u.field = MixColor(bg, black, 0.22);
    u.border = MixColor(u.win, fg, 0.13);
    u.text = fg;
    u.muted = ToColor(t.muted);
    u.faint = MixColor(ToColor(t.muted), u.win, 0.45);
    u.accent = ToColor(t.ac);
    u.onAccent = MixColor(bg, black, 0.3);
    u.accent2 = ToColor(t.glow);
    u.gold = ToColor(t.gold);
    u.warn = Color(232, 168, 92);
    u.warnBg = MixColor(u.win, Color(232, 168, 92), 0.12);
    return u;
}

// Плавный переход между палитрами: UiColors - это ряд Color подряд.
UiColors MixUi(const UiColors& a, const UiColors& b, float t) {
    UiColors r = a;
    const Color* pa = reinterpret_cast<const Color*>(&a);
    const Color* pb = reinterpret_cast<const Color*>(&b);
    Color* pr = reinterpret_cast<Color*>(&r);
    for (size_t i = 0; i < sizeof(UiColors) / sizeof(Color); ++i)
        pr[i] = MixColor(pa[i], pb[i], t, (BYTE)(pa[i].GetA() + (pb[i].GetA() - pa[i].GetA()) * t));
    return r;
}

bool SameUi(const UiColors& a, const UiColors& b) {
    const Color* pa = reinterpret_cast<const Color*>(&a);
    const Color* pb = reinterpret_cast<const Color*>(&b);
    for (size_t i = 0; i < sizeof(UiColors) / sizeof(Color); ++i) {
        if (std::abs((int)pa[i].GetR() - (int)pb[i].GetR()) > 1 ||
            std::abs((int)pa[i].GetG() - (int)pb[i].GetG()) > 1 || std::abs((int)pa[i].GetB() - (int)pb[i].GetB()) > 1)
            return false;
    }
    return true;
}
