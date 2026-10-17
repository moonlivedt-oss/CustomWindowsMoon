#include "desk/boostpick.h"

#include <algorithm>
#include <cmath>

#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

constexpr float kPad = 16, kText = 66, kRowH = 30, kSegW = 56;
const wchar_t* kActs[3] = {L"Закрыть", L"Усыпить", L"Оставить"};

std::wstring Mb(double mb) {
    wchar_t b[32];
    if (mb >= 1024)
        swprintf(b, 32, L"%.1f ГБ", mb / 1024);
    else
        swprintf(b, 32, L"%d МБ", (int)mb);
    return b;
}

}  // namespace

void BoostPick::Create(HINSTANCE inst) { CreateWnd(inst, L"MoonRadialBoostPick", L"Boost"); }

void BoostPick::Show(const std::wstring& title, const std::wstring& sub, std::vector<Row> rows,
                     const ThemeInfo& theme) {
    if (rows.size() > (size_t)kRows) rows.resize(kRows);
    title_ = title;
    sub_ = sub;
    rows_ = std::move(rows);
    theme_ = theme;
    for (size_t i = 0; i < rows_.size(); ++i) slide_[i] = (float)rows_[i].act;
    Open(30000);
}

double BoostPick::Freed() const {
    double mb = 0;
    for (const Row& r : rows_) mb += r.act == 0 ? r.mb : r.act == 1 ? r.mb * 0.8 : 0;
    return mb;
}

float BoostPick::BodyHeight(Graphics& g) {
    float K = k();
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12.5f * K, FontStyleRegular, UnitPixel);
    subH_ = std::min(TextHeight(g, sub_, fS, (kW - kText - kPad) * K), 2 * 17.5f * K);
    rowsY_ = std::max(kPad + 21 + (subH_ > 0 ? 4 + subH_ / K : 0) + 12, 62.0f);
    RectF body(kM * K, kM * K, kW * K, 0);
    float segX = body.GetRight() - kPad * K - 3 * kSegW * K;
    for (size_t i = 0; i < rows_.size(); ++i)
        for (int s = 0; s < 3; ++s)
            seg_[i][s] = RectF(segX + s * kSegW * K, body.Y + (rowsY_ + i * kRowH + 3) * K, kSegW * K, (kRowH - 6) * K);
    float top = rowsY_ + rows_.size() * kRowH + 12, bh = 32;
    float aw = ButtonWidth(g, L"Применить") / K, sw = ButtonWidth(g, L"Не сейчас") / K;
    skip_ = RectF(body.GetRight() - kPad * K - sw * K, body.Y + top * K, sw * K, bh * K);
    apply_ = RectF(skip_.X - 8 * K - aw * K, skip_.Y, aw * K, bh * K);
    return top + bh + kPad + 2;
}

void BoostPick::Paint(Graphics& g, const RectF& body) {
    float K = k();
    ULONGLONG now = GetTickCount64();
    float dt = lastPaint_ ? std::min(0.05f, (now - lastPaint_) / 1000.0f) : 0.016f;
    lastPaint_ = now;
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), ac = ToColor(theme_.ac);
    DrawGlyph(g, body.X + 35 * K, body.Y + 35 * K, G_ASK, ac);

    float x0 = body.X + kText * K, tw = body.GetRight() - kPad * K - x0;
    Font fT(Family(), 14.5f * K, FontStyleRegular, UnitPixel);
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12.5f * K, FontStyleRegular, UnitPixel), fR(&fam, 12.5f * K, FontStyleRegular, UnitPixel),
        fSeg(&fam, 11.5f * K, FontStyleRegular, UnitPixel);
    StringFormat one;
    one.SetFormatFlags(StringFormatFlagsNoWrap);
    one.SetTrimming(StringTrimmingEllipsisCharacter);
    one.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tB(fg), mB(MixColor(muted, fg, 0.25));
    g.DrawString(title_.c_str(), -1, &fT, RectF(x0, body.Y + (kPad - 1) * K, tw - 26 * K, 22 * K), &one, &tB);
    if (!sub_.empty()) {
        StringFormat wf;
        wf.SetTrimming(StringTrimmingEllipsisWord);
        g.DrawString(sub_.c_str(), -1, &fS, RectF(x0, body.Y + (kPad + 25) * K, tw, subH_ + 1), &wf, &mB);
    }

    bool moving = false;
    StringFormat cf;
    cf.SetAlignment(StringAlignmentCenter);
    cf.SetLineAlignment(StringAlignmentCenter);
    cf.SetFormatFlags(StringFormatFlagsNoWrap);
    StringFormat rf;
    rf.SetFormatFlags(StringFormatFlagsNoWrap);
    rf.SetLineAlignment(StringAlignmentCenter);
    rf.SetAlignment(StringAlignmentFar);
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        float y = body.Y + (rowsY_ + i * kRowH) * K;
        // имя и память
        float nameW = seg_[i][0].X - body.X - kPad * K - 60 * K;
        SolidBrush nb(r.act == 2 ? MixColor(muted, fg, 0.35) : fg);
        g.DrawString(r.title.c_str(), -1, &fR, RectF(body.X + kPad * K, y, nameW, kRowH * K), &one, &nb);
        g.DrawString(Mb(r.mb).c_str(), -1, &fSeg, RectF(seg_[i][0].X - 62 * K, y, 54 * K, kRowH * K), &rf, &mB);
        // переключатель из трёх: плашка скользит к выбранному
        RectF track(seg_[i][0].X, seg_[i][0].Y, 3 * kSegW * K, seg_[i][0].Height);
        GraphicsPath tp;
        RoundRectPath(tp, track, track.Height / 2);
        SolidBrush tb(Color(18, 255, 255, 255));
        g.FillPath(&tb, &tp);
        float want = (float)r.act, d = want - slide_[i];
        if (std::fabs(d) > 0.002f) {
            slide_[i] += d * std::min(1.0f, dt * 16);
            moving = true;
        } else {
            slide_[i] = want;
        }
        RectF knob(track.X + slide_[i] * kSegW * K + 1.5f * K, track.Y + 1.5f * K, kSegW * K - 3 * K,
                   track.Height - 3 * K);
        GraphicsPath kp;
        RoundRectPath(kp, knob, knob.Height / 2);
        Color kc = r.act == 0   ? ac
                   : r.act == 1 ? MixColor(ac, Color(255, 120, 150, 255), 0.55)
                                : Color(255, 90, 90, 100);
        SolidBrush kb(WithAlpha(kc, 225));
        g.FillPath(&kb, &kp);
        for (int s = 0; s < 3; ++s) {
            float hv = HoverOf(H_SEG + (int)i * 3 + s);
            bool sel = r.act == s;
            SolidBrush sb(sel ? Color(255, 255, 255, 255) : MixColor(muted, fg, 0.3 + 0.5 * hv));
            g.DrawString(kActs[s], -1, &fSeg, seg_[i][s], &cf, &sb);
        }
    }
    // итог выбора внизу слева
    std::wstring sum = Freed() > 1 ? L"Освободится ~" + Mb(Freed()) : L"Ничего не трогаем";
    g.DrawString(sum.c_str(), -1, &fS, RectF(body.X + kPad * K, apply_.Y, apply_.X - body.X - kPad * K, apply_.Height),
                 &one, &mB);
    DrawButton(g, apply_, L"Применить", true, H_APPLY);
    DrawButton(g, skip_, L"Не сейчас", false, H_SKIP);
    if (moving) Invalidate();
}

int BoostPick::HitContent(const PointF& p) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        for (int s = 0; s < 3; ++s)
            if (seg_[i][s].Contains(p)) return H_SEG + (int)i * 3 + s;
    if (apply_.Contains(p)) return H_APPLY;
    if (skip_.Contains(p)) return H_SKIP;
    return H_NONE;
}

void BoostPick::Click(int id) {
    if (id >= H_SEG && id < H_SEG + (int)rows_.size() * 3) {
        rows_[(id - H_SEG) / 3].act = (id - H_SEG) % 3;
        lastPaint_ = 0;
        Invalidate();
        return;
    }
    if (id == H_APPLY) {
        Hide();
        if (onApply) {
            auto f = onApply;
            f(rows_);
        }
        return;
    }
    if (id == H_SKIP) Dismiss();
}

void BoostPick::Closed() {
    if (onSkip) {
        auto f = std::move(onSkip);
        onSkip = nullptr;
        f();
    }
}
