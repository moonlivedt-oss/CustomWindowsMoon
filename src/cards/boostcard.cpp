#include "cards/boostcard.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "cards/cardlog.h"
#include "core/fx.h"
#include "desktop/share.h"
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

// Когда последний раз нажимали кнопку на любой карточке: сообщение сразу после нажатия - итог того, что
// попросили («Готово», «Список в буфере»), а не новость, и в счёт «не читают» не идёт
ULONGLONG g_lastClick = 0;

}  // namespace

void BoostCard::Create(HINSTANCE inst) { CreateWnd(inst, L"MoonRadialBoost", L"Boost"); }

void BoostCard::InfoGone() {
    if (!info_) return;
    info_ = false;
    // смахнули руками - не нужна, даже если мышь по пути прошла над ней; ушла сама - важно, читали ли
    cardlog::Closed(infoKind_, seen_ && !ClosedByHand());
}

void BoostCard::Unanswered() {
    if (!asked_) return;
    asked_ = false;
    cardlog::Missed m;
    m.kind = askKind_;
    m.title = title_;
    m.sub = sub_;
    m.yes = askYes_;
    m.no = askNo_;
    m.onYes = askOnYes_;  // снимки с показа: к уходу карточки onYes могли уже переписать под следующий вопрос
    m.onNo = askOnNo_;
    cardlog::Record(std::move(m));
    cardlog::Closed(askKind_, false);
}

void BoostCard::Enqueue(Later l) {
    // то же сообщение второй раз не копится; больше восьми отложенных - самое старое уходит
    later_.erase(std::remove_if(later_.begin(), later_.end(),
                                [&](const Later& o) { return o.title == l.title && o.kind == l.kind; }),
                 later_.end());
    if (later_.size() >= 8) later_.erase(later_.begin());
    later_.push_back(std::move(l));
}

void BoostCard::MenuChoose(int minutes, bool repeat) {
    if (minutes <= 0) {  // выключить повтор: карточка остаётся на экране и дальше живёт как обычная
        repeatMin_ = 0;
        return;
    }
    // отложили - значит, отреагировали: в пропущенные и в счёт «не читают» это не идёт
    seen_ = true;
    if (asked_) {
        asked_ = false;
        cardlog::Closed(askKind_, true);
    }
    if (info_) {
        info_ = false;
        cardlog::Closed(infoKind_, true);
    }
    repeatMin_ = 0;
    Later l;
    l.due = GetTickCount64() + minutes * 60000ull;
    l.repeatMin = repeat ? minutes : 0;
    l.mode = mode_;
    l.kind = infoKind_;
    l.title = title_;
    l.sub = sub_;
    l.subHidden = subHidden_;
    l.more = more_;
    l.yes = yesLabel;
    l.no = noLabel;
    l.onYes = onYes;
    l.onNo = onNo;
    l.hold = holdUsed_;
    l.glyph = glyph_;
    l.meter = meter_;
    Enqueue(std::move(l));
}

void BoostCard::MenuMuteKind() {
    if (infoKind_ == L"-") return;
    cardlog::Mute(infoKind_, true);
    asked_ = info_ = false;
    repeatMin_ = 0;
    std::wstring k = infoKind_;
    later_.erase(std::remove_if(later_.begin(), later_.end(), [&](const Later& o) { return o.kind == k; }), later_.end());
}

// Пора показать отложенное. Подождёт, пока эта карточка занята другим, и не лезет в игровой режим.
void BoostCard::Idle(ULONGLONG now) {
    if (later_.empty() || Visible() || fx::Game()) return;
    for (size_t i = 0; i < later_.size(); ++i) {
        if (now < later_[i].due) continue;
        Later l = std::move(later_[i]);
        later_.erase(later_.begin() + (long)i);
        yesLabel = l.yes;
        noLabel = l.no;
        onYes = l.onYes;
        onNo = l.onNo;
        kind = l.kind;
        more = l.more;
        meter = l.meter;
        refiring_ = true;
        bool shown = Show(l.mode, l.title, l.sub, theme_, l.hold, l.glyph);
        refiring_ = false;
        if (shown) {
            subHidden_ = l.subHidden;
            repeatMin_ = l.repeatMin;
            if (repeatMin_) cur_ = std::move(l);
        }
        return;
    }
}

void BoostCard::Closed() {
    if (repeatMin_ > 0) {
        // ушла сама - вернётся через свой срок и не считается «не читают»; убрали руками - повтору конец
        if (!ClosedByHand()) {
            asked_ = info_ = false;
            cur_.due = GetTickCount64() + repeatMin_ * 60000ull;
            cur_.repeatMin = repeatMin_;
            Enqueue(cur_);
        }
        repeatMin_ = 0;
    }
    Unanswered();
    InfoGone();
}

bool BoostCard::Show(Mode mode, const std::wstring& title, const std::wstring& sub, const ThemeInfo& theme, int holdMs,
                     Glyph glyph) {
    if (repeatMin_ > 0 && !refiring_) {  // на месте повторяющегося появляется другое: то вернётся в свой срок
        asked_ = info_ = false;
        cur_.due = GetTickCount64() + repeatMin_ * 60000ull;
        cur_.repeatMin = repeatMin_;
        Enqueue(cur_);
        repeatMin_ = 0;
    }
    std::wstring k = kind.empty() ? cardlog::KindOf(title) : kind;
    kind.clear();
    std::wstring moreText = std::move(more);
    more.clear();
    // прежний вопрос сменился другим, так и не отвеченный (тот же вид - просто обновление)
    if (Visible() && k != askKind_) Unanswered();
    // итог того, что только что делалось в этой карточке или нажали секунды назад, - не новость
    bool result = (Visible() && mode_ == WORKING) || GetTickCount64() - g_lastClick < 3000;
    if (Visible() && k != infoKind_) InfoGone();
    bool info = mode == DONE && !result && k != L"-";
    if (info && cardlog::Muted(k)) {  // заглушённое сообщение: не выезжает, только в вечерний дайджест
        cardlog::ToDigest(title, sub);
        return false;
    }
    if (info && k != infoKind_) seen_ = false;  // то же сообщение обновилось - прочитанное остаётся прочитанным
    info_ = info;
    infoKind_ = k;
    bool ask = (mode == OFFER || mode == RESTORE) && onYes && k != L"-";
    if (ask && cardlog::Muted(k)) {  // заглушённый вид (Л11): не выезжает - в пропущенные и в дайджест
        cardlog::Missed m;
        m.kind = k;
        m.title = title;
        m.sub = Pretty(sub);
        Mode was = mode_;  // подписи по умолчанию зависят от вида; открытая карточка остаётся как была
        mode_ = mode;
        m.yes = Label(true);
        m.no = Label(false);
        mode_ = was;
        m.onYes = onYes;
        m.onNo = onNo;
        cardlog::Record(std::move(m));
        cardlog::ToDigest(title, sub);
        return false;
    }
    asked_ = ask;
    askKind_ = k;
    calmUpdate_ = Visible() && mode == WORKING && mode_ == WORKING;  // ход работы обновляется часто
    mode_ = mode;
    meter_ = meter;
    meter = -1;
    if (ask) {
        askYes_ = Label(true), askNo_ = Label(false);
        askOnYes_ = onYes, askOnNo_ = onNo;
    }
    if (mode != WORKING) onStop = nullptr, progress = -1;
    // то же «Подробнее» при обновлении (ход работы над тем же вопросом) остаётся раскрытым
    std::wstring masked = share::Mask(moreText);
    if (masked != more_) moreOpen_ = false;
    more_ = masked;
    title_ = share::Mask(title);  // в показе экрана - без путей с именем пользователя и адресов
    sub_ = share::Mask(Pretty(sub));
    subHidden_.clear();
    if (!secret.empty() && sub.find(secret) != std::wstring::npos) {
        std::wstring h = sub;
        for (size_t at; (at = h.find(secret)) != std::wstring::npos;) h.replace(at, secret.size(), L"••••••");
        subHidden_ = share::Mask(Pretty(h));
    }
    secret.clear();
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
    holdUsed_ = hold;
    Open(hold);
    return Visible();
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

// Главная кнопка говорит тем же цветом, что и знак: у ошибки - красная, у предупреждения - янтарная
Color BoostCard::ButtonTone() const { return glyph_ == G_FAIL || glyph_ == G_WARN ? Tone() : ToColor(theme_.ac); }

// Предупреждение дышит, а шкала колышется: достаточно кадра в 33 мс
bool BoostCard::Animate(double) {
    if (!Visible() || fx::Game() || (glyph_ != G_WARN && meter_ < 0)) return false;
    ULONGLONG now = GetTickCount64();
    if (now - lastPulse_ < 33) return false;
    lastPulse_ = now;
    return true;
}

// Жидкая шкала: наполняется при появлении, на конце - колышущаяся капля
void BoostCard::DrawLevel(Graphics& g, const RectF& track, float level) const {
    float rad = track.Height / 2;
    GraphicsPath tp;
    RoundRectPath(tp, track, rad);
    SolidBrush back(Color(30, 255, 255, 255));
    g.FillPath(&back, &tp);
    float w = track.Width * std::clamp(level, 0.0f, 1.0f);
    if (w <= 0) return;
    Color tone = Tone();
    GraphicsState st = g.Save();
    g.SetClip(&tp, CombineModeIntersect);
    LinearGradientBrush fill(PointF(track.X - 1, 0), PointF(track.X + std::max(w, 2.0f) + 1, 0), WithAlpha(tone, 170), tone);
    g.FillRectangle(&fill, RectF(track.X, track.Y, w, track.Height));
    float wave = 1 + 0.35f * (float)std::sin(GetTickCount64() / 190.0);
    float hr = rad * 1.5f;
    SolidBrush head(MixColor(tone, Color(255, 255, 255), 0.35));
    g.FillEllipse(&head, track.X + w - hr, track.Y + track.Height / 2 - hr * wave, hr * 2, hr * 2 * wave);
    g.Restore(st);
}

std::wstring BoostCard::Label(bool yes) const {
    if (mode_ == WORKING) return L"Стоп";
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
    moreH_ = 0;
    if (!more_.empty() && moreOpen_) {
        Font fM(&fam, 11.5f * K, FontStyleRegular, UnitPixel);
        moreH_ = std::min(TextHeight(g, more_, fM, tw), 22 * 16.0f * K);  // до 22 строк
        h += 10 + moreH_ / K;
    }
    float linkW = 0;
    if (!more_.empty()) {
        RectF m;
        g.MeasureString(moreOpen_ ? L"Скрыть" : L"Подробнее", -1, &fS, PointF(0, 0), &m);
        linkW = m.Width / K + 22;
    }
    if (mode_ == WORKING && progress >= 0) h += 14;  // полоска хода
    if (meter_ >= 0) h += 16;                        // шкала уровня
    if (Buttons()) {
        float bh = 32, top = std::max(h + 12, 52.0f);
        float bw = ButtonWidth(g, Label(true)) / K, nw = ButtonWidth(g, Label(false)) / K;
        RectF body = Body();
        float right = body.GetRight() - kPad * K;
        no_ = RectF(right - nw * K, body.Y + top * K, nw * K, bh * K);
        yes_ = StopButton() ? RectF() : RectF(no_.X - 8 * K - bw * K, no_.Y, bw * K, bh * K);
        h = top + bh;
        link_ = RectF();
        if (linkW > 0) {
            float left = (StopButton() ? no_.X : yes_.X) - 8 * K, x = body.X + (kText - 8) * K;
            if (x + linkW * K <= left) {  // слева от кнопок
                link_ = RectF(x, no_.Y, linkW * K, bh * K);
            } else {  // не влезла - строкой под кнопками
                link_ = RectF(x, body.Y + (h + 6) * K, linkW * K, 26 * K);
                h += 32;
            }
        }
    } else {
        yes_ = no_ = RectF();
        RectF body = Body();
        link_ = linkW > 0 ? RectF(body.X + (kText - 8) * K, body.Y + (h + 6) * K, linkW * K, 26 * K) : RectF();
        if (linkW > 0) h += 32;
    }
    return std::max(h + kPad + 2, 70.0f);
}

void BoostCard::Paint(Graphics& g, const RectF& body) {
    if (Hovered()) seen_ = true;  // мышь подходила - сообщение читали
    float K = k();
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), tone = Tone();
    // знак реагирует на важность: выпрыгивает, от него расходится круг; ошибка вздрагивает, предупреждение дышит
    float gx = body.X + 35 * K, gy = body.Y + 35 * K;
    float rp = Reveal(0.08f, 0.8f);
    if (rp > 0 && rp < 1) {
        float rr = (17 + 24 * (float)EaseOut(rp)) * K;
        Pen ring(WithAlpha(tone, (BYTE)(170 * (1 - rp))), 1.6f * K);
        g.DrawEllipse(&ring, gx - rr, gy - rr, rr * 2, rr * 2);
    }
    if (glyph_ == G_WARN && rp >= 1) {
        float t = (GetTickCount64() % 1800) / 1800.0f, rr = (17 + 10 * t) * K;
        Pen ring(WithAlpha(tone, (BYTE)(110 * (1 - t))), 1.4f * K);
        g.DrawEllipse(&ring, gx - rr, gy - rr, rr * 2, rr * 2);
    }
    float shake = Reveal(0.12f, 0.45f);
    float dx = glyph_ == G_FAIL && shake > 0 && shake < 1 ? (float)std::sin(shake * 28) * (1 - shake) * 4.5f * K : 0;
    float pop = 0.4f + 0.6f * (float)EaseOutBack(Reveal(0, 0.5f), 2.0);
    {
        GraphicsState st = g.Save();
        g.TranslateTransform(gx + dx, gy);
        g.ScaleTransform(pop, pop);
        g.TranslateTransform(-gx, -gy);
        DrawFaded(g, RectF(gx - 20 * K, gy - 20 * K, 40 * K, 40 * K), Reveal(0, 0.12f),
                  [&](Graphics& gg) { DrawGlyph(gg, gx, gy, glyph_, tone); });
        g.Restore(st);
    }

    float x0 = body.X + kText * K, tw = body.GetRight() - kPad * K - x0;
    Font fT(Family(), 14.5f * K, FontStyleRegular, UnitPixel);
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12.5f * K, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tB(fg), mB(MixColor(muted, fg, 0.25));
    // содержимое въезжает справа, одно за другим
    auto rise = [&](float delay, const RectF& area, const std::function<void(Graphics&)>& paint) {
        GraphicsState st = g.Save();
        g.TranslateTransform(16 * K * (1 - (float)EaseOutBack(Reveal(delay, 0.5f), 1.3)), 0);
        DrawFaded(g, area, Reveal(delay, 0.2f), paint);
        g.Restore(st);
    };
    // повторяющееся сообщение говорит об этом рядом с заголовком
    float titleW = tw - 56 * K;
    if (repeatMin_ > 0) {
        std::wstring every = repeatMin_ == 60 ? std::wstring(L"каждый час") : L"каждые " + std::to_wstring(repeatMin_) + L" мин";
        Font fR(&fam, 10.5f * K, FontStyleRegular, UnitPixel);
        RectF m;
        g.MeasureString(every.c_str(), -1, &fR, PointF(0, 0), &m);
        RectF er(x0 + titleW - m.Width, body.Y + kPad * K - 1 * K, m.Width + 2 * K, 22 * K);
        SolidBrush eb(tone);
        g.DrawString(every.c_str(), -1, &fR, er, &sf, &eb);
        titleW -= m.Width + 6 * K;
    }
    RectF titleR(x0, body.Y + kPad * K - 1 * K, titleW, 22 * K);
    rise(0.08f, titleR, [&](Graphics& gt) { gt.DrawString(title_.c_str(), -1, &fT, titleR, &sf, &tB); });
    if (!sub_.empty()) {
        RectF subR(x0, body.Y + (kPad + 25) * K, tw, subH_ + 1);
        const std::wstring& shown = subHidden_.empty() || Hovered() ? sub_ : subHidden_;
        rise(0.14f, subR, [&](Graphics& gt) {
            StringFormat wf;
            wf.SetTrimming(StringTrimmingEllipsisWord);
            gt.DrawString(shown.c_str(), -1, &fS, subR, &wf, &mB);
        });
    }
    if (!more_.empty() && moreOpen_ && moreH_ > 0) {  // «Подробнее»: черта и объяснение под текстом
        float y = body.Y + (kPad + 25 + 8) * K + subH_;
        SolidBrush line(WithAlpha(muted, 60));
        g.FillRectangle(&line, RectF(x0, y - 4 * K, tw, std::max(1.0f, K)));
        Font fM(&fam, 11.5f * K, FontStyleRegular, UnitPixel);
        StringFormat wf;
        wf.SetTrimming(StringTrimmingEllipsisWord);
        SolidBrush b(MixColor(muted, fg, 0.55));
        g.DrawString(more_.c_str(), -1, &fM, RectF(x0, y, tw, moreH_ + 1), &wf, &b);
    }
    if (link_.Width > 0) {  // ссылка «Подробнее» / «Скрыть» с галочкой
        float hv = HoverOf(H_MORE);
        Color ac = ToColor(theme_.ac);
        if (hv > 0.01f) {
            GraphicsPath lp;
            RoundRectPath(lp, link_, 8 * K);
            SolidBrush hb(WithAlpha(ac, (BYTE)(34 * hv)));
            g.FillPath(&hb, &lp);
        }
        StringFormat lf;
        lf.SetLineAlignment(StringAlignmentCenter);
        lf.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush lb(MixColor(ac, Color(255, 255, 255), 0.25 * hv));
        g.DrawString(moreOpen_ ? L"Скрыть" : L"Подробнее", -1, &fS,
                     RectF(link_.X + 8 * K, link_.Y, link_.Width, link_.Height), &lf, &lb);
        GraphicsState st = g.Save();
        g.TranslateTransform(link_.GetRight() - 10 * K, link_.Y + link_.Height / 2);
        g.RotateTransform(moreOpen_ ? 180.0f : 0.0f);
        Pen pen(ac, 1.5f * K);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        PointF pts[3] = {{-3.5f * K, -1.6f * K}, {0, 2.0f * K}, {3.5f * K, -1.6f * K}};
        g.DrawLines(&pen, pts, 3);
        g.Restore(st);
    }
    if (mode_ == WORKING && progress >= 0) {  // полоска хода под текстом
        float y = body.Y + (kPad + 25 + 6) * K + subH_ + (moreH_ > 0 ? moreH_ + 10 * K : 0);
        DrawLevel(g, RectF(x0, y, tw, 5 * K), progress);
    }
    if (meter_ >= 0) {  // шкала наливается при появлении
        float y = body.Y + (kPad + 25 + 6) * K + subH_ + (moreH_ > 0 ? moreH_ + 10 * K : 0);
        float fillT = Reveal(0.2f, 0.8f), e = fillT * fillT * (3 - 2 * fillT);
        DrawLevel(g, RectF(x0, y, tw, 7 * K), meter_ * e);
    }
    if (Buttons()) {
        RectF btns = no_;
        if (!StopButton()) RectF::Union(btns, yes_, no_);
        rise(0.22f, Grow(btns, 3 * K), [&](Graphics& gb) {
            if (!StopButton()) DrawButton(gb, yes_, Label(true), true, H_YES);
            DrawButton(gb, no_, Label(false), false, H_NO);
        });
    }
}

int BoostCard::HitContent(const PointF& p) const {
    if (link_.Width > 0 && link_.Contains(p)) return H_MORE;
    if (!Buttons()) return H_NONE;
    if (!StopButton() && yes_.Contains(p)) return H_YES;
    if (no_.Contains(p)) return H_NO;
    return H_NONE;
}

void BoostCard::Click(int id) {
    seen_ = true;
    g_lastClick = GetTickCount64();
    if (id == H_MORE) {
        moreOpen_ = !moreOpen_;
        Invalidate();  // высота пересчитается
        return;
    }
    if (id == H_YES || id == H_NO) repeatMin_ = 0;  // ответили кнопкой - повторять больше незачем
    if ((id == H_YES || (id == H_NO && !StopButton())) && asked_) {
        // ответили: «да» или вторая кнопка со своим действием - это действие, «Не сейчас» без него - нет
        if (id == H_YES || onNo) {
            asked_ = false;
            cardlog::Closed(askKind_, true);
        } else {
            Unanswered();
        }
    }
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
    if (id == H_NO && StopButton()) {  // «Стоп»: карточка остаётся, работа сама скажет, что остановилась
        auto f = std::move(onStop);
        onStop = nullptr;
        f();
        Invalidate();  // кнопка ушла - высота пересчитается
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

void BoostCard::DemoRightClick() { SendMessageW(Wnd(), WM_RBUTTONUP, 0, 0); }

void BoostCard::DemoClickMenu(int row, int index) {
    RectF r = MenuRect((row ? H_M_REPEAT : H_M_LATER) + index);
    LPARAM lp = MAKELPARAM((int)(r.X + r.Width / 2), (int)(r.Y + r.Height / 2));
    SendMessageW(Wnd(), WM_LBUTTONDOWN, MK_LBUTTON, lp);
    SendMessageW(Wnd(), WM_LBUTTONUP, 0, lp);
}

void BoostCard::DemoClickYes() {
    LPARAM lp = MAKELPARAM((int)(yes_.X + yes_.Width / 2), (int)(yes_.Y + yes_.Height / 2));
    SendMessageW(Wnd(), WM_LBUTTONDOWN, MK_LBUTTON, lp);
    SendMessageW(Wnd(), WM_LBUTTONUP, 0, lp);
}

void BoostCard::DemoFastForward() {
    for (Later& l : later_) l.due = 0;
}
