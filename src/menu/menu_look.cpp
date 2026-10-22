// Облик пунктов: значки проектов и их оживление при наведении, подписи с тенью, оттенки групп,
// карточка о наведённом пункте.
#include "menu/menu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "core/files.h"
#include "core/perf.h"
#include "core/project.h"
#include "core/macro.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "menu/menu_internal.h"

using namespace Gdiplus;
using namespace menu_detail;

namespace {

// как оживает значок при наведении - по имени его картинки
enum class Anim { Bounce, Lid, Drop, Rocket, Beat, Wobble, Tilt, Turn };

Anim AnimFor(const std::wstring& png) {
    std::wstring n = Lower(png.substr(png.find_last_of(L"\\/") + 1));
    auto has = [&](const wchar_t* s) { return n.find(s) != std::wstring::npos; };
    if (has(L"recycle") || has(L"trash")) return Anim::Lid;
    if (has(L"download")) return Anim::Drop;
    if (has(L"project") || has(L"rocket")) return Anim::Rocket;
    if (has(L"music")) return Anim::Beat;
    if (has(L"code") || has(L"editor") || has(L"terminal")) return Anim::Wobble;
    if (has(L"study")) return Anim::Tilt;
    if (has(L"apps")) return Anim::Turn;
    return Anim::Bounce;
}

// размытие только альфы (тень подписи - один цвет, каналы цвета не нужны)
void BlurAlpha(std::vector<uint8_t>& a, int w, int h, int r) {
    std::vector<uint8_t> t(a.size());
    auto pass = [&](const uint8_t* src, uint8_t* dst, int len, int count, int step, int line) {
        for (int l = 0; l < count; ++l) {
            const uint8_t* s = src + (size_t)l * line;
            uint8_t* d = dst + (size_t)l * line;
            int sum = 0, n = 2 * r + 1;
            auto at = [&](int i) { return (int)s[(size_t)std::clamp(i, 0, len - 1) * step]; };
            for (int i = -r; i <= r; ++i) sum += at(i);
            for (int i = 0; i < len; ++i) {
                d[(size_t)i * step] = (uint8_t)(sum / n);
                sum += at(i + r + 1) - at(i - r);
            }
        }
    };
    for (int k = 0; k < 2; ++k) {
        pass(a.data(), t.data(), w, h, 1, w);
        pass(t.data(), a.data(), h, w, w, 1);
    }
}

// Подписи секторов: на стекле и сотах тонкий шрифт теряется - крупнее и настоящим жирным Segoe UI.
Gdiplus::FontFamily* LabelFamily() {
    static Gdiplus::FontFamily* fam = new Gdiplus::FontFamily(L"Segoe UI");
    return fam->GetLastStatus() == Gdiplus::Ok ? fam : Family();
}

}  // namespace

// Группы различаются оттенком в пределах темы: от одного края кольца к другому ±20°.
double RadialMenu::RootHue(int root) const {
    int n = ring_ ? (int)ring_->size() : 0;
    if (n < 2 || root < 0 || winMode_) return 0;
    return (root / (n - 1.0) - 0.5) * 40.0;
}

Bitmap* RadialMenu::ItemIcon(const Item& it, int px) {
    if (!it.dir || !it.iconFull.empty() || it.special || it.full.empty()) return icons_.Get(IconPng(it), it.full, px);
    const project::Look& look = project::LookOf(it.full);
    if (!look.ownPng.empty())
        if (Bitmap* b = icons_.Get(look.ownPng, L"", px)) return b;
    if (!look.ownIco.empty())
        if (Bitmap* b = icons_.Get(L"", look.ownIco, px)) return b;
    Bitmap* folder = icons_.Get(IconPng(it), it.full, px);
    if (!folder || (look.kind == project::Kind::None && !look.letter)) return folder;

    std::wstring key = std::to_wstring(px) + L"|" + it.full;
    auto found = composed_.find(key);
    if (found != composed_.end()) return found->second.get();
    PERF_SCOPE("menu.ComposeIcon");
    auto bmp = std::make_unique<Bitmap>(px, px, PixelFormat32bppPARGB);
    {
        Graphics g(bmp.get());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        g.DrawImage(folder, 0, 0, px, px);
        if (const wchar_t* k = project::KindIcon(look.kind)) {
            // значок типа - в правом нижнем углу папки, как бейдж
            int bs = (int)(px * 0.56 + 0.5);
            if (Bitmap* badge = icons_.Get(ExeDir() + L"\\icons\\" + k, L"", bs))
                g.DrawImage(badge, px - bs + (int)(px * 0.04), px - bs + (int)(px * 0.04), bs, bs);
        } else {
            // монограмма: первая буква на папке
            wchar_t s[2] = {look.letter, 0};
            Font font(Family(), (REAL)(px * 0.40), FontStyleBold, UnitPixel);
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            RectF box(0, (REAL)(px * 0.12), (REAL)px, (REAL)px * 0.88f);
            SolidBrush shade(Color(110, 60, 10, 30));
            RectF sh = box;
            sh.Offset(0, (REAL)(px * 0.03));
            g.DrawString(s, 1, &font, sh, &sf, &shade);
            SolidBrush white(Color(240, 255, 255, 255));
            g.DrawString(s, 1, &font, box, &sf, &white);
        }
    }
    Bitmap* raw = bmp.get();
    composed_[key] = std::move(bmp);
    return raw;
}

// Значок с оживлением: при наведении каждый двигается по-своему (крышка корзины, стрелка загрузок...).
void RadialMenu::DrawIcon(Graphics& g, Bitmap* icon, const Item& it, double x, double y, double sz, double hl,
                          double appear, bool hot) {
    PERF_SCOPE("menu.r.Icon");
    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, (REAL)appear, 0}, {0, 0, 0, 0, 1}}};
    ImageAttributes ia;
    ia.SetColorMatrix(&cm);
    ImageAttributes* attr = appear >= 0.999 ? nullptr : &ia;
    const REAL iw = (REAL)icon->GetWidth(), ih = (REAL)icon->GetHeight();
    auto part = [&](const RectF& dst, REAL sy, REAL sh) { g.DrawImage(icon, dst, 0, sy, iw, sh, UnitPixel, attr); };
    double t = hot && !fx::Game() ? (GetTickCount64() - hoverT_) / 1000.0 : 9.0;
    double dx = 0, dy = 0, ang = 0, scale = 1;
    Anim kind = AnimFor(IconPng(it));
    switch (kind) {
        case Anim::Lid: {
            // крышка (верхняя треть) приподнимается и откидывается, пока на корзину наведено
            double lift = hl;
            RectF body((REAL)x, (REAL)(y + sz * 0.30), (REAL)sz, (REAL)(sz * 0.70));
            part(body, ih * 0.30f, ih * 0.70f);
            GraphicsState st = g.Save();
            g.TranslateTransform((REAL)(x + sz * 0.16), (REAL)(y + sz * 0.30 - sz * 0.10 * lift));
            g.RotateTransform((REAL)(-18 * lift));
            part(RectF((REAL)(-sz * 0.16), (REAL)(-sz * 0.30), (REAL)sz, (REAL)(sz * 0.30)), 0, ih * 0.30f);
            g.Restore(st);
            return;
        }
        case Anim::Drop: {
            // стрелка падает в лоток и возвращается
            double k = t < 0.5 ? std::sin(kPi * t / 0.5) : 0;
            part(RectF((REAL)x, (REAL)(y + sz * 0.58), (REAL)sz, (REAL)(sz * 0.42)), ih * 0.58f, ih * 0.42f);
            part(RectF((REAL)x, (REAL)(y + sz * 0.12 * k), (REAL)sz, (REAL)(sz * 0.58)), 0, ih * 0.58f);
            return;
        }
        case Anim::Rocket:
            dy = -sz * 0.08 * EaseOut(t / 0.25) * hl;
            dx = sz * 0.03 * std::sin(t * 55) * std::exp(-t * 6);
            break;
        case Anim::Beat:
            scale = 1 + 0.10 * std::fabs(std::sin(t * kPi * 2.6)) * std::exp(-t * 2.2);
            break;
        case Anim::Wobble:
            ang = 14 * std::sin(t * 16) * std::exp(-t * 5);
            break;
        case Anim::Tilt:
            ang = -12 * std::sin(t * 11) * std::exp(-t * 4);
            dy = -sz * 0.05 * std::fabs(std::sin(t * 11)) * std::exp(-t * 4);
            break;
        case Anim::Turn:
            ang = hot ? 90 * EaseOutBack(t / 0.55) : 0;  // четыре плитки: после четверти оборота как были
            break;
        case Anim::Bounce:
            dy = -sz * 0.08 * std::fabs(std::sin(t * 9)) * std::exp(-t * 4.5);
            break;
    }
    if (ang == 0 && scale == 1) {
        RectF dst((REAL)(x + dx), (REAL)(y + dy), (REAL)sz, (REAL)sz);
        // 1:1 - без GDI+; затухающая подсветка оставляет от масштаба миллионные доли пикселя
        if (!attr && std::fabs(sz - iw) < 0.01 && iw == ih && Blit(icon, dst.X, dst.Y)) return;
        part(dst, 0, ih);
        return;
    }
    GraphicsState st = g.Save();
    g.TranslateTransform((REAL)(x + sz / 2 + dx), (REAL)(y + sz / 2 + dy));
    g.RotateTransform((REAL)ang);
    g.ScaleTransform((REAL)scale, (REAL)scale);
    part(RectF((REAL)(-sz / 2), (REAL)(-sz / 2), (REAL)sz, (REAL)sz), 0, ih);
    g.Restore(st);
}

// Счётчик у значка: капля цвета группы с белым текстом, центр - в (x, y).
void RadialMenu::DrawBadge(Graphics& g, const std::wstring& text, double x, double y, Color fill, double appear) {
    if (appear <= 0.05) return;
    Font font(Family(), (REAL)(10.5 * geo_.k), FontStyleBold, UnitPixel);
    RectF tb;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), StringFormat::GenericTypographic(), &tb);
    REAL h = (REAL)(16 * geo_.k), w = std::max(h, tb.Width + (REAL)(9 * geo_.k));
    RectF r((REAL)(x - w / 2), (REAL)(y - h / 2), w, h);
    GraphicsPath pill;
    pill.AddArc(r.X, r.Y, h, h, 90, 180);
    pill.AddArc(r.X + w - h, r.Y, h, h, 270, 180);
    pill.CloseFigure();
    SolidBrush back(WithAlpha(fill, (BYTE)(240 * appear)));
    g.FillPath(&back, &pill);
    Pen edge(Color((BYTE)(110 * appear), 255, 255, 255), (REAL)(1.0 * geo_.k));
    g.DrawPath(&edge, &pill);
    SolidBrush ink(Color((BYTE)(255 * appear), 255, 255, 255));
    g.DrawString(text.c_str(), -1, &font, PointF(r.X + (w - tb.Width) / 2, r.Y + (h - tb.Height) / 2),
                 StringFormat::GenericTypographic(), &ink);
}

float RadialMenu::TextWidth(const std::wstring& s) {
    std::wstring key = s + L"|" + std::to_wstring((int)(LabelPx() * 10));
    auto it = textW_.find(key);
    if (it != textW_.end()) return it->second;
    static Bitmap one(1, 1, PixelFormat32bppPARGB);
    Graphics g(&one);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    Font font(LabelFamily(), LabelPx(), FontStyleBold, UnitPixel);
    RectF tb;
    g.MeasureString(s.c_str(), -1, &font, PointF(0, 0), StringFormat::GenericTypographic(), &tb);
    return textW_[key] = tb.Width;
}

// Подпись с мягкой тенью: рисуется один раз в маленькую картинку, дальше только копируется.
// Тень гасит рябь сот под буквами.
Bitmap* RadialMenu::LabelBitmap(const std::wstring& text, bool bright) {
    Palette pal = Pal(cfg_);
    Color tc = pal.text, sc = pal.shadow;
    std::wstring key = text + (bright ? L"|b|" : L"|d|") + std::to_wstring((int)(LabelPx() * 10)) + L"|" +
                       std::to_wstring(tc.GetValue()) + L"|" + std::to_wstring(sc.GetValue());
    auto found = labels_.find(key);
    if (found != labels_.end()) return found->second.bmp.get();
    PERF_SCOPE("menu.LabelBitmap");
    std::vector<std::wstring> lines = Split(text, L'\n');
    double lh = LabelPx() * 1.22, pad = 4 * geo_.k;
    float tw = 0;
    for (auto& l : lines) tw = std::max(tw, TextWidth(l));
    int w = (int)(tw + 2 * pad + 2), h = (int)(lines.size() * lh + 2 * pad + 1);
    auto bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    Font font(LabelFamily(), LabelPx(), FontStyleBold, UnitPixel);
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetAlignment(StringAlignmentCenter);
    auto drawText = [&](Graphics& g, Brush& b) {
        for (size_t i = 0; i < lines.size(); ++i)
            g.DrawString(lines[i].c_str(), -1, &font, RectF(0, (REAL)(pad + i * lh), (REAL)w, (REAL)lh), &sf, &b);
    };
    {
        // тень: буквы, размытые по альфе, цветом тени
        Graphics g(bmp.get());
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        SolidBrush b(Color(255, 0, 0, 0));
        drawText(g, b);
    }
    BitmapData bd;
    Rect r(0, 0, w, h);
    if (bmp->LockBits(&r, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Ok) {
        std::vector<uint8_t> a((size_t)w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) a[(size_t)y * w + x] = ((uint32_t*)((BYTE*)bd.Scan0 + y * bd.Stride))[x] >> 24;
        BlurAlpha(a, w, h, std::max(1, (int)(1.8 * geo_.k + 0.5)));
        for (int y = 0; y < h; ++y) {
            auto* row = (uint32_t*)((BYTE*)bd.Scan0 + y * bd.Stride);
            for (int x = 0; x < w; ++x) {
                uint32_t al = std::min(255u, (uint32_t)a[(size_t)y * w + x] * 3u) * 235u / 255u;
                row[x] = (al << 24) | ((sc.GetR() * al / 255) << 16) | ((sc.GetG() * al / 255) << 8) |
                         (sc.GetB() * al / 255);
            }
        }
        bmp->UnlockBits(&bd);
    }
    {
        Graphics g(bmp.get());
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        SolidBrush b(WithAlpha(tc, bright ? 255 : 240));
        drawText(g, b);
    }
    Bitmap* raw = bmp.get();
    labels_[key] = {std::move(bmp), w, h};
    return raw;
}

// Сколько места у подписи по горизонтали на радиусе rad: от точки на середине сектора в обе стороны,
// пока строка высотой 2*halfH целиком внутри сектора и не задевает значок. Стороны считаются
// отдельно, подпись потом встаёт по центру свободного места (x, y).
double RadialMenu::FitWidth(const Ring& r, double a0, double a1, double rad, double halfH, double& x, double& y) const {
    const double cx = center_.x, cy = center_.y, m = 3 * geo_.k;
    double mid = (a0 + a1) / 2, midR = Rad(AbsAngle(mid));
    double ix = cx + std::cos(midR) * r.rIcon, iy = cy + std::sin(midR) * r.rIcon, ir = r.iconPx * 0.52;
    double lx = cx + std::cos(midR) * rad, ly = cy + std::sin(midR) * rad;
    auto inside = [&](double px, double py) {
        double dx = px - cx, dy = py - cy, d = std::hypot(dx, dy);
        if (d < r.rIn + m || d > r.rOut - m) return false;
        if (std::hypot(px - ix, py - iy) < ir) return false;
        double a = std::atan2(dy, dx) * 180.0 / kPi;
        double rel = full_ ? a - 90.0 : left_ ? a : 180.0 - a;
        while (rel < mid - 180) rel += 360;
        while (rel > mid + 180) rel -= 360;
        double pad = m / d * 180.0 / kPi;
        return rel >= a0 + pad && rel <= a1 - pad;
    };
    auto fits = [&](double px) { return inside(px, ly - halfH) && inside(px, ly) && inside(px, ly + halfH); };
    x = lx;
    y = ly;
    if (!fits(lx)) return 0;
    // шагами от точки наружу: за значком место бывает снова свободно, и деление пополам его перепрыгивает
    double side[2], stepPx = 3 * geo_.k;
    for (int s = 0; s < 2; ++s) {
        double t = 0;
        while (t < 280 * geo_.k && fits(lx + (s ? t + stepPx : -t - stepPx))) t += stepPx;
        double lo = t, hi = t + stepPx;  // уточнить край внутри последнего шага
        for (int i = 0; i < 3; ++i) {
            double m2 = (lo + hi) / 2;
            (fits(lx + (s ? m2 : -m2)) ? lo : hi) = m2;
        }
        side[s] = lo;
    }
    x = lx + (side[1] - side[0]) / 2;
    return side[0] + side[1];
}

void RadialMenu::DrawLabel(Graphics& g, const Ring& r, double a0, double a1, const Item& it, double hl, double appear) {
    PERF_SCOPE("menu.r.Label");
    // раскладка подписи зависит только от сектора и имени: пока кольцо стоит, считаем её один раз
    wchar_t geo[96];
    swprintf(geo, 96, L"|%.1f|%.1f|%.0f|%.0f|%.0f|%d", a0, a1, r.rIn * 2, r.rOut * 2, r.rLabel * 2, r.iconPx);
    std::wstring fitKey = it.name + geo;
    auto cached = labelFit_.find(fitKey);
    if (cached == labelFit_.end()) cached = labelFit_.emplace(fitKey, LayoutLabel(r, a0, a1, it)).first;
    const LabelFit& lf = cached->second;
    if (lf.text.empty()) return;
    labelPx_ = lf.px;
    Bitmap* bmp = LabelBitmap(lf.text, hl > 0.5);
    labelPx_ = 0;
    if (!bmp) return;
    REAL w = (REAL)bmp->GetWidth(), h = (REAL)bmp->GetHeight();
    RectF dst((REAL)(lf.x - w / 2), (REAL)(lf.y - h / 2), w, h);
    if (appear >= 0.999) {
        if (!Blit(bmp, dst.X, dst.Y)) g.DrawImage(bmp, dst);
    } else {
        ColorMatrix cm = {
            {{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, (REAL)appear, 0}, {0, 0, 0, 0, 1}}};
        ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        g.DrawImage(bmp, dst, 0, 0, w, h, UnitPixel, &ia);
    }
}

// Где разорвать слово переносом: самая длинная голова, что влезает (fits), не короче 3 букв и с хвостом не
// короче 3; лучше - после гласной перед согласной («Расши-рения», «Бранд-мауэр»); 0 - нигде.
static size_t HyphenCut(const std::wstring& w, const std::function<bool(const std::wstring&)>& fits) {
    auto vowel = [](wchar_t c) { return wcschr(L"аеёиоуыэюяАЕЁИОУЫЭЮЯaeiouyAEIOUY", c) != nullptr; };
    size_t best[3] = {0, 0, 0};  // гласная|согласная+гласная («По-чинка»), согласная|согласная+гласная, любая
    for (size_t k = w.size() >= 6 ? w.size() - 3 : 0; k >= 3; --k) {
        if (!IsCharAlphaW(w[k - 1]) || !IsCharAlphaW(w[k]) || wcschr(L"ьъйЬЪЙ", w[k]) || !fits(w.substr(0, k)))
            continue;
        bool cv = !vowel(w[k]) && k + 1 < w.size() && vowel(w[k + 1]);
        int kind = cv && vowel(w[k - 1]) ? 0 : cv ? 1 : 2;
        if (!best[kind]) best[kind] = k;
    }
    return best[0] ? best[0] : best[1] ? best[1] : best[2];
}

RadialMenu::LabelFit RadialMenu::LayoutLabel(const Ring& r, double a0, double a1, const Item& it) {
    PERF_SCOPE("menu.LayoutLabel");
    LabelFit out;
    // крупно, если влезает целиком; иначе мельче (до 0,8 - ещё читается), но не обрезая
    // перенос внутри слова - только когда без него обрезается при любом размере
    for (bool hyphen : {false, true}) {
        for (float scale : {1.15f, 1.04f, 0.95f, 0.87f, 0.8f}) {
            labelPx_ = geo_.font * scale;
            out = LayoutLabelAt(r, a0, a1, it, hyphen);
            out.px = labelPx_;
            if (out.text.find(L'…') == std::wstring::npos && !out.text.empty()) break;
        }
        if (out.text.find(L'…') == std::wstring::npos) break;
    }
    labelPx_ = 0;
    return out;
}

RadialMenu::LabelFit RadialMenu::LayoutLabelAt(const Ring& r, double a0, double a1, const Item& it, bool hyphen) {
    LabelFit out;
    double lh = LabelPx() * 1.22;
    // где подписи просторнее: на своём радиусе или чуть ближе к центру (у края у горизонтальных секторов тесно)
    double f0 = (r.rLabel - r.rIn) / (r.rOut - r.rIn);
    auto best = [&](double halfH, double& x, double& y) {
        double w = -1;
        for (double f : {f0, f0 - 0.04, f0 - 0.08, f0 - 0.12, f0 + 0.04}) {
            double px = 0, py = 0, pw = FitWidth(r, a0, a1, r.rIn + (r.rOut - r.rIn) * f, halfH, px, py);
            if (pw > w + 4 * geo_.k) {
                w = pw;
                x = px;
                y = py;
            }
        }
        return w;
    };
    double lx = 0, ly = 0, w1 = best(lh * 0.55, lx, ly);
    if (w1 < 16 * geo_.k) return out;
    const std::wstring& name = it.name;

    auto ellipsize = [&](const std::wstring& s, double w) {
        if (TextWidth(s) <= w) return s;
        size_t lo = 0, hi = s.size();
        while (lo < hi) {  // самая длинная голова строки, которая с «…» влезает
            size_t k = (lo + hi + 1) / 2;
            if (TextWidth(Trim(s.substr(0, k)) + L"…") <= w)
                lo = k;
            else
                hi = k - 1;
        }
        return Trim(s.substr(0, lo)) + L"…";
    };
    std::wstring text;
    if (TextWidth(name) <= w1) {
        text = name;
    } else {
        // две строки, если хватает высоты: по пробелам, после дефиса и на стыке слов внутри имени -
        // «Might and Magic / Heroes VI», «cpp-docs- / panel», «Auto / Translator»
        double x2, y2, w2 = best(lh * 1.05, x2, y2);
        if (w2 >= 24 * geo_.k) {
            std::vector<std::wstring> parts;
            std::wstring cur;
            for (size_t i = 0; i < name.size(); ++i) {
                wchar_t c = name[i];
                if (c == L' ') {
                    parts.push_back(cur);
                    cur = L" ";
                    continue;
                }
                cur += c;
                bool last = i + 1 == name.size();
                if (!last && (c == L'-' || c == L'_' || (IsCharLowerW(c) && IsCharUpperW(name[i + 1])))) {
                    parts.push_back(cur);
                    cur.clear();
                }
            }
            parts.push_back(cur);
            std::wstring first;
            size_t i = 0;
            for (; i < parts.size(); ++i) {
                std::wstring tryLine = Trim(first + parts[i]);
                if (TextWidth(tryLine) > w2) break;
                first += parts[i];
            }
            first = Trim(first);
            if (hyphen && first.empty() && i < parts.size()) {  // первое слово само не влезает: перенос внутри слова
                std::wstring word = Trim(parts[i]);
                size_t cut = HyphenCut(word, [&](const std::wstring& head) { return TextWidth(head + L"-") <= w2; });
                if (cut) {
                    first = word.substr(0, cut) + L"-";
                    parts[i] = word.substr(cut);
                }
            }
            if (!first.empty() && i < parts.size()) {
                std::wstring rest;
                for (; i < parts.size(); ++i) rest += parts[i];
                text = first + L"\n" + ellipsize(Trim(rest), w2);
                lx = x2;
                ly = y2;
            }
        }
        if (text.empty()) text = ellipsize(name, w1);
    }
    out.text = text;
    out.x = lx;
    out.y = ly;
    return out;
}

// Карточка над кольцом: что это, сколько внутри, когда менялось, где лежит.
void RadialMenu::DrawInfoCard(Graphics& g, double grow, double alpha, bool below) {
    const Item* it = HoveredItem();
    if (!it || winMode_ || (it->special && it->special != 3 && it->special != 4) || alpha <= 0.01) return;
    double a = alpha * Clamp01((GetTickCount64() - keyT_ - 120) / 160.0);
    if (a <= 0.01) return;
    std::wstring kind, detail, path;
    if (it->special == 3) {  // действие сети: что сделает
        kind = it->path.empty() ? L"Группа" : L"Действие";
        detail = it->note;
    } else if (it->special == 4) {
        kind = L"Кольцо программы";
        detail = Plural((int)it->children.size(), L"команда", L"команды", L"команд");
    } else if (macro::Is(it->path)) {
        kind = L"Нажать";
        detail = macro::Describe(it->path);
    } else if (it->full.empty()) {
        kind = L"Группа";
        int n = (int)it->children.size();
        if (n) detail = Plural(n, L"пункт", L"пункта", L"пунктов");
    } else {
        // у пунктов из настроек свой значок и смысл - тип проекта по файлам им не угадываем
        const project::Info& in = project::InfoOf(it->full, it->dir, it->iconFull.empty());
        kind = in.kind;
        detail = in.detail;
        path = in.path;
    }
    // у действий сети пояснение длинное: ему две строки с переносом вместо строки пути
    bool note2 = it->special == 3;
    std::wstring line2 = note2 ? detail : kind + (detail.empty() ? L"" : L"   |   " + detail);

    const double k = geo_.k, pad = 10 * k, isz = 38 * k;
    double fTitle = geo_.font * 1.12, fSmall = geo_.font * 0.92;
    double ch = pad * 2 + fTitle * 1.3 + fSmall * 1.35 * (path.empty() && !note2 ? 1 : 2);
    double cw = std::min(geo_.w - 24 * k, 340 * k);
    double cx = full_ ? (geo_.w - cw) / 2 : left_ ? 12 * k : geo_.w - 12 * k - cw;
    double cy = below ? center_.y + geo_.r4 * grow + 14 * k : center_.y - geo_.r4 * grow - 14 * k - ch;
    RectF card((REAL)cx, (REAL)cy, (REAL)cw, (REAL)ch);
    Palette pal = Pal(cfg_);
    BYTE ba = (BYTE)(240 * a);
    GraphicsPath p;
    RoundRectPath(p, card, (float)(12 * k));
    SolidBrush bg(WithAlpha(pal.pillBg, ba));
    g.FillPath(&bg, &p);
    double hue = mode_ == Mode::Root ? RootHue(subParent_) : hoverRoot_ >= 0 ? RootHue(hoverRoot_) : 0;
    Pen edge(WithAlpha(HueShift(pal.pillEdge, hue), (BYTE)(200 * a)), (REAL)(1.2 * k));
    g.DrawPath(&edge, &p);

    double tx = cx + pad;
    if (Bitmap* icon = ItemIcon(*it, (int)isz)) {
        ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, (REAL)a, 0}, {0, 0, 0, 0, 1}}};
        ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        g.DrawImage(icon, RectF((REAL)tx, (REAL)(cy + (ch - isz) / 2), (REAL)isz, (REAL)isz), 0, 0,
                    (REAL)icon->GetWidth(), (REAL)icon->GetHeight(), UnitPixel, &ia);
        tx += isz + 10 * k;
    }
    double tw = cx + cw - pad - tx, y = cy + pad;
    StringFormat sf(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    Font title(Family(), (REAL)fTitle, FontStyleRegular, UnitPixel);
    SolidBrush tb(WithAlpha(pal.text, (BYTE)(255 * a)));
    g.DrawString(it->name.c_str(), -1, &title, RectF((REAL)tx, (REAL)y, (REAL)tw, (REAL)(fTitle * 1.35)), &sf, &tb);
    y += fTitle * 1.3;
    Font small(Family(), (REAL)fSmall, FontStyleRegular, UnitPixel);
    SolidBrush mb(WithAlpha(pal.text, (BYTE)(175 * a)));
    if (note2) {
        StringFormat wf;
        wf.SetTrimming(StringTrimmingEllipsisWord);
        g.DrawString(line2.c_str(), -1, &small, RectF((REAL)tx, (REAL)y, (REAL)tw, (REAL)(fSmall * 2.75)), &wf, &mb);
    } else {
        g.DrawString(line2.c_str(), -1, &small, RectF((REAL)tx, (REAL)y, (REAL)tw, (REAL)(fSmall * 1.4)), &sf, &mb);
    }
    if (!path.empty()) {
        y += fSmall * 1.35;
        StringFormat pf(StringFormatFlagsNoWrap);
        pf.SetTrimming(StringTrimmingEllipsisPath);  // длинный путь сокращается в середине
        SolidBrush pb(WithAlpha(pal.text, (BYTE)(120 * a)));
        g.DrawString(path.c_str(), -1, &small, RectF((REAL)tx, (REAL)y, (REAL)tw, (REAL)(fSmall * 1.4)), &pf, &pb);
    }
}
