// Отрисовка панели «Сейчас играет»: маленькая карточка, полная, громкость и текст песни.
#include "desk/widget.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/night.h"
#include "desk/widget_internal.h"
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

void MediaWidget::Render() {
    dirty_ = false;
    SIZE sz = WindowSize();
    int W = sz.cx, H = sz.cy;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        if (mode_ == M_MINI)
            DrawMini(g);
        else
            DrawFull(g);
    }
    // для разработки: RADIAL_SNAP=1 - каждый кадр панели пишется в media_snap.png рядом с exe
    if (GetEnvironmentVariableW(L"RADIAL_SNAP", nullptr, 0)) {
        Bitmap snap(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
        snap.Save((ExeDir() + L"\\media_snap.png").c_str(), &png, nullptr);
    }
    // спрятанная карточка стоит верхним краем на нижнем краю экрана
    float e = (float)EaseOut(slide_);
    LONG hidden = mon_.bottom - (LONG)Body().Y;
    POINT at{pos_.x, (LONG)(hidden + (pos_.y - hidden) * e)};
    POINT src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(255 * std::min(1.0f, e * 1.4f)), AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, scr, &at, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd_)) ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void MediaWidget::DrawMini(Graphics& g) {
    float k = (float)k_;
    RectF body = Body();
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), bg = ToColor(theme_.bg), ac = ToColor(accent_);
    if (shadowMini_) g.DrawImage(shadowMini_.get(), body.X - kPad * k, body.Y - kPad * k + 6 * k);
    GraphicsPath path;
    RoundRect(path, body, 18 * k);
    GraphicsState st = g.Save();
    g.SetClip(&path);
    if (artBg_) {
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.DrawImage(artBg_.get(), Inset(body, -20 * k, -60 * k));
        SolidBrush veil(WithAlpha(bg, 160));
        g.FillRectangle(&veil, body);
    } else {
        SolidBrush b(WithAlpha(bg, 245));
        g.FillRectangle(&b, body);
    }
    if (track_.end > 0) {
        float t = (float)std::clamp((double)track_.Now() / track_.end, 0.0, 1.0);
        SolidBrush pb(ac);
        g.FillRectangle(&pb, RectF(body.X, body.GetBottom() - 3 * k, body.Width * t, 3 * k));
    }
    g.Restore(st);
    Pen rim(Color(40, 255, 255, 255), 1 * k);
    g.DrawPath(&rim, &path);

    RectF cov = Rect(H_COVER);
    GraphicsPath cp;
    RoundRect(cp, cov, 11 * k);
    if (artSmall_) {
        st = g.Save();
        g.SetClip(&cp);
        g.DrawImage(artSmall_.get(), cov);
        g.Restore(st);
    } else {
        SolidBrush cb(WithAlpha(ac, 220));
        g.FillPath(&cb, &cp);
        if (note_) g.DrawImage(note_.get(), Inset(cov, 11 * k, 11 * k));
    }
    RectF tr = Rect(H_TEXT);
    Font ft(Family(), 14 * k, FontStyleRegular, UnitPixel);
    FontFamily fam(L"Segoe UI");
    Font fs(&fam, 11.5f * k, FontStyleRegular, UnitPixel);
    SolidBrush tb(fg), mb(muted), ab(ac);
    std::wstring head = L"Сейчас играет";
    if (!track_.app.empty()) head += L"  /  " + track_.app;
    Font fh(Family(), 9.5f * k, FontStyleRegular, UnitPixel);
    g.DrawString(head.c_str(), -1, &fh, RectF(tr.X, tr.Y, tr.Width, 14 * k), LineFormat(), &ab);
    g.DrawString(track_.title.c_str(), -1, &ft, RectF(tr.X, tr.Y + 14 * k, tr.Width, 22 * k), LineFormat(), &tb);
    g.DrawString(track_.artist.c_str(), -1, &fs, RectF(tr.X, tr.Y + 35 * k, tr.Width, 18 * k), LineFormat(), &mb);
}

void MediaWidget::DrawFull(Graphics& g) {
    float k = (float)k_;
    RectF body = Body();
    float ex = (float)EaseOut(expand_);
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), bg = ToColor(theme_.bg), ac = ToColor(accent_);

    // тень: готовая картинка для ближайшего размера, растянутая под текущий
    Bitmap* sh = ex > 0.5f ? shadowTall_.get() : shadowFull_.get();
    if (sh)
        g.DrawImage(sh, RectF(body.X - kPad * k, body.Y - kPad * k + 8 * k, body.Width + kPad * k * 2,
                              body.Height + kPad * k * 2));

    GraphicsPath path;
    RoundRect(path, body, kRadius * k);
    GraphicsState st = g.Save();
    g.SetClip(&path);
    // фон: размытая обложка под вуалью цвета темы, слева - отсвет акцента за обложкой
    if (artBg_) {
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.DrawImage(artBg_.get(), Inset(body, -30 * k, -30 * k));
        SolidBrush veil(WithAlpha(bg, (BYTE)(std::min(235.0f, 150 + 40 * ex + (night::On() ? 50 : 0)))));
        g.FillRectangle(&veil, body);
    } else {
        SolidBrush b(WithAlpha(bg, 245));
        g.FillRectangle(&b, body);
    }
    RectF cov = Rect(H_COVER);
    {
        float gr = 150 * k;
        GraphicsPath gp;
        gp.AddEllipse(cov.X + cov.Width / 2 - gr, cov.Y + cov.Height / 2 - gr, gr * 2, gr * 2);
        PathGradientBrush pg(&gp);
        pg.SetCenterColor(WithAlpha(ac, 70));
        Color edge(0, ac.GetR(), ac.GetG(), ac.GetB());
        int n = 1;
        pg.SetSurroundColors(&edge, &n);
        g.FillPath(&pg, &gp);
    }
    LinearGradientBrush shine(PointF(0, body.Y), PointF(0, body.Y + 100 * k), Color(26, 255, 255, 255),
                              Color(0, 255, 255, 255));
    g.FillRectangle(&shine, RectF(body.X, body.Y, body.Width, 100 * k));
    if (ex > 0.01f) {
        // текст песни над основной частью и тонкая линия-разделитель
        RectF area = Rect(H_LYRICS);
        DrawLyrics(g, area, ex);
        SolidBrush line(Color((BYTE)(30 * ex), 255, 255, 255));
        g.FillRectangle(&line, RectF(body.X + 24 * k, cov.Y - 10 * k, body.Width - 48 * k, 1 * k));
    }
    g.Restore(st);
    Pen rim(Color(40, 255, 255, 255), 1 * k);
    g.DrawPath(&rim, &path);

    // обложка с тенью
    float cr = 16 * k;
    for (int i = 3; i >= 1; --i) {
        GraphicsPath sp;
        RoundRect(sp, RectF(cov.X - i * k, cov.Y + (2 + i * 1.5f) * k, cov.Width + 2 * i * k, cov.Height + i * k),
                  cr + i * k);
        SolidBrush sb(Color((BYTE)(26 + 8 * (3 - i)), 0, 0, 0));
        g.FillPath(&sb, &sp);
    }
    GraphicsPath cp;
    RoundRect(cp, cov, cr);
    if (art_) {
        st = g.Save();
        g.SetClip(&cp);
        g.DrawImage(art_.get(), cov);
        if (hover_[H_COVER] > 0) {
            SolidBrush hb(Color((BYTE)(36 * hover_[H_COVER]), 255, 255, 255));
            g.FillRectangle(&hb, cov);
        }
        g.Restore(st);
    } else {
        LinearGradientBrush cb(PointF(cov.X, cov.Y), PointF(cov.X + cov.Width, cov.Y + cov.Height), WithAlpha(ac, 230),
                               WithAlpha(ToColor(theme_.glow), 150));
        g.FillPath(&cb, &cp);
        if (note_) g.DrawImage(note_.get(), Inset(cov, 26 * k, 26 * k));
    }
    Pen cpen(Color(46, 255, 255, 255), 1 * k);
    g.DrawPath(&cpen, &cp);

    // источник: одна программа - точка и имя, несколько - переключатель
    FontFamily fam(L"Segoe UI");
    Font fTitle(Family(), 17 * k, FontStyleRegular, UnitPixel);
    Font fSub(&fam, 12.5f * k, FontStyleRegular, UnitPixel);
    Font fSmall(Family(), 10.5f * k, FontStyleRegular, UnitPixel);
    RectF tr = Rect(H_TEXT);
    bool playing = track_.status == media::kPlaying;
    srcRects_.clear();
    if (track_.sources.size() > 1) {
        float x = tr.X;
        for (int i = 0; i < (int)track_.sources.size() && x < tr.GetRight() - 30 * k; ++i) {
            const media::Source& s = track_.sources[i];
            RectF m;
            g.MeasureString(s.app.c_str(), -1, &fSmall, PointF(0, 0), &m);
            float dotW = s.status == media::kPlaying ? 9 * k : 0;
            RectF chip(x, tr.Y - 1 * k, m.Width + 14 * k + dotW, 20 * k);
            bool on = s.aumid == track_.aumid;
            bool hot = hot_ == H_SRC && hotSrc_ == i;
            GraphicsPath cpath;
            RoundRect(cpath, chip, 10 * k);
            SolidBrush cb(on ? WithAlpha(ac, 70) : Color(hot ? 40 : 18, 255, 255, 255));
            g.FillPath(&cb, &cpath);
            if (on) {
                Pen cpn(WithAlpha(ac, 160), 1 * k);
                g.DrawPath(&cpn, &cpath);
            }
            if (dotW > 0) {
                SolidBrush db(on ? fg : ac);
                g.FillEllipse(&db, RectF(chip.X + 8 * k, chip.Y + chip.Height / 2 - 2.5f * k, 5 * k, 5 * k));
            }
            SolidBrush tb(on || hot ? fg : muted);
            g.DrawString(s.app.c_str(), -1, &fSmall, RectF(chip.X + 7 * k + dotW, chip.Y, m.Width + 4 * k, chip.Height),
                         LineFormat(), &tb);
            srcRects_.push_back(chip);
            x += chip.Width + 6 * k;
        }
    } else {
        float dot = 6 * k;
        SolidBrush db(playing ? ac : WithAlpha(muted, 200));
        g.FillEllipse(&db, RectF(tr.X + 1 * k, tr.Y + 9 * k - dot / 2, dot, dot));
        std::wstring src = track_.app.empty() ? L"Плеер" : track_.app;
        if (!playing) src += L"  /  на паузе";
        SolidBrush sbm(WithAlpha(muted, 230));
        g.DrawString(src.c_str(), -1, &fSmall, RectF(tr.X + 12 * k, tr.Y, tr.Width - 12 * k, 18 * k), LineFormat(),
                     &sbm);
    }
    SolidBrush tb(MixColor(fg, ac, hover_[H_TEXT] * 0.45));
    g.DrawString(track_.title.c_str(), -1, &fTitle, RectF(tr.X, tr.Y + 20 * k, tr.Width, 28 * k), LineFormat(), &tb);
    SolidBrush mb(muted);
    g.DrawString(track_.artist.c_str(), -1, &fSub, RectF(tr.X, tr.Y + 47 * k, tr.Width, 20 * k), LineFormat(), &mb);

    // полоса времени; после колеса на её месте ненадолго громкость
    RectF bar = Rect(H_BAR);
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
        g.FillPath(&tbk, &lp);
        if (t > 0) {
            RectF done(line.X, line.Y, std::max(bh, line.Width * t), bh);
            GraphicsPath dp;
            RoundRect(dp, done, bh / 2);
            LinearGradientBrush pb(PointF(line.X - 1, 0), PointF(line.X + line.Width + 1, 0),
                                   WithAlpha(MixColor(ac, Color(255, 255, 255), 0.35), (BYTE)(255 * a)),
                                   WithAlpha(ac, (BYTE)(255 * a)));
            g.FillPath(&pb, &dp);
        }
        float kn = (6 + 6 * hb) * k;
        SolidBrush kb(Color((BYTE)(255 * a * (0.35f + 0.65f * hb)), 255, 255, 255));
        g.FillEllipse(&kb, RectF(line.X + line.Width * t - kn / 2, line.Y + bh / 2 - kn / 2, kn, kn));
        SolidBrush tm(WithAlpha(muted, (BYTE)(220 * a)));
        RectF times(bar.X, bar.Y + 8 * k, bar.Width, 16 * k);
        g.DrawString(Clock(track_.Now()).c_str(), -1, &fSmall, times, LineFormat(), &tm);
        g.DrawString(Clock(track_.end).c_str(), -1, &fSmall, times, LineFormat(StringAlignmentFar), &tm);
    }
    if (vs > 0) DrawVolume(g);

    // кнопки
    for (Hit h : {H_PREV, H_PLAY, H_NEXT, H_LYR}) {
        RectF r = Rect(h);
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
                g.FillPath(&pg, &gp);
            }
            SolidBrush pb(MixColor(ac, Color(255, 255, 255), hv * 0.15));
            g.FillEllipse(&pb, r);
            double lum = (0.299 * ac.GetR() + 0.587 * ac.GetG() + 0.114 * ac.GetB()) / 255;
            icon = lum > 0.72 ? Color(255, 24, 20, 28) : Color(255, 255, 255, 255);
        } else if (hv > 0 || (h == H_LYR && lyricsOn_)) {
            GraphicsPath hp;
            RoundRect(hp, r, r.Height / 2);
            SolidBrush hb(h == H_LYR && lyricsOn_ ? WithAlpha(ac, (BYTE)(60 + 40 * hv))
                                                  : Color((BYTE)(30 * hv), 255, 255, 255));
            g.FillPath(&hb, &hp);
        }
        SolidBrush ib(icon);
        if (h == H_PLAY) {
            if (track_.status == media::kPlaying) {
                float bw = 4.4f * k, bh = 15 * k, gp2 = 4.2f * k;
                GraphicsPath p1, p2;
                RoundRect(p1, RectF(cx - gp2 / 2 - bw, cy - bh / 2, bw, bh), 1.5f * k);
                RoundRect(p2, RectF(cx + gp2 / 2, cy - bh / 2, bw, bh), 1.5f * k);
                g.FillPath(&ib, &p1);
                g.FillPath(&ib, &p2);
            } else {
                float s = 16 * k;
                Triangle(g, &ib, cx - s * 0.34f, cy - s / 2, s, true);
            }
        } else if (h == H_LYR) {
            // значок текста: три строки разной длины
            SolidBrush lb(lyricsOn_ ? fg : icon);
            float lw[3] = {14, 10, 12};
            for (int i = 0; i < 3; ++i) {
                GraphicsPath p;
                RoundRect(p, RectF(cx - 7 * k, cy - 6 * k + i * 5 * k, lw[i] * k, 2.2f * k), 1.1f * k);
                g.FillPath(&lb, &p);
            }
        } else {
            float s = 12 * k, bw = 2.6f * k;
            bool next = h == H_NEXT;
            float tx = next ? cx - s * 0.55f : cx - s * 0.32f;
            Triangle(g, &ib, tx, cy - s / 2, s, next);
            g.FillRectangle(&ib, RectF(next ? tx + s * 0.87f : tx - bw, cy - s / 2, bw, s));
        }
    }
}

void MediaWidget::DrawVolume(Graphics& g) {
    float k = (float)k_, a = (float)EaseOut(volShow_);
    RectF bar = Rect(H_BAR);
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
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), ac = ToColor(accent_);
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
