// Отрисовка панели «Сейчас играет»: неподвижный слой (карточка, текст, кнопки) и верхний (яблоко, рычаг, столбики).
#include "media/widget.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "core/night.h"
#include "media/widget_internal.h"
#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace widget_detail {

std::wstring Clock(int64_t t) {
    int s = (int)(std::max<int64_t>(0, t) / 10000000);
    wchar_t b[16];
    if (s >= 3600)
        swprintf(b, 16, L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        swprintf(b, 16, L"%d:%02d", s / 60, s % 60);
    return b;
}

void RoundRect(GraphicsPath& p, const RectF& r, float rad) {
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void Triangle(Graphics& g, Brush* b, float x, float y, float s, bool right) {
    PointF p[3];
    if (right) {
        p[0] = PointF(x, y);
        p[1] = PointF(x + s * 0.87f, y + s / 2);
        p[2] = PointF(x, y + s);
    } else {
        p[0] = PointF(x + s * 0.87f, y);
        p[1] = PointF(x, y + s / 2);
        p[2] = PointF(x + s * 0.87f, y + s);
    }
    g.FillPolygon(b, p, 3);
}

StringFormat* LineFormat(StringAlignment h) {
    // живут до конца программы: деструкторы после GdiplusShutdown упали бы
    static StringFormat* f = new StringFormat[3];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 3; ++i) {
            f[i].SetFormatFlags(StringFormatFlagsNoWrap);
            f[i].SetTrimming(StringTrimmingEllipsisCharacter);
            f[i].SetLineAlignment(StringAlignmentCenter);
            f[i].SetAlignment((StringAlignment)i);
        }
        init = true;
    }
    return &f[h];
}

}  // namespace widget_detail

using namespace widget_detail;

namespace {
// Плотность вуали над размытой обложкой: на светлой обложке текст должен оставаться читаемым
constexpr BYTE kVeil = 200;
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
}  // namespace

void MediaWidget::FreeFrame() {
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
        dib_ = nullptr;
        bits_ = nullptr;
    }
    std::vector<uint32_t>().swap(layer_);
    dibW_ = dibH_ = 0;
    layerDirty_ = true;
}

void MediaWidget::Render() {
    SIZE sz = WindowSize();
    int W = sz.cx, H = sz.cy;
    HDC scr = GetDC(nullptr);
    if (!memDC_ || W != dibW_ || H != dibH_) {
        FreeFrame();
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
        memDC_ = CreateCompatibleDC(scr);
        dib_ = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        oldBmp_ = SelectObject(memDC_, dib_);
        dibW_ = W, dibH_ = H;
        layer_.resize((size_t)W * H);
    }
    auto setup = [](Graphics& g) {
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    };
    bool rebuilt = layerDirty_;
    if (layerDirty_) {
        layerDirty_ = false;
        std::fill(layer_.begin(), layer_.end(), 0);
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)layer_.data());
        Graphics g(&bmp);
        setup(g);
        DrawStatic(g);
    }
    memcpy(bits_, layer_.data(), layer_.size() * 4);
    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
        Graphics g(&bmp);
        setup(g);
        DrawDynamic(g);
    }
    // для разработки: RADIAL_SNAP=1 - каждый кадр панели пишется в media_snap.png рядом с exe
    if (GetEnvironmentVariableW(L"RADIAL_SNAP", nullptr, 0)) {
        Bitmap snap(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
        const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
        snap.Save((ExeDir() + L"\\media_snap.png").c_str(), &png, nullptr);
    }
    // спрятанная карточка стоит верхним краем большой карточки на нижнем краю экрана
    float top = (kM + kLyr) * (float)k_;
    LONG hidden = mon_.bottom - (LONG)top;
    POINT at{pos_.x, (LONG)(hidden + (pos_.y - hidden) * slide_)};
    POINT src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(255 * std::clamp(slide_ * 1.4f, 0.0f, 1.0f)), AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &at, &sz, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    // поверх всех: только при перерисовке слоя, а не в каждом кадре вращения
    if (rebuilt) SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void MediaWidget::DrawStatic(Graphics& g) {
    float k = (float)k_;
    RectF body = Body();
    float ex = (float)EaseOut(expand_), mo = std::clamp(morph_, 0.0f, 1.0f);
    Color bg = ToColor(theme_.bg), ac = Ac();

    // тень: готовая картинка для ближайшего размера, растянутая под текущий
    Bitmap* sh = mo < 0.5f ? shadowMini_.get() : ex > 0.5f ? shadowTall_.get() : shadowFull_.get();
    if (sh)
        g.DrawImage(sh, RectF(body.X - kPad * k, body.Y - kPad * k + 8 * k, body.Width + kPad * k * 2,
                              body.Height + kPad * k * 2));
    GraphicsPath path;
    RoundRect(path, body, Lerp(18, kRadius, mo) * k);
    GraphicsState st = g.Save();
    g.SetClip(&path);
    // фон: размытая обложка под вуалью цвета темы и отсвет цвета обложки за пластинкой
    if (artBg_) {
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.DrawImage(artBg_.get(), Inset(body, -30 * k, -30 * k));
        SolidBrush veil(WithAlpha(bg, (BYTE)(std::min(240.0f, kVeil + 30 * ex + (night::On() ? 30 : 0)))));
        g.FillRectangle(&veil, body);
    } else {
        SolidBrush b(WithAlpha(bg, 245));
        g.FillRectangle(&b, body);
    }
    RectF rc = RecordRect();
    {
        float gr = rc.Width * 1.17f;
        GraphicsPath gp;
        gp.AddEllipse(rc.X + rc.Width / 2 - gr, rc.Y + rc.Height / 2 - gr, gr * 2, gr * 2);
        PathGradientBrush pg(&gp);
        pg.SetCenterColor(WithAlpha(ac, (BYTE)(78 * glow_)));
        Color edge(0, ac.GetR(), ac.GetG(), ac.GetB());
        int n = 1;
        pg.SetSurroundColors(&edge, &n);
        g.FillPath(&pg, &gp);
    }
    // с запасом в пиксель: иначе градиент на последней строке заворачивается и видна черта
    LinearGradientBrush shine(PointF(0, body.Y - 1), PointF(0, body.Y + 100 * k + 1), Color(26, 255, 255, 255),
                              Color(0, 255, 255, 255));
    g.FillRectangle(&shine, RectF(body.X, body.Y, body.Width, 100 * k));
    if (ex > 0.01f) {
        // текст песни над основной частью и тонкая линия-разделитель
        float by = (kM + kLyr) * k;
        DrawLyrics(g, Rect(H_LYRICS), ex);
        SolidBrush line(Color((BYTE)(30 * ex), 255, 255, 255));
        g.FillRectangle(&line, RectF(body.X + 24 * k, by + 8 * k, body.Width - 48 * k, 1 * k));
    }
    if (track_.end > 0 && mo < 0.5f) {  // в маленькой карточке время - полоска по нижнему краю
        float t = (float)std::clamp((double)track_.Now() / track_.end, 0.0, 1.0);
        SolidBrush pb(WithAlpha(ac, (BYTE)(255 * (1 - mo * 2))));
        g.FillRectangle(&pb, RectF(body.X, body.GetBottom() - 3 * k, body.Width * t, 3 * k));
    }
    g.Restore(st);
    Pen rim(Color(40, 255, 255, 255), 1 * k);
    g.DrawPath(&rim, &path);

    // содержимое сменяется посередине роста; большая часть режется по контуру, пока не выросла
    float fa = (float)Clamp01((mo - 0.45) / 0.45), ma = 1 - (float)Clamp01(mo / 0.45);
    if (fa > 0) {
        GraphicsState cs = g.Save();
        if (mo < 0.999f) g.SetClip(&path);
        DrawFull(g, fa);
        g.Restore(cs);
    }
    if (ma > 0) DrawMini(g, ma);
    discBaked_ = rx_ == 0 && rxT_ == 0;
    if (discBaked_) DrawDisc(g, rc);
}

void MediaWidget::DrawDisc(Graphics& g, const RectF& rc) {
    if (!disc_) return;
    float sc = rc.Width / (kRec * (float)k_), pad = 12 * (float)k_ * sc;
    float size = disc_->GetWidth() * sc;
    g.SetInterpolationMode(sc > 0.99f && sc < 1.01f ? InterpolationModeBilinear : InterpolationModeHighQualityBilinear);
    g.DrawImage(disc_.get(), RectF(rc.X - pad, rc.Y - pad, size, size));
}

void MediaWidget::DrawLabel(Graphics& g, const RectF& rc) {
    float cx = rc.X + rc.Width / 2, cy = rc.Y + rc.Height / 2, ld = rc.Width * 0.46f;
    if (label_) {
        GraphicsState st = g.Save();
        g.SetInterpolationMode(InterpolationModeBilinear);
        g.TranslateTransform(cx, cy);
        g.RotateTransform(ang_);
        g.DrawImage(label_.get(), RectF(-ld / 2, -ld / 2, ld, ld));
        g.Restore(st);
    }
    Pen lr(Color(70, 0, 0, 0), std::max(1.0f, rc.Width * 0.008f));
    g.DrawEllipse(&lr, cx - ld / 2, cy - ld / 2, ld, ld);
    float hr = rc.Width * 0.035f;
    SolidBrush hole(Color(255, 16, 13, 18));
    g.FillEllipse(&hole, cx - hr, cy - hr, hr * 2, hr * 2);
    Pen hp(Color(70, 255, 255, 255), 1);
    g.DrawEllipse(&hp, cx - hr, cy - hr, hr * 2, hr * 2);
}

// То, что движется каждый кадр. Пластинка, пока стоит на месте, лежит в неподвижном слое.
void MediaWidget::DrawDynamic(Graphics& g) {
    float k = (float)k_, mo = std::clamp(morph_, 0.0f, 1.0f);
    Color ac = Ac();
    RectF rc = RecordRect();
    float dx = rx_ * k * Lerp(0.6f, 1, mo);
    rc.X += dx;
    float alpha = (float)Clamp01(1 + rx_ / kRoll * 1.05);
    // кольцо бьётся в такт, на паузе просто тлеет
    float cx = rc.X + rc.Width / 2, cy = rc.Y + rc.Height / 2, R = rc.Width / 2 + 7 * k * Lerp(0.4f, 1, mo);
    bool playing = track_.status == media::kPlaying;
    double beat = fmod(GetTickCount64() / 940.0, 1.0);
    float haloR = playing ? R * (float)(0.98 + 0.10 * beat) : R;
    BYTE haloA = (BYTE)((playing ? 140 * (1 - beat) : 50) * alpha);
    Pen halo(WithAlpha(ac, haloA), 2 * k);
    g.DrawEllipse(&halo, cx - haloR, cy - haloR, haloR * 2, haloR * 2);
    bool baked = rx_ == 0 && rxT_ == 0 && discBaked_;
    RectF wide = Grow(rc, 24 * k);
    DrawFaded(g, wide, alpha, [&](Graphics& gr) {
        if (!baked) DrawDisc(gr, rc);
        DrawLabel(gr, rc);
    });

    // рычаг: шарнир стоит, головка идёт к центру вместе с песней
    float armA = (float)Clamp01(mo * 2 - 1);
    if (armA > 0) {
        float px = (kM + kRollL + 148) * k, py = (kM + kLyr + 20) * k;
        BYTE a = (BYTE)(255 * armA);
        GraphicsState st = g.Save();
        g.TranslateTransform(px, py);
        g.RotateTransform(arm_);
        Pen shadow(Color((BYTE)(90 * armA), 0, 0, 0), 4 * k);
        shadow.SetLineJoin(LineJoinRound);
        shadow.SetStartCap(LineCapRound);
        shadow.SetEndCap(LineCapRound);
        PointF pts[3] = {PointF(0, 0), PointF(-8 * k, 50 * k), PointF(-30 * k, 78 * k)};
        PointF low[3] = {PointF(2 * k, 3 * k), PointF(-6 * k, 53 * k), PointF(-28 * k, 81 * k)};
        g.DrawLines(&shadow, low, 3);
        Pen tube(Color(a, 217, 210, 214), 3 * k);
        tube.SetLineJoin(LineJoinRound);
        tube.SetStartCap(LineCapRound);
        tube.SetEndCap(LineCapRound);
        g.DrawLines(&tube, pts, 3);
        g.TranslateTransform(-34 * k, 78 * k);
        g.RotateTransform(-38);
        GraphicsPath head;
        RoundRect(head, RectF(-7.5f * k, -4.5f * k, 15 * k, 9 * k), 2 * k);
        SolidBrush hb(Color(a, 241, 236, 238));
        g.FillPath(&hb, &head);
        g.Restore(st);
        float pr = 9 * k;
        SolidBrush base(Color(a, 58, 51, 64));
        g.FillEllipse(&base, px - pr, py - pr, pr * 2, pr * 2);
        Pen pb(Color((BYTE)(50 * armA), 255, 255, 255), 1 * k);
        g.DrawEllipse(&pb, px - pr, py - pr, pr * 2, pr * 2);
        SolidBrush cap(Color(a, 217, 210, 214));
        g.FillEllipse(&cap, px - 3.5f * k, py - 3.5f * k, 7 * k, 7 * k);
    }

    // столбики у названия программы
    RectF tr = Rect(H_TEXT);
    float ea = mode_ == M_MINI ? 1 - (float)Clamp01(mo / 0.45) : (float)Clamp01((mo - 0.45) / 0.45) * Rev(0.12f, 0.2f);
    if (ea > 0) {
        GraphicsState eqState = g.Save();
        if (mode_ == M_FULL && mo < 0.999f) {
            RectF body = Body();
            GraphicsPath bp;
            RoundRect(bp, body, Lerp(18, kRadius, mo) * k);
            g.SetClip(&bp);
        }
        double t = GetTickCount64() / 83.0;
        SolidBrush bar(WithAlpha(ac, (BYTE)(255 * ea)));
        float ex = tr.X + 1 * k, base = tr.Y + (mode_ == M_MINI ? 11 : 14) * k;
        static const float ph[4] = {0.0f, 1.7f, 3.6f, 1.0f};
        for (int i = 0; i < 4; ++i) {
            float lvl = 0.25f + 0.75f * eq_ * (0.5f + 0.5f * (float)std::sin(t * 0.5 + ph[i]));
            float h = 11 * k * lvl;
            g.FillRectangle(&bar, RectF(ex + i * 4.5f * k, base - h, 2.5f * k, h));
        }
        g.Restore(eqState);
    }
}

void MediaWidget::DrawMini(Graphics& g, float alpha) {
    float k = (float)k_;
    Color fg = ToColor(theme_.fg);
    Color muted = MixColor(ToColor(theme_.muted), fg, 0.3);
    RectF tr = Rect(H_TEXT);
    if (mode_ != M_MINI) {  // большая уже растёт, а маленькая ещё гаснет: те же координаты
        float x0 = (kM + kRollL) * k + (kW - kTW) / 2 * k, y0 = (kM + kLyr + kH - kTH) * k;
        tr = RectF(x0 + 78 * k, y0 + 10 * k, (kTW - 92) * k, 58 * k);
    }
    DrawFaded(g, Grow(tr, 2 * k), alpha * Rev(0.05f, 0.2f), [&](Graphics& gm) {
        Font ft(Family(), 14 * k, FontStyleRegular, UnitPixel);
        FontFamily fam(L"Segoe UI");
        Font fs(&fam, 11.5f * k, FontStyleRegular, UnitPixel);
        Font fh(Family(), 10.5f * k, FontStyleRegular, UnitPixel);
        SolidBrush tb(fg), mb(muted);
        std::wstring head = track_.app.empty() ? L"Плеер" : track_.app;
        gm.DrawString(head.c_str(), -1, &fh, RectF(tr.X + 22 * k, tr.Y - 1 * k, tr.Width - 22 * k, 16 * k),
                      LineFormat(), &mb);
        gm.DrawString(track_.title.c_str(), -1, &ft, RectF(tr.X, tr.Y + 14 * k, tr.Width, 22 * k), LineFormat(), &tb);
        gm.DrawString(track_.artist.c_str(), -1, &fs, RectF(tr.X, tr.Y + 35 * k, tr.Width, 18 * k), LineFormat(), &mb);
    });
}

void MediaWidget::DrawFull(Graphics& g, float alpha) {
    float k = (float)k_;
    Color fg = ToColor(theme_.fg), ac = Ac();
    Color muted = MixColor(ToColor(theme_.muted), fg, 0.3);  // поверх обложки чистый приглушённый цвет теряется
    RectF tr;
    {
        float mx = (kM + kRollL) * k;
        tr = RectF(mx + 166 * k, (kM + kLyr + 16) * k, (kW - 166 - 22) * k, 70 * k);
    }
    // строки поднимаются одна за другой
    auto row = [&](float delay, const RectF& area, const std::function<void(Graphics&)>& paint) {
        GraphicsState st = g.Save();
        g.TranslateTransform(0, 12 * k * (1 - (float)EaseOutBack(Rev(delay, 0.5f), 1.25)));
        DrawFaded(g, area, alpha * Rev(delay, 0.2f), paint);
        g.Restore(st);
    };
    FontFamily fam(L"Segoe UI");
    Font fTitle(Family(), 17 * k, FontStyleRegular, UnitPixel);
    Font fSub(&fam, 12.5f * k, FontStyleRegular, UnitPixel);
    Font fSmall(Family(), 10.5f * k, FontStyleRegular, UnitPixel);
    bool playing = track_.status == media::kPlaying;
    float eqW = 22 * k;  // слева от источника столбики (верхний слой)

    // источник: одна программа - имя, несколько - переключатель
    srcRects_.clear();
    if (track_.sources.size() > 1) {
        float x = tr.X + eqW;
        std::vector<RectF> chips;
        for (int i = 0; i < (int)track_.sources.size() && x < tr.GetRight() - 30 * k; ++i) {
            RectF m;
            g.MeasureString(track_.sources[i].app.c_str(), -1, &fSmall, PointF(0, 0), &m);
            float dotW = track_.sources[i].status == media::kPlaying ? 9 * k : 0;
            RectF chip(x, tr.Y - 1 * k, m.Width + 14 * k + dotW, 20 * k);
            chips.push_back(chip);
            srcRects_.push_back(chip);
            x += chip.Width + 6 * k;
        }
        row(0.12f, RectF(tr.X, tr.Y - 3 * k, tr.Width, 24 * k), [&](Graphics& gs) {
            for (int i = 0; i < (int)chips.size(); ++i) {
                const media::Source& s = track_.sources[i];
                RectF chip = chips[i];
                float dotW = s.status == media::kPlaying ? 9 * k : 0;
                bool on = s.aumid == track_.aumid, hot = hot_ == H_SRC && hotSrc_ == i;
                GraphicsPath cpath;
                RoundRect(cpath, chip, 10 * k);
                SolidBrush cb(on ? WithAlpha(ac, 70) : Color(hot ? 40 : 18, 255, 255, 255));
                gs.FillPath(&cb, &cpath);
                if (on) {
                    Pen cpn(WithAlpha(ac, 160), 1 * k);
                    gs.DrawPath(&cpn, &cpath);
                }
                if (dotW > 0) {
                    SolidBrush db(on ? fg : ac);
                    gs.FillEllipse(&db, RectF(chip.X + 8 * k, chip.Y + chip.Height / 2 - 2.5f * k, 5 * k, 5 * k));
                }
                SolidBrush tb(on || hot ? fg : muted);
                gs.DrawString(s.app.c_str(), -1, &fSmall,
                              RectF(chip.X + 7 * k + dotW, chip.Y, chip.Width - 7 * k - dotW, chip.Height),
                              LineFormat(), &tb);
            }
        });
    } else {
        std::wstring src = track_.app.empty() ? L"Плеер" : track_.app;
        if (!playing) src += L"  ·  на паузе";
        RectF area(tr.X + eqW, tr.Y, tr.Width - eqW, 18 * k);
        row(0.12f, Grow(area, 2 * k), [&](Graphics& gs) {
            SolidBrush sbm(WithAlpha(muted, 230));
            gs.DrawString(src.c_str(), -1, &fSmall, area, LineFormat(), &sbm);
        });
    }

    // название: при смене трека уезжает вверх и приходит снизу; длинное прокручивается
    if (titleW_ < 0) {
        RectF m;
        g.MeasureString(track_.title.c_str(), -1, &fTitle, PointF(0, 0), &m);
        titleW_ = m.Width;
    }
    RectF titleR(tr.X, tr.Y + 20 * k, tr.Width, 28 * k);
    row(0.17f, Grow(titleR, 2 * k), [&](Graphics& gs) {
        GraphicsState st = gs.Save();
        gs.IntersectClip(titleR);
        gs.TranslateTransform(0, (1 - titleA_) * (swap_ ? -14 : 14) * k);
        SolidBrush tb(WithAlpha(MixColor(fg, ac, hover_[H_TEXT] * 0.45), (BYTE)(255 * titleA_)));
        if (titleOff_ > 0) {
            StringFormat free;
            free.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
            free.SetLineAlignment(StringAlignmentCenter);
            gs.DrawString(track_.title.c_str(), -1, &fTitle,
                          RectF(tr.X - titleOff_, titleR.Y, titleW_ + 20 * k, titleR.Height), &free, &tb);
        } else {
            gs.DrawString(track_.title.c_str(), -1, &fTitle, titleR, LineFormat(), &tb);
        }
        gs.Restore(st);
    });
    RectF artistR(tr.X, tr.Y + 47 * k, tr.Width, 20 * k);
    row(0.22f, Grow(artistR, 2 * k), [&](Graphics& gs) {
        SolidBrush mb(WithAlpha(muted, (BYTE)(255 * titleA_)));
        gs.DrawString(track_.artist.c_str(), -1, &fSub, artistR, LineFormat(), &mb);
    });

    // полоса времени; после колеса на её месте ненадолго громкость
    float by = (kM + kLyr) * k;
    RectF bar(tr.X, by + 92 * k, tr.Width, 4 * k);
    RectF barArea(tr.X - 8 * k, bar.Y - 10 * k, tr.Width + 16 * k, 38 * k);
    row(0.27f, barArea, [&](Graphics& gs) {
        float vs = (float)EaseOut(volShow_);
        if (track_.end > 0 && vs < 1) {
            float a = 1 - vs;
            float t = (float)std::clamp((double)track_.Now() / track_.end, 0.0, 1.0);
            float hb = hover_[H_BAR];
            float bh = (4 + 2 * hb) * k;
            RectF line(bar.X, bar.Y + bar.Height / 2 - bh / 2, bar.Width, bh);
            GraphicsPath lp;
            RoundRect(lp, line, bh / 2);
            SolidBrush tbk(Color((BYTE)(46 * a), 255, 255, 255));
            gs.FillPath(&tbk, &lp);
            if (t > 0) {
                RectF done(line.X, line.Y, std::max(bh, line.Width * t), bh);
                GraphicsPath dp;
                RoundRect(dp, done, bh / 2);
                LinearGradientBrush pb(PointF(line.X - 1, 0), PointF(line.X + line.Width + 1, 0),
                                       WithAlpha(MixColor(ac, Color(255, 255, 255), 0.35), (BYTE)(255 * a)),
                                       WithAlpha(ac, (BYTE)(255 * a)));
                gs.FillPath(&pb, &dp);
            }
            float kn = (6 + 6 * hb) * k;
            SolidBrush kb(Color((BYTE)(255 * a * (0.35f + 0.65f * hb)), 255, 255, 255));
            gs.FillEllipse(&kb, RectF(line.X + line.Width * t - kn / 2, line.Y + bh / 2 - kn / 2, kn, kn));
            SolidBrush tm(WithAlpha(muted, (BYTE)(220 * a)));
            RectF times(bar.X, bar.Y + 8 * k, bar.Width, 16 * k);
            gs.DrawString(Clock(track_.Now()).c_str(), -1, &fSmall, times, LineFormat(), &tm);
            gs.DrawString(Clock(track_.end).c_str(), -1, &fSmall, times, LineFormat(StringAlignmentFar), &tm);
        }
        if (vs > 0) DrawVolume(gs);
    });

    // кнопки
    RectF btnArea(tr.X - 4 * k, by + 106 * k, tr.Width + 8 * k, 54 * k);
    row(0.32f, btnArea, [&](Graphics& gs) {
        float cxb = tr.X + tr.Width / 2;
        for (Hit h : {H_PREV, H_PLAY, H_NEXT, H_LYR}) {
            RectF r = h == H_PREV   ? RectF(cxb - 70 * k, by + 116 * k, 34 * k, 34 * k)
                      : h == H_PLAY ? RectF(cxb - 23 * k, by + 110 * k, 46 * k, 46 * k)
                      : h == H_NEXT ? RectF(cxb + 36 * k, by + 116 * k, 34 * k, 34 * k)
                                    : RectF(tr.X + tr.Width - 32 * k, by + 118 * k, 32 * k, 30 * k);
            bool off = (h == H_NEXT && !CanNext()) || (h == H_PREV && !CanPrev());
            float hv = off ? 0 : hover_[h];
            float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
            Color icon = off ? WithAlpha(muted, 70) : MixColor(muted, fg, 0.55 + 0.45 * hv);
            if (h == H_PLAY) {
                if (hv > 0) {
                    float gr = r.Width * (0.5f + 0.25f * hv);
                    GraphicsPath gp;
                    gp.AddEllipse(cx - gr, cy - gr, gr * 2, gr * 2);
                    PathGradientBrush pg(&gp);
                    pg.SetCenterColor(WithAlpha(ac, (BYTE)(110 * hv)));
                    Color edge(0, ac.GetR(), ac.GetG(), ac.GetB());
                    int n = 1;
                    pg.SetSurroundColors(&edge, &n);
                    gs.FillPath(&pg, &gp);
                }
                SolidBrush pb(MixColor(ac, Color(255, 255, 255), hv * 0.15));
                gs.FillEllipse(&pb, r);
                double lum = (0.299 * ac.GetR() + 0.587 * ac.GetG() + 0.114 * ac.GetB()) / 255;
                icon = lum > 0.72 ? Color(255, 24, 20, 28) : Color(255, 255, 255, 255);
            } else if (hv > 0 || (h == H_LYR && lyricsOn_)) {
                GraphicsPath hp;
                RoundRect(hp, r, r.Height / 2);
                SolidBrush hb(h == H_LYR && lyricsOn_ ? WithAlpha(ac, (BYTE)(60 + 40 * hv))
                                                      : Color((BYTE)(30 * hv), 255, 255, 255));
                gs.FillPath(&hb, &hp);
            }
            SolidBrush ib(icon);
            if (h == H_PLAY) {
                if (playing) {
                    float bw = 4.4f * k, bh = 15 * k, gp2 = 4.2f * k;
                    GraphicsPath p1, p2;
                    RoundRect(p1, RectF(cx - gp2 / 2 - bw, cy - bh / 2, bw, bh), 1.5f * k);
                    RoundRect(p2, RectF(cx + gp2 / 2, cy - bh / 2, bw, bh), 1.5f * k);
                    gs.FillPath(&ib, &p1);
                    gs.FillPath(&ib, &p2);
                } else {
                    float s = 16 * k;
                    Triangle(gs, &ib, cx - s * 0.34f, cy - s / 2, s, true);
                }
            } else if (h == H_LYR) {
                // значок текста: три строки разной длины
                SolidBrush lb(lyricsOn_ ? fg : icon);
                float lw[3] = {14, 10, 12};
                for (int i = 0; i < 3; ++i) {
                    GraphicsPath p;
                    RoundRect(p, RectF(cx - 7 * k, cy - 6 * k + i * 5 * k, lw[i] * k, 2.2f * k), 1.1f * k);
                    gs.FillPath(&lb, &p);
                }
            } else {
                float s = 12 * k, bw = 2.6f * k;
                bool next = h == H_NEXT;
                float tx = next ? cx - s * 0.55f : cx - s * 0.32f;
                Triangle(gs, &ib, tx, cy - s / 2, s, next);
                gs.FillRectangle(&ib, RectF(next ? tx + s * 0.87f : tx - bw, cy - s / 2, bw, s));
            }
        }
    });
}

void MediaWidget::DrawVolume(Graphics& g) {
    float k = (float)k_, a = (float)EaseOut(volShow_);
    float mx = (kM + kRollL) * k;
    RectF bar(mx + 166 * k, (kM + kLyr + 92) * k, (kW - 166 - 22) * k, 4 * k);
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted);
    float bh = 6 * k;
    RectF line(bar.X, bar.Y + bar.Height / 2 - bh / 2, bar.Width, bh);
    GraphicsPath lp;
    RoundRect(lp, line, bh / 2);
    SolidBrush tb(Color((BYTE)(46 * a), 255, 255, 255));
    g.FillPath(&tb, &lp);
    float v = std::clamp(vol_, 0.0f, 1.0f);
    if (v > 0) {
        GraphicsPath dp;
        RoundRect(dp, RectF(line.X, line.Y, std::max(bh, line.Width * v), bh), bh / 2);
        SolidBrush vb(Color((BYTE)(240 * a), 255, 255, 255));
        g.FillPath(&vb, &dp);
    }
    Font f(Family(), 10.5f * k, FontStyleRegular, UnitPixel);
    RectF row(bar.X, bar.Y + 8 * k, bar.Width, 16 * k);
    SolidBrush mb(WithAlpha(muted, (BYTE)(230 * a))), fb(WithAlpha(fg, (BYTE)(255 * a)));
    std::wstring what = volApp_ ? L"Громкость плеера" : L"Общая громкость";
    g.DrawString(what.c_str(), -1, &f, row, LineFormat(), &mb);
    std::wstring pct = std::to_wstring((int)std::lround(v * 100)) + L"%";
    g.DrawString(pct.c_str(), -1, &f, row, LineFormat(StringAlignmentFar), &fb);
}

void MediaWidget::DrawLyrics(Graphics& g, const RectF& area, float alpha) {
    if (area.Height < 8) return;
    float k = (float)k_;
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), ac = Ac();
    lyrics::Lyrics ly = fetch_.Get(track_.artist, track_.title);
    FontFamily fam(L"Segoe UI");
    Font fCur(Family(), 16 * k, FontStyleRegular, UnitPixel);
    Font fLine(&fam, 13.5f * k, FontStyleRegular, UnitPixel);
    GraphicsState st = g.Save();
    g.IntersectClip(area);
    const wchar_t* note = nullptr;
    switch (ly.state) {
        case lyrics::kNone:
        case lyrics::kLoading:
            note = L"Ищу текст песни...";
            break;
        case lyrics::kMissing:
            note = L"Текст для этой песни не нашёлся";
            break;
        case lyrics::kInstrumental:
            note = L"Без слов";
            break;
        case lyrics::kOffline:
            note = L"Нет связи с lrclib.net";
            break;
        default:
            break;
    }
    if (note) {
        SolidBrush b(WithAlpha(muted, (BYTE)(230 * alpha)));
        g.DrawString(note, -1, &fLine, area, LineFormat(StringAlignmentCenter), &b);
        g.Restore(st);
        return;
    }
    float lineH = 30 * k;
    if (ly.state == lyrics::kSynced) {
        // текущая строка в середине, соседние гаснут к краям
        float cy = area.Y + area.Height * 0.5f;
        int cur = ly.LineAt(track_.Now());
        int first = std::max(0, (int)std::floor(lyrPos_ - area.Height / lineH / 2) - 1);
        int last = std::min((int)ly.lines.size() - 1, (int)std::ceil(lyrPos_ + area.Height / lineH / 2) + 1);
        for (int i = first; i <= last; ++i) {
            float y = cy + (i - lyrPos_) * lineH;
            float dist = std::fabs(i - lyrPos_);
            float fade = std::max(0.0f, 1 - dist * 0.24f) * alpha;
            if (fade <= 0) continue;
            bool on = i == cur;
            std::wstring t = ly.lines[i].text.empty() ? L"..." : ly.lines[i].text;
            SolidBrush b(on ? WithAlpha(fg, (BYTE)(255 * fade)) : WithAlpha(muted, (BYTE)(210 * fade)));
            g.DrawString(t.c_str(), -1, on ? &fCur : &fLine, RectF(area.X, y - lineH / 2, area.Width, lineH),
                         LineFormat(StringAlignmentCenter), &b);
            if (on) {
                SolidBrush db(WithAlpha(ac, (BYTE)(255 * alpha)));
                g.FillEllipse(&db, RectF(area.X, y - 3 * k, 6 * k, 6 * k));
            }
        }
    } else {
        int first = (int)plainOff_;
        float y = area.Y + 4 * k - (plainOff_ - first) * lineH;
        for (int i = first; i < (int)ly.lines.size() && y < area.GetBottom(); ++i, y += lineH * 0.85f) {
            SolidBrush b(WithAlpha(i == first && plainOff_ > 0 ? muted : fg, (BYTE)(235 * alpha)));
            g.DrawString(ly.lines[i].text.c_str(), -1, &fLine, RectF(area.X, y, area.Width, lineH * 0.85f),
                         LineFormat(StringAlignmentCenter), &b);
        }
    }
    g.Restore(st);
}
