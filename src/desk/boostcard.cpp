#include "desk/boostcard.h"

#include <algorithm>

#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace {

constexpr float kPad = 16, kText = 66;  // поля и начало текста (после знака)

bool Has(const std::wstring& s, const wchar_t* w) { return s.find(w) != std::wstring::npos; }

// «Великобритания -> Германия»: в подписи стрелка рисуется настоящей
std::wstring Pretty(std::wstring s) {
    for (size_t at; (at = s.find(L"->")) != std::wstring::npos;) s.replace(at, 2, L"→");
    return s;
}

}  // namespace

void BoostCard::Create(HINSTANCE inst) { CreateWnd(inst, L"MoonRadialBoost", L"Boost"); }

void BoostCard::Show(Mode mode, const std::wstring& title, const std::wstring& sub, const ThemeInfo& theme, int holdMs,
                     Glyph glyph) {
    mode_ = mode;
    title_ = title;
    sub_ = Pretty(sub);
    theme_ = theme;
    if (glyph == G_NONE) {
        // знак по виду карточки и по словам заголовка
        if (mode == OFFER)
            glyph = G_ASK;
        else if (mode == WORKING)
            glyph = G_BUSY;
        else if (mode == RESTORE)
            glyph = G_RESTORE;
        else if (Has(title, L"не получилось") || Has(title, L"Не получилось") || Has(title, L"Не удалось") ||
                 Has(title, L"не найден") || Has(title, L"Пропал"))
            glyph = G_FAIL;
        else if (Has(title, L"Подождите") || Has(title, L"Мало места") || Has(title, L"В буфере нет"))
            glyph = G_WARN;
        else if (Has(title, L"Выход") || Has(title, L"Интернет") || Has(title, L"Сеть"))
            glyph = G_NET;
        else
            glyph = G_OK;
    }
    glyph_ = glyph;
    // вопрос висит дольше, итог - коротко; длинный текст - чуть дольше
    int hold = holdMs > 0        ? holdMs
               : mode == DONE    ? 4500 + (int)std::min<size_t>(sub_.size(), 160) * 25
               : mode == WORKING ? 90000
                                 : 16000;
    Open(hold);
}

Color BoostCard::Tone() const {
    switch (glyph_) {
        case G_OK:
            return Color(255, 92, 196, 140);
        case G_FAIL:
            return Color(255, 236, 98, 90);
        case G_WARN:
            return Color(255, 236, 176, 72);
        default:
            return ToColor(theme_.ac);
    }
}

std::wstring BoostCard::Label(bool yes) const {
    if (yes) return !yesLabel.empty() ? yesLabel : mode_ == OFFER ? L"Освободить" : L"Вернуть";
    return !noLabel.empty() ? noLabel : mode_ == OFFER ? L"Не сейчас" : L"Не нужно";
}

float BoostCard::BodyHeight(Graphics& g) {
    float K = k();
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12.5f * K, FontStyleRegular, UnitPixel);
    float tw = (kW - kText - kPad) * K;
    subH_ = std::min(TextHeight(g, sub_, fS, tw), 4 * 17.5f * K);  // не больше четырёх строк
    float h = kPad + 21 + (subH_ > 0 ? 4 + subH_ / K : 0);
    if (Buttons()) {
        float bh = 32, top = std::max(h + 12, 52.0f);
        float bw = ButtonWidth(g, Label(true)) / K, nw = ButtonWidth(g, Label(false)) / K;
        RectF body(kM * K, kM * K, kW * K, 0);
        float right = body.GetRight() - kPad * K;
        no_ = RectF(right - nw * K, body.Y + top * K, nw * K, bh * K);
        yes_ = RectF(no_.X - 8 * K - bw * K, no_.Y, bw * K, bh * K);
        h = top + bh;
    } else {
        yes_ = no_ = RectF();
    }
    return std::max(h + kPad + 2, 70.0f);
}

void BoostCard::Paint(Graphics& g, const RectF& body) {
    float K = k();
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted);
    DrawGlyph(g, body.X + 35 * K, body.Y + 35 * K, glyph_, Tone());

    float x0 = body.X + kText * K, tw = body.GetRight() - kPad * K - x0;
    Font fT(Family(), 14.5f * K, FontStyleRegular, UnitPixel);
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12.5f * K, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tB(fg), mB(MixColor(muted, fg, 0.25));
    g.DrawString(title_.c_str(), -1, &fT, RectF(x0, body.Y + kPad * K - 1 * K, tw - 26 * K, 22 * K), &sf, &tB);
    if (!sub_.empty()) {
        StringFormat wf;
        wf.SetTrimming(StringTrimmingEllipsisWord);
        g.DrawString(sub_.c_str(), -1, &fS, RectF(x0, body.Y + (kPad + 25) * K, tw, subH_ + 1), &wf, &mB);
    }
    if (Buttons()) {
        DrawButton(g, yes_, Label(true), true, H_YES);
        DrawButton(g, no_, Label(false), false, H_NO);
    }
}

int BoostCard::HitContent(const PointF& p) const {
    if (!Buttons()) return H_NONE;
    if (yes_.Contains(p)) return H_YES;
    if (no_.Contains(p)) return H_NO;
    return H_NONE;
}

void BoostCard::Click(int id) {
    if (id == H_YES) {
        // сначала уходит вопрос, потом действие: оно может сразу показать «Работаю...» в этой же карточке
        Hide();
        // через копию: действие может задать новый вопрос и переписать onYes, пока само ещё выполняется
        if (onYes) {
            auto f = onYes;
            f();
        }
        return;
    }
    if (id == H_NO) {
        Hide();
        if (onNo) {
            auto f = std::move(onNo);
            onNo = nullptr;
            f();
        }
        return;
    }
    if (id == H_BODY && mode_ == DONE) Dismiss();  // итог убирается кликом
}
