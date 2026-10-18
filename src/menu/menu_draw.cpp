// Рисование колец: стекло, секторы, плашки, ступица.
#include "menu/menu.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "core/files.h"
#include "core/launch.h"
#include "core/perf.h"
#include "core/macro.h"
#include "core/text.h"
#include "core/themes.h"
#include "core/winlist.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/hex.h"
#include "gfx/sound.h"
#include "menu/menu_internal.h"

using namespace Gdiplus;
using namespace menu_detail;

namespace {

// Отрезки строки y, лежащие в кольце r0..r1 вокруг (cx, cy) и внутри [lo, hi): без углов квадрата и без дырки.
template <class F>
void AnnulusSpans(double cx, double cy, int y, double r0, double r1, int lo, int hi, F&& fn) {
    double dy = y + 0.5 - cy;
    if (std::fabs(dy) >= r1) return;
    double ho = std::sqrt(r1 * r1 - dy * dy);
    int a = std::max(lo, (int)std::floor(cx - ho)), b = std::min(hi, (int)std::ceil(cx + ho) + 1);
    if (r0 > 0 && std::fabs(dy) < r0) {
        double hh = std::sqrt(r0 * r0 - dy * dy);
        int c = std::max(a, (int)std::ceil(cx - hh)), d = std::min(b, (int)std::floor(cx + hh));
        if (c < d) {
            if (a < c) fn(a, c);
            if (d < b) fn(d, b);
            return;
        }
    }
    if (a < b) fn(a, b);
}

}  // namespace

// Снимок того, что под меню, уменьшенный в 4 раза, размытый и растянутый обратно: из него делается «стекло».
// Всё на своих простых циклах: StretchBlt с экрана и растягивание GDI+ стоили по 10 мс каждое.
void RadialMenu::CaptureBackdrop() {
    PERF_SCOPE("menu.CaptureBackdrop");
    blurPx_ = {};
    if (!cfg_->blur || fx::Game()) return;  // в игровом режиме без стекла
    constexpr int kDown = 4;
    int w = geo_.w, h = geo_.h;
    int sw = std::max(1, w / kDown), sh = std::max(1, h / kDown);

    // копия экрана под будущим окном (оно ещё не показано, в снимок не попадёт)
    HDC scr = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(scr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* px = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &px, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    BitBlt(dc, 0, 0, w, h, scr, origin_.x, origin_.y, SRCCOPY);
    GdiFlush();
    ReleaseDC(nullptr, scr);

    std::vector<uint32_t> small((size_t)sw * sh);
    Downscale((const uint32_t*)px, w, h, small.data(), sw, sh, kDown);
    BoxBlur(small.data(), sw, sh, std::max(1, (int)(2 * geo_.k)));

    blurPx_.resize((size_t)w * h);
    UpscaleBilinear(small.data(), sw, sh, blurPx_.data(), w, h, w);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

namespace {

// Пиксель src (PARGB) поверх dst с покрытием cov 0..255, как заливка GDI+ со сглаживанием.
// Каналы парами (красный с синим, альфа с зелёным): одно умножение на два канала.
inline uint32_t Scale(uint32_t c, uint32_t k) {
    uint32_t rb = (c & 0xFF00FF) * k + 0x800080, ag = (c >> 8 & 0xFF00FF) * k + 0x800080;
    rb = (rb + (rb >> 8 & 0xFF00FF)) >> 8 & 0xFF00FF;  // деление на 255 с округлением
    ag = (ag + (ag >> 8 & 0xFF00FF)) >> 8 & 0xFF00FF;
    return rb | ag << 8;
}

inline uint32_t Over(uint32_t dst, uint32_t src, uint32_t cov) {
    uint32_t s = cov == 255 ? src : Scale(src, cov);
    uint32_t sa = s >> 24;
    return sa == 255 ? s : sa ? s + Scale(dst, 255 - sa) : dst;
}

}  // namespace

bool RadialMenu::Blit(Bitmap* bmp, double x, double y) {
    if (!blitOk_) return false;
    const int iw = (int)bmp->GetWidth(), ih = (int)bmp->GetHeight();
    BitmapData bd;
    Rect r(0, 0, iw, ih);
    if (bmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Ok) return false;
    // центр пикселя кадра j попадает в картинку на j - x: смешиваем соседние пиксели с весами дробной части
    const int X0 = (int)std::floor(x), Y0 = (int)std::floor(y);
    const uint32_t fx = (uint32_t)std::lround((x - X0) * 256), fy = (uint32_t)std::lround((y - Y0) * 256);
    const uint32_t w00 = fx * fy >> 8, w10 = (256 - fx) * fy >> 8, w01 = fx * (256 - fy) >> 8,
                   w11 = 256 - w00 - w10 - w01;
    auto at = [&](int i, int j) -> uint32_t {
        return i < 0 || j < 0 || i >= iw || j >= ih ? 0 : ((const uint32_t*)((const BYTE*)bd.Scan0 + j * bd.Stride))[i];
    };
    auto* px = static_cast<uint32_t*>(bits_);
    const int jx0 = std::max(0, X0), jx1 = std::min(geo_.w, X0 + iw + 1);
    const int jy0 = std::max(0, Y0), jy1 = std::min(geo_.h, Y0 + ih + 1);
    for (int jy = jy0; jy < jy1; ++jy) {
        int sy = jy - Y0;
        for (int jx = jx0; jx < jx1; ++jx) {
            int sx = jx - X0;
            uint32_t a = at(sx - 1, sy - 1), b = at(sx, sy - 1), c = at(sx - 1, sy), d = at(sx, sy);
            if (!(a | b | c | d)) continue;
            uint32_t rb =
                (a & 0xFF00FF) * w00 + (b & 0xFF00FF) * w10 + (c & 0xFF00FF) * w01 + (d & 0xFF00FF) * w11 + 0x800080;
            uint32_t ag = (a >> 8 & 0xFF00FF) * w00 + (b >> 8 & 0xFF00FF) * w10 + (c >> 8 & 0xFF00FF) * w01 +
                          (d >> 8 & 0xFF00FF) * w11 + 0x800080;
            uint32_t src = (rb >> 8 & 0xFF00FF) | (ag & 0xFF00FF00);
            uint32_t& dst = px[(size_t)jy * geo_.w + jx];
            dst = Over(dst, src, 255);
        }
    }
    bmp->UnlockBits(&bd);
    return true;
}

void RadialMenu::DrawRing(Graphics& g, const std::vector<Sector>& secs, GraphicsPath* clip) {
    const double cx = center_.x, cy = center_.y;
    const double gap = 3.0 * geo_.k;
    const double sgn = left_ || full_ ? 1 : -1;
    struct Shape {
        GraphicsPath path;
        double rOut = 0, o0 = 0, o1 = 0, i0 = 0, i1 = 0;
        bool ok = false;
    };
    std::vector<Shape> shapes(secs.size());
    double reach = 0, hole = 1e9;
    bool masked = false;
    for (size_t k = 0; k < secs.size(); ++k) {
        const Sector& s = secs[k];
        Shape& sh = shapes[k];
        sh.rOut = s.ring.rOut + 3.5 * geo_.k * s.hl;  // наведённый сектор чуть выдвигается наружу
        double inO = gap / 2 / sh.rOut * 180 / kPi, inI = gap / 2 / s.ring.rIn * 180 / kPi;
        sh.o0 = s.a0 + inO, sh.o1 = s.a1 - inO, sh.i0 = s.a0 + inI, sh.i1 = s.a1 - inI;
        if (sh.o1 <= sh.o0 || sh.i1 <= sh.i0) continue;
        sh.ok = true;
        double ri = s.ring.rIn;
        sh.path.AddArc((REAL)(cx - sh.rOut), (REAL)(cy - sh.rOut), (REAL)(2 * sh.rOut), (REAL)(2 * sh.rOut),
                       (REAL)AbsAngle(sh.o0), (REAL)(sgn * (sh.o1 - sh.o0)));
        sh.path.AddArc((REAL)(cx - ri), (REAL)(cy - ri), (REAL)(2 * ri), (REAL)(2 * ri), (REAL)AbsAngle(sh.i1),
                       (REAL)(-sgn * (sh.i1 - sh.i0)));
        sh.path.CloseFigure();
        if (s.appear > 0.5) {
            masked = true;
            reach = std::max(reach, sh.rOut);
            hole = std::min(hole, ri);
        }
    }
    // Покрытие секторов лежит в кольце hole..reach: строки обходятся только там - без углов квадрата и без
    // дырки в середине. Чистка, заливка и наложение идут по одним и тем же отрезкам строки.
    auto spans = [&](int y, int lo, int hi, auto&& fn) { AnnulusSpans(cx, cy, y, hole - 3, reach + 3, lo, hi, fn); };

    // Кольцо, которое не меняется (водят по другому кольцу), не рисуется заново: его пиксели берутся из прошлого
    // кадра. Под кольцом только подложка, а кольца друг друга не касаются (между r2 и r3 зазор), поэтому
    // кусок кадра в его полосе зависит лишь от подложки и состояния секторов - это и есть ключ.
    RingCache& rc = ringCache_[!secs.empty() && secs[0].ring.sub ? 1 : 0];
    const bool glassOn = !blurPx_.empty(), hexOn = hexOn_ && !hexPx_.empty();
    std::vector<double> key = {baseNow_.ext,        baseNow_.catcherR, (double)baseNow_.color, clip ? 1.0 : 0.0,
                               glassOn ? 1.0 : 0.0, hexOn ? 1.0 : 0.0, (double)geo_.w,         (double)geo_.h};
    bool still = true;
    double bandLo = 1e9, bandHi = 0;
    ULONGLONG nowMs = GetTickCount64();
    for (const Sector& s : secs) {
        // значок под курсором играет анимацию около секунды - такой кадр не повторяется
        if (s.hot && nowMs - hoverT_ < 1500) still = false;
        if (hexOn && s.hl > 0.02) {  // приподнятая сота идёт за курсором
            hex::Cell c = hex::At(cursorLocal_.x, cursorLocal_.y, 13 * geo_.k);
            key.insert(key.end(), {(double)c.q, (double)c.r});
        }
        key.insert(key.end(),
                   {s.a0, s.a1, s.ring.rIn, s.ring.rOut, (double)s.ring.iconPx, s.hl, s.appear, s.active ? 1.0 : 0.0,
                    s.hue, s.hot ? 1.0 : 0.0, (double)std::hash<std::wstring>()(s.it->name + L"|" + s.it->full)});
        bandLo = std::min(bandLo, s.ring.rIn);
        bandHi = std::max(bandHi, s.ring.rOut);
    }
    // полоса кольца с запасом под выдвинутый сектор и светящуюся кромку
    bandLo -= 4;
    bandHi += 3.5 * geo_.k + 2.5 * gap + 6;
    const size_t total = (size_t)geo_.w * geo_.h;
    auto* px = static_cast<uint32_t*>(bits_);
    auto band = [&](auto&& fn) {
        int ya = std::max(0, (int)(cy - bandHi)), yb = std::min(geo_.h, (int)(cy + bandHi) + 1);
        for (int y = ya; y < yb; ++y)
            AnnulusSpans(cx, cy, y, bandLo, bandHi, 0, geo_.w,
                         [&](int a, int b) { fn((size_t)y * geo_.w + a, b - a); });
    };
    if (still && rc.ok && rc.key == key && rc.px.size() == total) {
        PERF_SCOPE("menu.r.Cached");
        band([&](size_t at, int n) { memcpy(px + at, rc.px.data() + at, (size_t)n * 4); });
        return;
    }

    // Стекло и соты: маска всех проявившихся секторов одной сплошной заливкой, дальше проход по пикселям.
    // Слои те же, что были у заливок TextureBrush: стекло под градиентом, соты над ним.
    masked = masked && (glassOn || hexOn);
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    if (masked) {
        PERF_SCOPE("menu.r.Mask");
        x0 = std::max(0, (int)(cx - reach - 3)), x1 = std::min(geo_.w, (int)(cx + reach + 4));
        y0 = std::max(0, (int)(cy - reach - 3)), y1 = std::min(geo_.h, (int)(cy + reach + 4));
        maskPx_.resize((size_t)geo_.w * geo_.h);
        for (int y = y0; y < y1; ++y)
            spans(y, x0, x1, [&](int a, int b) { memset(&maskPx_[(size_t)y * geo_.w + a], 0, (size_t)(b - a) * 4); });
        Bitmap mb(geo_.w, geo_.h, geo_.w * 4, PixelFormat32bppPARGB, (BYTE*)maskPx_.data());
        Graphics mg(&mb);
        mg.SetSmoothingMode(g.GetSmoothingMode());
        mg.SetPixelOffsetMode(g.GetPixelOffsetMode());
        if (clip) mg.SetClip(clip);
        SolidBrush white(Color(255, 255, 255, 255));
        for (size_t k = 0; k < secs.size(); ++k)
            if (shapes[k].ok && secs[k].appear > 0.5) mg.FillPath(&white, &shapes[k].path);
    }
    auto composite = [&](const std::vector<uint32_t>& src) {
        for (int y = y0; y < y1; ++y) {
            size_t row = (size_t)y * geo_.w;
            const uint32_t* m = &maskPx_[row];
            spans(y, x0, x1, [&](int a, int b) {
                for (int x = a; x < b; ++x) {
                    uint32_t cov = m[x] >> 24;
                    if (cov) px[row + x] = Over(px[row + x], src[row + x], cov);
                }
            });
        }
    };
    if (masked && glassOn) {
        PERF_SCOPE("menu.r.Glass");
        composite(blurPx_);  // размытая подложка: обои просвечивают сквозь кольцо
    }

    std::vector<Palette> pals(secs.size(), Pal(cfg_));
    for (size_t k = 0; k < secs.size(); ++k) {
        if (!shapes[k].ok) continue;
        const Sector& s = secs[k];
        Palette& pal = pals[k];
        if (s.hue != 0) {  // у каждой группы свой оттенок
            pal.hotIn = HueShift(pal.hotIn, s.hue);
            pal.hotOut = HueShift(pal.hotOut, s.hue);
            pal.active = HueShift(pal.active, s.hue);
            pal.rim = HueShift(pal.rim, s.hue);
        }
        Color base = s.ring.sub ? pal.baseSub : pal.base;
        bool glass = glassOn && s.appear > 0.5;
        BYTE alpha = (BYTE)((glass ? 178 : 232) * s.appear);
        // наведённый сектор переливается от центра к краю: малина -> коралл (в тёмной теме - один акцент)
        double t = std::max(s.hl, s.active ? 0.32 : 0.0);
        BYTE fillAlpha = std::max<BYTE>(alpha, (BYTE)(232 * s.appear * t));
        Color from = s.hl >= 0.32 || !s.active ? pal.hotIn : pal.active,
              to = s.hl >= 0.32 || !s.active ? pal.hotOut : pal.active;
        double mr = Rad(AbsAngle((s.a0 + s.a1) / 2));
        PointF pin((REAL)(cx + std::cos(mr) * s.ring.rIn), (REAL)(cy + std::sin(mr) * s.ring.rIn));
        PointF pout((REAL)(cx + std::cos(mr) * shapes[k].rOut), (REAL)(cy + std::sin(mr) * shapes[k].rOut));
        LinearGradientBrush fill(pin, pout, MixColor(base, from, t * 0.92, fillAlpha),
                                 MixColor(base, to, t * 0.92, fillAlpha));
        fill.SetWrapMode(WrapModeTileFlipXY);
        g.FillPath(&fill, &shapes[k].path);
    }

    if (masked && hexOn) {
        PERF_SCOPE("menu.r.Hex");
        composite(hexPx_);  // грани сотового стекла
    }

    blitOk_ = !clip;
    for (size_t k = 0; k < secs.size(); ++k) {
        if (!shapes[k].ok) continue;
        PERF_SCOPE("menu.r.Sector");
        const Sector& s = secs[k];
        Shape& sh = shapes[k];
        const Palette& pal = pals[k];
        bool glass = glassOn && s.appear > 0.5;
        if (hexOn && s.appear > 0.5 && s.hl > 0.02) DrawHexLift(g, sh.path, s.hl);
        Pen border(WithAlpha(pal.border, (BYTE)((glass ? 46 : 34) * s.appear)), 1.0f);
        g.DrawPath(&border, &sh.path);
        {  // тонкая полоса оттенка группы у внутреннего края: видно, чьё второе кольцо
            Pen band(WithAlpha(pal.rim, (BYTE)((70 + 90 * std::max(s.hl, s.active ? 1.0 : 0.0)) * s.appear)),
                     (REAL)(2.2 * geo_.k));
            double rb = s.ring.rIn + 2.4 * geo_.k;
            g.DrawArc(&band, (REAL)(cx - rb), (REAL)(cy - rb), (REAL)(2 * rb), (REAL)(2 * rb), (REAL)AbsAngle(sh.i0),
                      (REAL)(sgn * (sh.i1 - sh.i0)));
        }

        if (s.hl > 0.02) {  // светящаяся кромка снаружи
            Pen rim(WithAlpha(pal.rim, (BYTE)(230 * s.hl * s.appear)), (REAL)gap);
            double rr = sh.rOut + gap * 1.4;
            g.DrawArc(&rim, (REAL)(cx - rr), (REAL)(cy - rr), (REAL)(2 * rr), (REAL)(2 * rr), (REAL)AbsAngle(sh.o0),
                      (REAL)(sgn * (sh.o1 - sh.o0)));
        }

        double mid = Rad(AbsAngle((s.a0 + s.a1) / 2));
        if (Bitmap* icon = ItemIcon(*s.it, s.ring.iconPx)) {
            double sz = s.ring.iconPx * (0.7 + 0.3 * s.appear) * (1 + 0.1 * s.hl);
            double ix = cx + std::cos(mid) * s.ring.rIcon - sz / 2, iy = cy + std::sin(mid) * s.ring.rIcon - sz / 2;
            DrawIcon(g, icon, *s.it, ix, iy, sz, s.hl, s.appear, s.hot);
        }
        DrawLabel(g, s.ring, s.a0, s.a1, *s.it, s.hl, s.appear);
    }
    blitOk_ = false;
    // кольцо, которое второй кадр подряд не меняется, запоминаем: дальше оно будет копироваться
    if (still && rc.last == key) {
        rc.px.resize(total);
        band([&](size_t at, int n) { memcpy(rc.px.data() + at, px + at, (size_t)n * 4); });
        rc.key = key;
        rc.ok = true;
    }
    rc.last = std::move(key);
}
// Плашка с текстом у края экрана: путь вверху, поиск и подсказки внизу.
void RadialMenu::DrawPill(Graphics& g, const std::wstring& text, const std::wstring& iconFile, double y, double alpha,
                          bool accentEdge) {
    if (text.empty() || alpha <= 0.01) return;
    Font font(Family(), geo_.font, FontStyleRegular, UnitPixel);
    StringFormat sf(StringFormatFlagsNoWrap);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF tb;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &tb);
    double ph = geo_.font * 2.0, pad = 12 * geo_.k;
    Bitmap* icon = iconFile.empty() ? nullptr : icons_.Get(iconFile, L"", (int)(ph * 0.72));
    double iw = icon ? ph * 0.72 + 6 * geo_.k : 0;
    double maxW = geo_.w - 24 * geo_.k;
    double pw = std::min(maxW, tb.Width + 2 * pad + iw);
    double px = full_ ? (geo_.w - pw) / 2 : left_ ? 12 * geo_.k : geo_.w - 12 * geo_.k - pw;
    double rad = ph / 2;
    GraphicsPath pp;
    pp.AddArc((REAL)px, (REAL)y, (REAL)(2 * rad), (REAL)ph, 90, 180);
    pp.AddArc((REAL)(px + pw - 2 * rad), (REAL)y, (REAL)(2 * rad), (REAL)ph, 270, 180);
    pp.CloseFigure();
    BYTE a = (BYTE)(245 * Clamp01(alpha));
    Palette pal = Pal(cfg_);
    SolidBrush bg(WithAlpha(pal.pillBg, a));
    g.FillPath(&bg, &pp);
    Pen edge(accentEdge ? WithAlpha(pal.pillEdge, a) : WithAlpha(pal.border, (BYTE)(a * 0.3)), 1.2f);
    g.DrawPath(&edge, &pp);
    if (icon) {
        double s = ph * 0.72;
        g.DrawImage(icon, RectF((REAL)(px + pad * 0.7), (REAL)(y + (ph - s) / 2), (REAL)s, (REAL)s));
    }
    SolidBrush txt(WithAlpha(pal.text, a));
    RectF box((REAL)(px + pad + iw * 0.9), (REAL)y, (REAL)(pw - 2 * pad - iw * 0.9), (REAL)ph);
    g.DrawString(text.c_str(), -1, &font, box, &sf, &txt);
}

std::wstring RadialMenu::TopHint() const {
    if (mode_ != Mode::Root || navStack_.empty()) return {};
    std::wstring s = subParent_ >= 0 ? (*ring_)[subParent_].name : L"";  // из поиска - без сектора
    for (const Item& f : navStack_) s += (s.empty() ? L"" : L" / ") + f.name;
    return s;
}

std::wstring RadialMenu::BottomHint(double& alpha) const {
    alpha = 1;
    if (mode_ == Mode::Search) {
        std::wstring s = query_ + L"_";
        if (subItems_.empty()) s += L"   ничего не найдено";
        return s;
    }
    if (palette_ && query_.empty() && !HoveredItem()) {
        alpha = 0.8;
        return L"Печатайте: команда, папка, окно, раздел студии   |   Enter - выполнить";
    }
    if (dragging_) {
        if (targetKey_.empty()) return L"Наведите на папку";
        std::wstring q = L" «" + targetName_ + L"»";
        if (dragEffect_ & DROPEFFECT_MOVE) return L"Переместить в" + q;
        if (dragEffect_ & DROPEFFECT_COPY) return (targetDir_ ? L"Скопировать в" : L"Открыть в") + q;
        if (dragEffect_ & DROPEFFECT_LINK) return L"Ярлык в" + q;
        return L"Сюда нельзя";
    }
    const Item* it = HoveredItem();
    if (winMode_) {
        if (it && it->special == 2) return winlist::Title(it->win);
        alpha = 0.55;
        return L"Отпустите Alt над окном   |   средняя кнопка: закрыть";
    }
    if (it && !clickKey_.empty() && clickKey_ == hoverKey_ && GetTickCount64() - clickT_ <= GetDoubleClickTime())
        return netMode_ ? L"Ещё раз - выполнить" : L"Ещё раз - открыть";
    if (netMode_) {
        alpha = 0.55;
        if (!it) {  // ни на что не наведено - внизу состояние сети
            alpha = 1;
            return netStatus ? netStatus() : L"";
        }
        return it->path.empty() ? L"Наведите на действие" : L"2 клика: выполнить";
    }
    if (it && ToDialog(*it)) return it->dir ? L"Перейти сюда в диалоге" : L"Выбрать в диалоге";
    if (it && macro::Is(it->path)) {
        alpha = 0.55;
        return L"2 клика: нажать " + macro::Describe(it->path);
    }
    if (it && it->special == 4) {
        alpha = 0.55;
        return L"Команды этой программы";
    }
    if (it && !it->special && !it->full.empty()) {
        if (shift_ && ctrl_) return L"Показать в Проводнике";
        if (shift_) return L"Открыть в VS Code";
        if (ctrl_) return L"Терминал в этой папке";
    }
    alpha = 0.55;
    if (it && it->dir) return L"2 клика: открыть   |   ПКМ: внутрь   |   тянуть: вращать";
    if (it) return L"2 клика: открыть   |   тянуть: вращать";
    return L"Печатайте для поиска";
}

// Мягкая тень вокруг колец и почти прозрачная подложка (alpha 1), которая ловит клики.
// Обе круглые, поэтому считаются прямо в пикселях: прозрачность зависит только от расстояния до центра.
// Это в разы быстрее заливки градиентом GDI+ и заодно очищает кадр.
void RadialMenu::PaintBase(double ext, double catcherR) {
    Color sc = Pal(cfg_).shadow;
    const double peak = blurPx_.empty() ? 120 : 70;  // со стеклом тень слабее
    const double soft = 26 * geo_.k;                 // ширина размытого края
    const double outer = ext + soft;
    auto* px = static_cast<uint32_t*>(bits_);
    const double cx = center_.x, cy = center_.y;
    auto pack = [&](double a) {
        uint32_t ai = (uint32_t)(a + 0.5);
        return (ai << 24) | ((sc.GetR() * ai / 255) << 16) | ((sc.GetG() * ai / 255) << 8) | (sc.GetB() * ai / 255);
    };
    // внутри тени прозрачность постоянна, корень нужен только в полосе мягкого края
    const uint32_t full = pack(peak);
    BaseKey key{ext, catcherR, peak, center_.x, center_.y, geo_.w, geo_.h, full};
    baseNow_ = key;
    const size_t count = (size_t)geo_.w * geo_.h;
    if (key == baseCached_ && baseCache_.size() == count) {
        memcpy(px, baseCache_.data(), count * 4);
        return;
    }
    // кэш заводим, только когда размер устоялся (второй кадр подряд с тем же ключом): во время раскрытия
    // каждый кадр новый, и копия была бы лишней работой
    const bool keep = key == baseLast_;
    baseLast_ = key;
    const double ext2 = ext * ext, outer2 = outer * outer;
    for (int y = 0; y < geo_.h; ++y) {
        uint32_t* row = px + (size_t)y * geo_.w;
        double dy = y + 0.5 - cy;
        if (std::fabs(dy) >= outer) {
            memset(row, 0, (size_t)geo_.w * 4);
            continue;
        }
        double dy2 = dy * dy;
        for (int x = 0; x < geo_.w; ++x) {
            double dx = x + 0.5 - cx;
            double d2 = dx * dx + dy2;
            if (d2 >= outer2) {
                row[x] = 0;
                continue;
            }
            if (d2 <= ext2) {
                row[x] = full;
                continue;
            }
            double d = std::sqrt(d2);
            double a = peak * (outer - d) / soft;
            if (d <= catcherR && a < 1) a = 1;
            row[x] = pack(a);
        }
    }
    if (keep) {
        baseCache_.assign(px, px + count);
        baseCached_ = key;
    }
}

void RadialMenu::Render() {
    PERF_SCOPE("menu.Render");
    ++frames_;
    ULONGLONG now = GetTickCount64();
    double openT = (now - tOpen_) / fx::Dur(kOpenMs);
    double openE = EaseOut(openT);
    double closeT = closing_ ? Clamp01((now - tClose_) / CloseMs()) : 0;
    // при запуске меню держится и гаснет к концу волны, при простом закрытии - сразу
    double alpha = closing_ ? (launching_ ? 1 - closeT * closeT : 1 - closeT) : openE;

    const double cx = center_.x, cy = center_.y;
    // кольцо раскрывается пружиной и чуть сжимается, когда гаснет
    double grow = (0.80 + 0.20 * EaseOutBack(openT)) * (closing_ && !launching_ ? 1 - 0.05 * EaseOut(closeT) : 1);
    bool hasSub = mode_ != Mode::None && !subItems_.empty();
    double subT = (now - tSub_) / fx::Dur(kSubMs);
    double subE = mode_ != Mode::None ? EaseOut(subT) : 0;
    struct {
        bool on = false;
        Ring ring{};
        double a0 = 0, a1 = 0;
    } ripple;  // сектор, который запустили
    double ext = (mode_ != Mode::None ? geo_.r2 + (geo_.r4 - geo_.r2) * subE : geo_.r2) * grow;
    // у целого кольца второй ярус - узкий веер, а не полукруг у края: тень только под первым кольцом,
    // иначе вокруг висит большой затемнённый диск; подложка для кликов - на всю ширину
    {
        PERF_SCOPE("menu.r.Base");
        PaintBase(full_ ? geo_.r2 * grow : ext, ext + 10);  // заменяет и очистку кадра
    }
    {
        POINT cp = CursorPt();
        cursorLocal_ = {cp.x - origin_.x, cp.y - origin_.y};
    }
    Bitmap surface(geo_.w, geo_.h, geo_.w * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
    Graphics g(&surface);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetInterpolationMode(InterpolationModeBilinear);  // значки уже нужного размера, масштаб почти 1:1
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    struct {
        const Item* it = nullptr;
        Ring ring{};
        double a0 = 0, a1 = 0, hl = 0;
    } pill;

    // первое кольцо; во время поиска оно приглушено
    int n = (int)ring_->size();
    double step = 2 * Span() / n;
    Ring root{geo_.r1 * grow, geo_.r2 * grow, 0, 0, geo_.iconRoot, false};
    root.rIcon = root.rIn + (root.rOut - root.rIn) * 0.42;
    root.rLabel = root.rIn + (root.rOut - root.rIn) * 0.84;
    double elapsed = (double)(now - tOpen_);
    // кольцо повёрнуто не на целый сектор: у края полукруга один сектор виден двумя половинками
    bool wrap = !full_ && std::fabs(rootRot_ - std::round(rootRot_)) > 1e-3;
    GraphicsState rootClip = g.Save();
    GraphicsPath pie;
    if (wrap) {
        double big = geo_.r2 * 1.3;
        pie.AddPie((REAL)(cx - big), (REAL)(cy - big), (REAL)(2 * big), (REAL)(2 * big), (REAL)AbsAngle(-Span()),
                   (REAL)((left_ ? 1 : -1) * 2 * Span()));
        g.SetClip(&pie);
    }
    std::vector<Sector> secs;
    for (int i = 0; i < n; ++i) {
        double ap = EaseOut((elapsed - fx::Dur(kStaggerMs) * i) / fx::Dur(170));
        double slot = RootSlot(i);
        double a0 = -Span() + step * slot, a1 = a0 + step;
        bool launched = launching_ && launchSub_ < 0 && i == launchRoot_;
        if (launched) ripple = {true, root, a0, a1};
        double hl = launched ? 1.0 : hlRoot_[i];
        bool active = mode_ == Mode::Root && i == subParent_;
        secs.push_back({root, a0, a1, &(*ring_)[i], hl, ap, active, RootHue(i), i == hoverRoot_});
        if (wrap && slot > n - 1)  // хвост, ушедший за край, выходит с другой стороны
            secs.push_back(
                {root, a0 - 2 * Span(), a1 - 2 * Span(), &(*ring_)[i], hl, ap, active, RootHue(i), i == hoverRoot_});
        if (i == hoverRoot_) pill = {&(*ring_)[i], root, a0, a1, hlRoot_[i]};
    }
    DrawRing(g, secs, wrap ? &pie : nullptr);
    g.Restore(rootClip);

    if (mode_ == Mode::Search) {  // во время поиска первое кольцо уходит в тень
        GraphicsPath band;
        band.AddEllipse((REAL)(cx - root.rOut - 4), (REAL)(cy - root.rOut - 4), (REAL)(2 * root.rOut + 8),
                        (REAL)(2 * root.rOut + 8));
        band.AddEllipse((REAL)(cx - root.rIn), (REAL)(cy - root.rIn), (REAL)(2 * root.rIn), (REAL)(2 * root.rIn));
        SolidBrush shade(WithAlpha(Pal(cfg_).shadow, (BYTE)(150 * subE)));
        g.FillPath(&shade, &band);
    }

    // второе кольцо раскрывается веером от родителя
    if (hasSub) {
        double from = SubFrom(), sstep = SubStep();
        Ring sub{
            geo_.r3 * grow, (geo_.r3 + (geo_.r4 - geo_.r3) * (0.55 + 0.45 * subE)) * grow, 0, 0, geo_.iconSub, true};
        sub.rIcon = sub.rIn + (sub.rOut - sub.rIn) * 0.36;
        sub.rLabel = sub.rIn + (sub.rOut - sub.rIn) * 0.76;
        int hot = HotSub();
        double mid = (subItems_.size() - 1) / 2.0;
        double wFrom = SubWinFrom(), wTo = SubWinTo();
        double hue = mode_ == Mode::Root ? RootHue(subParent_) : 0;
        secs.clear();
        for (size_t i = 0; i < subItems_.size(); ++i) {
            double a0 = from + sstep * i, a1 = a0 + sstep, ap = subE;
            if (a1 <= wFrom || a0 >= wTo) continue;  // прокручено за край окна
            double edgeFade = Clamp01(std::min(a1 - wFrom, wTo - a0) / sstep);
            if (subSlide_) {
                // перелистывание: новая страница въезжает поворотом со стороны листания
                double shift = -subSlide_ * (1 - subE) * sstep * 2.5;
                a0 += shift;
                a1 += shift;
            } else {
                // веер от родителя, секторы проявляются волной от середины к краям
                a0 = subMid_ + (a0 - subMid_) * subE;
                a1 = subMid_ + (a1 - subMid_) * subE;
                ap = EaseOut((now - tSub_ - fx::Dur(kSubWaveMs) * std::fabs(i - mid)) / fx::Dur(kSubMs));
            }
            bool launched = launching_ && (int)i == launchSub_;
            if (launched) ripple = {true, sub, a0, a1};
            secs.push_back(
                {sub, a0, a1, subItems_[i], launched ? 1.0 : hlSub_[i], ap * edgeFade, false, hue, (int)i == hot});
            if ((int)i == hot) pill = {subItems_[i], sub, a0, a1, hlSub_[i]};
        }
        DrawRing(g, secs, nullptr);
        // полоса прокрутки по внешнему краю: сколько видно и где мы
        if (SubMaxScroll() > 0 && subE > 0.5) {
            double rr = sub.rOut + 9 * geo_.k, win = wTo - wFrom, m = (double)subItems_.size();
            double sgn = left_ || full_ ? 1 : -1;
            Palette sp = Pal(cfg_);
            Pen track(WithAlpha(sp.border, (BYTE)(40 * subE)), (REAL)(2 * geo_.k));
            track.SetStartCap(LineCapRound);
            track.SetEndCap(LineCapRound);
            g.DrawArc(&track, (REAL)(cx - rr), (REAL)(cy - rr), (REAL)(2 * rr), (REAL)(2 * rr),
                      (REAL)AbsAngle(wFrom + 2), (REAL)(sgn * (win - 4)));
            double vis = win / sstep, s0 = std::clamp(subScroll_, 0.0, m - vis);
            Pen thumb(WithAlpha(HueShift(sp.rim, hue), (BYTE)(150 * subE)), (REAL)(2.6 * geo_.k));
            thumb.SetStartCap(LineCapRound);
            thumb.SetEndCap(LineCapRound);
            double t0 = wFrom + 2 + (win - 4) * s0 / m, tl = std::max(6.0, (win - 4) * vis / m);
            g.DrawArc(&thumb, (REAL)(cx - rr), (REAL)(cy - rr), (REAL)(2 * rr), (REAL)(2 * rr), (REAL)AbsAngle(t0),
                      (REAL)(sgn * tl));
        }
    }

    // волна от запущенного сектора: его контур уходит наружу и тает
    if (ripple.on) {
        double sgn = left_ || full_ ? 1 : -1;
        for (int wave = 0; wave < 2; ++wave) {
            double t = Clamp01(closeT * 1.15 - wave * 0.22);
            if (t <= 0) continue;
            double e = EaseOut(t);
            double rr = ripple.ring.rOut + (6 + 44 * e) * geo_.k;
            Pen pen(WithAlpha(Pal(cfg_).rim, (BYTE)(230 * (1 - t) * (wave ? 0.55 : 1))),
                    (REAL)((3.4 - 2.4 * e) * geo_.k));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            double pad = (ripple.a1 - ripple.a0) * 0.12 * e;  // волна шире сектора
            g.DrawArc(&pen, (REAL)(cx - rr), (REAL)(cy - rr), (REAL)(2 * rr), (REAL)(2 * rr),
                      (REAL)AbsAngle(ripple.a0 - pad), (REAL)(sgn * (ripple.a1 - ripple.a0 + 2 * pad)));
        }
    }

    // полное имя наведённого сектора, если подпись не влезла
    if (winMode_ && pill.it && pill.hl > 0.05) {
        Font font(Family(), geo_.font, FontStyleRegular, UnitPixel);
        RectF tb;
        g.MeasureString(pill.it->name.c_str(), -1, &font, PointF(0, 0), &tb);
        double arc = pill.ring.rLabel * Rad(pill.a1 - pill.a0) - 4;
        if (tb.Width > arc) {
            double mid = Rad(AbsAngle((pill.a0 + pill.a1) / 2));
            double lx = cx + std::cos(mid) * pill.ring.rLabel, ly = cy + std::sin(mid) * pill.ring.rLabel;
            double pw = tb.Width + 16 * geo_.k, ph = geo_.font * 1.9;
            double px = std::clamp(lx - pw / 2, 4.0, geo_.w - pw - 4), py = ly - ph / 2;
            double rad = ph / 2;
            GraphicsPath pp;
            pp.AddArc((REAL)px, (REAL)py, (REAL)(2 * rad), (REAL)ph, 90, 180);
            pp.AddArc((REAL)(px + pw - 2 * rad), (REAL)py, (REAL)(2 * rad), (REAL)ph, 270, 180);
            pp.CloseFigure();
            BYTE a = (BYTE)(245 * pill.hl);
            SolidBrush bg(WithAlpha(Pal(cfg_).pillBg, a));
            g.FillPath(&bg, &pp);
            Pen edge(WithAlpha(Pal(cfg_).pillEdge, a), 1.2f);
            g.DrawPath(&edge, &pp);
            StringFormat sf(StringFormatFlagsNoWrap);
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush txt(WithAlpha(Pal(cfg_).text, a));
            g.DrawString(pill.it->name.c_str(), -1, &font, RectF((REAL)px, (REAL)py, (REAL)pw, (REAL)ph), &sf, &txt);
        }
    }

    // плашки: путь внутри папок сверху, поиск и подсказки снизу
    {
        double ph = geo_.font * 2.0;
        bool card = !winMode_ && HoveredItem() && GetTickCount64() - keyT_ > 120;
        std::wstring top = card ? L"" : TopHint();
        DrawPill(g, top, ExeDir() + L"\\icons\\back.png", cy - geo_.r4 * grow - ph - 18 * geo_.k, openE, false);
        // кольцо у верхнего края (например, из угла): над ним карточке места нет - она встаёт вниз,
        // на место подсказки
        bool below = false;
        if (card) {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
            below = origin_.y + cy - geo_.r4 * grow - 90 * geo_.k < mi.rcMonitor.top;
            PERF_SCOPE("menu.r.Card");
            DrawInfoCard(g, grow, openE, below);
        }
        double ha = 1;
        std::wstring bottom = below ? L"" : BottomHint(ha);
        bool search = mode_ == Mode::Search;
        DrawPill(g, bottom, search ? ExeDir() + L"\\icons\\search.png" : L"", cy + geo_.r4 * grow + 18 * geo_.k,
                 openE * ha, search || dragging_ || ha >= 1);
    }

    // ступица у края экрана и световой шов
    Palette pal = Pal(cfg_);
    {
        double rh = geo_.rHub * grow;
        SolidBrush hub(WithAlpha(pal.pillBg, 240));
        g.FillEllipse(&hub, (REAL)(cx - rh), (REAL)(cy - rh), (REAL)(2 * rh), (REAL)(2 * rh));
        Pen ring(WithAlpha(pal.hubRing, 220), (REAL)(2 * geo_.k));
        g.DrawEllipse(&ring, (REAL)(cx - rh), (REAL)(cy - rh), (REAL)(2 * rh), (REAL)(2 * rh));
        // в кольце окон середину занимает миниатюра; без наведения - эмблема и число окон
        RECT tr;
        bool thumb = winMode_ && ThumbRect(grow, tr);
        if (thumb) {
            double pad = 5 * geo_.k;
            RectF fr((REAL)(tr.left - pad), (REAL)(tr.top - pad), (REAL)(tr.right - tr.left + 2 * pad),
                     (REAL)(tr.bottom - tr.top + 2 * pad));
            GraphicsPath fp;
            RoundRectPath(fp, fr, (float)(8 * geo_.k));
            SolidBrush fb(WithAlpha(pal.shadow, 200));
            g.FillPath(&fb, &fp);
            Pen fe(WithAlpha(pal.pillEdge, 230), (REAL)(1.4 * geo_.k));
            g.DrawPath(&fe, &fp);
        } else if (winMode_) {
            int total = 0;
            for (const Item& r : *ring_) total += r.children.empty() ? 1 : (int)r.children.size();
            Font font(Family(), geo_.font * 1.15f, FontStyleRegular, UnitPixel);
            StringFormat sf(StringFormatFlagsNoWrap);
            sf.SetAlignment(StringAlignmentCenter);
            SolidBrush txt(WithAlpha(pal.text, 200));
            std::wstring label = Plural(total, L"окно", L"окна", L"окон");
            g.DrawString(label.c_str(), -1, &font, PointF((REAL)cx, (REAL)(cy + 30 * geo_.k * grow)), &sf, &txt);
        }
        double er = winMode_ ? 30 * geo_.k * grow : rh;
        Bitmap* emblem = thumb ? nullptr : icons_.Get(ExeDir() + L"\\icons\\hub.png", L"", (int)(er * 1.5));
        if (thumb) {
            // эмблему закрывает миниатюра
        } else if (emblem) {
            double s = er * 1.5, ex = full_ ? cx : cx + (left_ ? rh * 0.42 : -rh * 0.42);
            g.DrawImage(emblem, RectF((REAL)(ex - s / 2), (REAL)(cy - s / 2), (REAL)s, (REAL)s));
        } else {
            double rd = rh * 0.22;
            SolidBrush dot(pal.dot);
            double dx = full_ ? cx : cx + (left_ ? rh * 0.45 : -rh * 0.45);
            g.FillEllipse(&dot, (REAL)(dx - rd), (REAL)(cy - rd), (REAL)(2 * rd), (REAL)(2 * rd));
        }

        double len = geo_.r2 * 1.15 * grow, wseam = full_ ? 0 : 3 * geo_.k;
        RectF seam((REAL)(left_ ? 0 : geo_.w - wseam), (REAL)(cy - len), (REAL)wseam, (REAL)(2 * len));
        LinearGradientBrush lg(PointF(0, (REAL)(cy - len)), PointF(0, (REAL)(cy + len)), Color(0, 0, 0, 0),
                               Color(0, 0, 0, 0));
        Color cols[] = {WithAlpha(pal.seam, 0), WithAlpha(pal.rim, 230), WithAlpha(pal.seam, 0)};
        REAL pos[] = {0, 0.5f, 1};
        lg.SetInterpolationColors(cols, pos, 3);
        g.FillRectangle(&lg, seam);
    }

    PERF_SCOPE("menu.r.Present");
    BYTE a = (BYTE)(255 * Clamp01(alpha));
    HDC screen = GetDC(nullptr);
    POINT src{0, 0};
    SIZE size{geo_.w, geo_.h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, a, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &origin_, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    if (winMode_) UpdateThumb(grow, alpha);
}

// Узор сотового стекла: тонкие светлые грани и чуть разная яркость сот. Строится один раз на размер окна.
void RadialMenu::EnsureHexGlass() {
    hexOn_ = cfg_->hexGlass && !fx::Game();
    if (!hexOn_ || (!hexPx_.empty() && hexW_ == geo_.w && hexH_ == geo_.h)) return;
    PERF_SCOPE("menu.HexGlass");
    hexPx_.assign((size_t)geo_.w * geo_.h, 0);
    {
        Bitmap bmp(geo_.w, geo_.h, geo_.w * 4, PixelFormat32bppPARGB, (BYTE*)hexPx_.data());
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        double s = 13 * geo_.k;
        Pen line(Color(30, 255, 255, 255), 1.0f);
        SolidBrush facet(Color(0, 0, 0, 0));
        hex::ForEach(0, 0, geo_.w, geo_.h, s, [&](const hex::Cell& c) {
            GraphicsPath p;
            hex::Path(p, c.x, c.y, s, 0.94);
            uint32_t h = (uint32_t)(c.q * 73856093) ^ (uint32_t)(c.r * 19349663);
            h ^= h >> 13;
            facet.SetColor(Color((BYTE)(h % 9), 255, 255, 255));  // каждая сота чуть своей яркости
            g.FillPath(&facet, &p);
            g.DrawPath(&line, &p);
        });
    }
    hexW_ = geo_.w;
    hexH_ = geo_.h;
}

// Сота под курсором приподнята: светлее, с яркой кромкой, соседи чуть подсвечены.
void RadialMenu::DrawHexLift(Graphics& g, GraphicsPath& sector, double hl) {
    double s = 13 * geo_.k;
    hex::Cell c = hex::At(cursorLocal_.x, cursorLocal_.y, s);
    Palette pal = Pal(cfg_);
    GraphicsState st = g.Save();
    g.SetClip(&sector, CombineModeIntersect);
    for (int i = 0; i < 6; ++i) {  // соседи
        static const int dq[6] = {1, 1, 0, -1, -1, 0}, dr[6] = {0, -1, -1, 0, 1, 1};
        hex::Cell n = hex::Center(c.q + dq[i], c.r + dr[i], s);
        GraphicsPath p;
        hex::Path(p, n.x, n.y, s, 0.94);
        Pen e(WithAlpha(pal.rim, (BYTE)(70 * hl)), 1.0f);
        g.DrawPath(&e, &p);
    }
    GraphicsPath p;
    hex::Path(p, c.x, c.y - 1.5 * geo_.k * hl, s, 0.94 + 0.06 * hl);  // чуть больше и выше
    SolidBrush f(WithAlpha(pal.rim, (BYTE)(80 * hl)));
    g.FillPath(&f, &p);
    Pen e(WithAlpha(pal.rim, (BYTE)(245 * hl)), (REAL)(1.8 * geo_.k));
    g.DrawPath(&e, &p);
    g.Restore(st);
}

// Прямоугольник миниатюры наведённого окна, вписанный в ступицу с сохранением пропорций окна.
bool RadialMenu::ThumbRect(double grow, RECT& rc) {
    const Item* it = closing_ ? nullptr : HoveredItem();
    HWND w = it && it->special == 2 ? it->win : nullptr;
    if (w != thumbWin_) {
        DropThumb();
        thumbWin_ = w;
        if (w && FAILED(DwmRegisterThumbnail(hwnd_, w, &thumb_))) thumb_ = nullptr;
    }
    if (!thumb_) return false;
    SIZE src{};
    if (FAILED(DwmQueryThumbnailSourceSize(thumb_, &src)) || src.cx <= 0 || src.cy <= 0) return false;
    double r = geo_.rHub * grow - 12 * geo_.k;
    double a = (double)src.cx / src.cy;
    double h = 2 * r / std::sqrt(a * a + 1), wd = a * h;
    rc = {(LONG)(center_.x - wd / 2), (LONG)(center_.y - h / 2), (LONG)(center_.x + wd / 2), (LONG)(center_.y + h / 2)};
    return true;
}

void RadialMenu::UpdateThumb(double grow, double alpha) {
    RECT rc;
    if (!ThumbRect(grow, rc)) return;
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcDestination = rc;
    p.fVisible = TRUE;
    p.opacity = (BYTE)(255 * Clamp01(alpha));
    p.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(thumb_, &p);
}

void RadialMenu::DropThumb() {
    if (thumb_) DwmUnregisterThumbnail(thumb_);
    thumb_ = nullptr;
    thumbWin_ = nullptr;
}
