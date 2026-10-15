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

// Снимок того, что под меню, уменьшенный в 4 раза, размытый и растянутый обратно: из него делается «стекло».
// Всё на своих простых циклах: StretchBlt с экрана и растягивание GDI+ стоили по 10 мс каждое.
void RadialMenu::CaptureBackdrop() {
    PERF_SCOPE("menu.CaptureBackdrop");
    blurBrush_.reset();
    blurBmp_.reset();
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

    blurBmp_ = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    BitmapData bd;
    Rect r(0, 0, w, h);
    if (blurBmp_->LockBits(&r, ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Ok) {
        UpscaleBilinear(small.data(), sw, sh, (uint32_t*)bd.Scan0, w, h, bd.Stride / 4);
        blurBmp_->UnlockBits(&bd);
        blurBrush_ = std::make_unique<TextureBrush>(blurBmp_.get(), WrapModeClamp);
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void RadialMenu::DrawSector(Graphics& g, const Ring& r, double a0, double a1, const Item& it, double hl, double appear,
                            bool active, double hue, bool hot) {
    PERF_SCOPE("menu.r.Sector");
    const double cx = center_.x, cy = center_.y;
    const double gap = 3.0 * geo_.k;
    const double rOut = r.rOut + 3.5 * geo_.k * hl;  // наведённый сектор чуть выдвигается наружу
    double inO = gap / 2 / rOut * 180 / kPi, inI = gap / 2 / r.rIn * 180 / kPi;
    double o0 = a0 + inO, o1 = a1 - inO, i0 = a0 + inI, i1 = a1 - inI;
    if (o1 <= o0 || i1 <= i0) return;

    double sgn = left_ || full_ ? 1 : -1;
    GraphicsPath p;
    p.AddArc((REAL)(cx - rOut), (REAL)(cy - rOut), (REAL)(2 * rOut), (REAL)(2 * rOut), (REAL)AbsAngle(o0),
             (REAL)(sgn * (o1 - o0)));
    p.AddArc((REAL)(cx - r.rIn), (REAL)(cy - r.rIn), (REAL)(2 * r.rIn), (REAL)(2 * r.rIn), (REAL)AbsAngle(i1),
             (REAL)(-sgn * (i1 - i0)));
    p.CloseFigure();

    Palette pal = Pal(cfg_);
    if (hue != 0) {  // у каждой группы свой оттенок
        pal.hotIn = HueShift(pal.hotIn, hue);
        pal.hotOut = HueShift(pal.hotOut, hue);
        pal.active = HueShift(pal.active, hue);
        pal.rim = HueShift(pal.rim, hue);
    }
    Color base = r.sub ? pal.baseSub : pal.base;
    bool glass = blurBrush_ && appear > 0.5;
    if (glass) g.FillPath(blurBrush_.get(), &p);  // размытая подложка: обои просвечивают сквозь кольцо
    BYTE alpha = (BYTE)((glass ? 178 : 232) * appear);
    // наведённый сектор переливается от центра к краю: малина -> коралл (в тёмной теме - один акцент)
    double t = std::max(hl, active ? 0.32 : 0.0);
    BYTE fillAlpha = std::max<BYTE>(alpha, (BYTE)(232 * appear * t));
    Color from = hl >= 0.32 || !active ? pal.hotIn : pal.active, to = hl >= 0.32 || !active ? pal.hotOut : pal.active;
    double mr = Rad(AbsAngle((a0 + a1) / 2));
    PointF pin((REAL)(cx + std::cos(mr) * r.rIn), (REAL)(cy + std::sin(mr) * r.rIn));
    PointF pout((REAL)(cx + std::cos(mr) * rOut), (REAL)(cy + std::sin(mr) * rOut));
    LinearGradientBrush fill(pin, pout, MixColor(base, from, t * 0.92, fillAlpha),
                             MixColor(base, to, t * 0.92, fillAlpha));
    fill.SetWrapMode(WrapModeTileFlipXY);
    g.FillPath(&fill, &p);
    if (hexOn_ && appear > 0.5) {
        g.FillPath(hexBrush_.get(), &p);  // грани сотового стекла
        if (hl > 0.02) DrawHexLift(g, p, hl);
    }
    Pen border(WithAlpha(pal.border, (BYTE)((glass ? 46 : 34) * appear)), 1.0f);
    g.DrawPath(&border, &p);
    {  // тонкая полоса оттенка группы у внутреннего края: видно, чьё второе кольцо
        Pen band(WithAlpha(pal.rim, (BYTE)((70 + 90 * std::max(hl, active ? 1.0 : 0.0)) * appear)),
                 (REAL)(2.2 * geo_.k));
        double rb = r.rIn + 2.4 * geo_.k;
        g.DrawArc(&band, (REAL)(cx - rb), (REAL)(cy - rb), (REAL)(2 * rb), (REAL)(2 * rb), (REAL)AbsAngle(i0),
                  (REAL)(sgn * (i1 - i0)));
    }

    if (hl > 0.02) {  // светящаяся кромка снаружи
        Pen rim(WithAlpha(pal.rim, (BYTE)(230 * hl * appear)), (REAL)gap);
        double rr = rOut + gap * 1.4;
        g.DrawArc(&rim, (REAL)(cx - rr), (REAL)(cy - rr), (REAL)(2 * rr), (REAL)(2 * rr), (REAL)AbsAngle(o0),
                  (REAL)(sgn * (o1 - o0)));
    }

    double mid = Rad(AbsAngle((a0 + a1) / 2));
    if (Bitmap* icon = ItemIcon(it, r.iconPx)) {
        double sz = r.iconPx * (0.7 + 0.3 * appear) * (1 + 0.1 * hl);
        double ix = cx + std::cos(mid) * r.rIcon - sz / 2, iy = cy + std::sin(mid) * r.rIcon - sz / 2;
        DrawIcon(g, icon, it, ix, iy, sz, hl, appear, hot);
    }
    DrawLabel(g, r, a0, a1, it, hl, appear);
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
    if (mode_ != Mode::Root || subParent_ < 0 || navStack_.empty()) return {};
    std::wstring s = (*ring_)[subParent_].name;
    for (const Item& f : navStack_) s += L" / " + f.name;
    return s;
}

std::wstring RadialMenu::BottomHint(double& alpha) const {
    alpha = 1;
    if (mode_ == Mode::Search) {
        std::wstring s = query_ + L"_";
        if (subItems_.empty()) s += L"   ничего не найдено";
        return s;
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
        return L"Ещё раз - открыть";
    if (it && ToDialog(*it)) return it->dir ? L"Перейти сюда в диалоге" : L"Выбрать в диалоге";
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
    const double peak = blurBrush_ ? 70 : 120;  // со стеклом тень слабее
    const double soft = 26 * geo_.k;            // ширина размытого края
    const double outer = ext + soft;
    auto* px = static_cast<uint32_t*>(bits_);
    const double cx = center_.x, cy = center_.y;
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
            if (d2 >= outer * outer) {
                row[x] = 0;
                continue;
            }
            double d = std::sqrt(d2);
            double a = d <= ext ? peak : peak * (outer - d) / soft;
            if (d <= catcherR && a < 1) a = 1;
            uint32_t ai = (uint32_t)(a + 0.5);
            row[x] =
                (ai << 24) | ((sc.GetR() * ai / 255) << 16) | ((sc.GetG() * ai / 255) << 8) | (sc.GetB() * ai / 255);
        }
    }
}

void RadialMenu::Render() {
    PERF_SCOPE("menu.Render");
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
        POINT cp;
        GetCursorPos(&cp);
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
    if (wrap) {
        GraphicsPath pie;
        double big = geo_.r2 * 1.3;
        pie.AddPie((REAL)(cx - big), (REAL)(cy - big), (REAL)(2 * big), (REAL)(2 * big), (REAL)AbsAngle(-Span()),
                   (REAL)((left_ ? 1 : -1) * 2 * Span()));
        g.SetClip(&pie);
    }
    for (int i = 0; i < n; ++i) {
        double ap = EaseOut((elapsed - fx::Dur(kStaggerMs) * i) / fx::Dur(170));
        double slot = RootSlot(i);
        double a0 = -Span() + step * slot, a1 = a0 + step;
        bool launched = launching_ && launchSub_ < 0 && i == launchRoot_;
        if (launched) ripple = {true, root, a0, a1};
        double hl = launched ? 1.0 : hlRoot_[i];
        bool active = mode_ == Mode::Root && i == subParent_;
        DrawSector(g, root, a0, a1, (*ring_)[i], hl, ap, active, RootHue(i), i == hoverRoot_);
        if (wrap && slot > n - 1)  // хвост, ушедший за край, выходит с другой стороны
            DrawSector(g, root, a0 - 2 * Span(), a1 - 2 * Span(), (*ring_)[i], hl, ap, active, RootHue(i),
                       i == hoverRoot_);
        if (i == hoverRoot_) pill = {&(*ring_)[i], root, a0, a1, hlRoot_[i]};
    }
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
            DrawSector(g, sub, a0, a1, *subItems_[i], launched ? 1.0 : hlSub_[i], ap * edgeFade, false, hue,
                       (int)i == hot);
            if ((int)i == hot) pill = {subItems_[i], sub, a0, a1, hlSub_[i]};
        }
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
        if (card) {
            PERF_SCOPE("menu.r.Card");
            DrawInfoCard(g, grow, openE);
        }
        double ha = 1;
        std::wstring bottom = BottomHint(ha);
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
    if (!hexOn_ || (hexBmp_ && hexW_ == geo_.w && hexH_ == geo_.h)) return;
    PERF_SCOPE("menu.HexGlass");
    hexBrush_.reset();
    hexBmp_ = std::make_unique<Bitmap>(geo_.w, geo_.h, PixelFormat32bppPARGB);
    {
        Graphics g(hexBmp_.get());
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
    hexBrush_ = std::make_unique<TextureBrush>(hexBmp_.get(), WrapModeClamp);
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
