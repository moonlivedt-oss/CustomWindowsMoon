// «Телефон рядом»: карточка справа снизу с QR-ссылкой. «Другая сеть» - список путей, если телефон не в этой сети.
#include <algorithm>

#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/qr.h"
#include "phone/phone_internal.h"

using namespace Gdiplus;

namespace phone::detail {

namespace {

constexpr float kPad = 16, kQr = 132, kRowH = 46;

struct WayText {
    const wchar_t* title;
    const wchar_t* desc;
};

WayText TextOf(Way w) {
    switch (w) {
        case WAY_PHONE_HOTSPOT:
            return {L"Раздать Wi-Fi с телефона", L"Точка доступа на телефоне, ноутбук подключится к ней"};
        case WAY_PC_HOTSPOT:
            return {L"Раздать Wi-Fi с ноутбука", L"«Мобильный хот-спот» Windows, телефон подключится к нему"};
        case WAY_BLUETOOTH:
            return {L"Bluetooth", L"Без сети, для мелкого: до 20 МБ"};
    }
    return {L"", L""};
}

}  // namespace

PhoneCard& Card() {
    static PhoneCard c;
    return c;
}

void PhoneCard::Create(HINSTANCE inst) { CreateWnd(inst, L"MoonRadialPhone", L"Телефон"); }

void PhoneCard::Show(const View& v, const ThemeInfo& theme) {
    theme_ = theme;
    v_ = v;
    ways_ = !v.lan;  // общей сети нет - сразу пути
    hideOnly_ = false;
    stopAsked = false;
    Open(0);
}

void PhoneCard::Update(const View& v) {
    bool lanCame = v.lan && !v_.lan;
    v_ = v;
    if (lanCame) ways_ = false;  // адрес появился (раздача включилась) - назад к QR
    // страница на связи: карточка может уйти сама через 20 с, сессия останется (пульт работает)
    if (v.pageMode && v.connected && !hideOnly_ && Visible()) {
        hideOnly_ = true;
        Open(20000);
    }
    Invalidate();
}

bool PhoneCard::Busy() const { return !hideOnly_; }

void PhoneCard::Closed() {
    if (!hideOnly_) stopAsked = true;  // крестик или бросок до того, как телефон на связи, - конец передачи
    hideOnly_ = false;
}

Color PhoneCard::Tone() const { return ToColor(theme_.ac); }

std::vector<Way> PhoneCard::Rows() const {
    std::vector<Way> w = phone::Ways(v_.lan, v_.bluetooth && !v_.pageMode, v_.bytes);
    if (w.size() > 4) w.resize(4);
    return w;
}

float PhoneCard::BodyHeight(Graphics& g) {
    float K = k();
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12.5f * K, FontStyleRegular, UnitPixel);
    RectF body(kM * K, kM * K, kW * K, 0);
    float h;
    if (ways_) {
        h = kPad + 24 + (float)Rows().size() * kRowH;
        if (!v_.lan)
            h += 4 + TextHeight(g, L"Общей сети с телефоном нет - выберите путь:", fS, (kW - 2 * kPad) * K) / K;
        for (size_t i = 0; i < 4; ++i) rows_[i] = RectF();
        float y = h - (float)Rows().size() * kRowH;
        for (size_t i = 0; i < Rows().size(); ++i)
            rows_[i] = RectF(body.X + 8 * K, body.Y + (y + i * kRowH) * K, (kW - 16) * K, (kRowH - 4) * K);
    } else {
        float tw = (kW - kPad * 3 - kQr) * K;
        float text = 22 + 4 + TextHeight(g, v_.what, fS, tw) / K + 6 + TextHeight(g, v_.status, fS, tw) / K;
        if (!v_.hint.empty()) text += 6 + TextHeight(g, v_.hint, fS, tw) / K;
        if (!v_.fp.empty()) text += 6 + TextHeight(g, v_.fp, fS, tw) / K;
        h = kPad + std::max(kQr, text);
    }
    float bh = 32, top = h + 12;
    std::wstring la = ways_ ? (v_.lan ? L"Назад к QR" : L"Стоп") : L"Другая сеть";
    std::wstring lb = ways_ ? L"" : (v_.pageMode && v_.connected ? L"Стоп" : L"Ссылка");
    float right = body.GetRight() - kPad * K;
    float wa = ButtonWidth(g, la), wb = lb.empty() ? 0 : ButtonWidth(g, lb);
    b_ = lb.empty() ? RectF() : RectF(right - wb, body.Y + top * K, wb, bh * K);
    a_ = RectF((lb.empty() ? right : b_.X - 8 * K) - wa, body.Y + top * K, wa, bh * K);
    return top + bh + kPad;
}

void PhoneCard::DrawQr(Graphics& g, const RectF& box) const {
    SolidBrush white(Color(255, 255, 255, 255));
    GraphicsPath p;
    RoundRectPath(p, box, 10 * k());
    g.FillPath(&white, &p);
    if (v_.url.empty()) return;
    auto* self = const_cast<PhoneCard*>(this);
    std::string u = Narrow(v_.url);
    if (u != qrFor_) {
        qr::Code c = qr::Encode(u);
        self->qrFor_ = u;
        self->qrSize_ = c.size;
        self->qr_ = c.dark;
    }
    if (!qrSize_) return;
    int n = qrSize_ + 4;  // тихая зона по 2 модуля: белая подложка и так отделяет код
    int px = std::max(1, (int)(box.Width / n));
    float side = (float)(px * qrSize_);
    float x0 = std::floor(box.X + (box.Width - side) / 2), y0 = std::floor(box.Y + (box.Height - side) / 2);
    SolidBrush ink(Color(255, 0, 0, 0));
    SmoothingMode was = g.GetSmoothingMode();
    g.SetSmoothingMode(SmoothingModeNone);
    for (int y = 0; y < qrSize_; ++y)
        for (int x = 0; x < qrSize_; ++x)
            if (qr_[(size_t)y * qrSize_ + x]) g.FillRectangle(&ink, x0 + x * px, y0 + y * px, (REAL)px, (REAL)px);
    g.SetSmoothingMode(was);
}

void PhoneCard::Paint(Graphics& g, const RectF& body) {
    float K = k();
    Color fg = ToColor(theme_.fg), muted = ToColor(theme_.muted), ac = ToColor(theme_.ac);
    Font fT(Family(), 14.5f * K, FontStyleRegular, UnitPixel);
    FontFamily fam(L"Segoe UI");
    Font fS(&fam, 12.5f * K, FontStyleRegular, UnitPixel);
    Font fX(&fam, 11.5f * K, FontStyleRegular, UnitPixel);
    SolidBrush tB(fg), mB(MixColor(muted, fg, 0.25)), hB(MixColor(ac, fg, 0.35));
    StringFormat one;
    one.SetFormatFlags(StringFormatFlagsNoWrap);
    one.SetTrimming(StringTrimmingEllipsisCharacter);
    StringFormat wrap;
    wrap.SetTrimming(StringTrimmingEllipsisWord);
    if (ways_) {
        float x = body.X + kPad * K, tw = body.Width - 2 * kPad * K, y = body.Y + kPad * K;
        g.DrawString(L"Телефон не в этой сети?", -1, &fT, RectF(x, y - K, tw - 26 * K, 22 * K), &one, &tB);
        y += 24 * K;
        if (!v_.lan) {
            std::wstring s = L"Общей сети с телефоном нет - выберите путь:";
            float th = TextHeight(g, s, fS, tw);
            g.DrawString(s.c_str(), -1, &fS, RectF(x, y, tw, th + 1), &wrap, &mB);
        }
        std::vector<Way> rows = Rows();
        for (size_t i = 0; i < rows.size(); ++i) {
            RectF r = rows_[i];
            float hv = HoverOf(H_ROW0 + (int)i);
            GraphicsPath p;
            RoundRectPath(p, r, 10 * K);
            SolidBrush bg(WithAlpha(ac, (BYTE)(18 + 40 * hv)));
            g.FillPath(&bg, &p);
            WayText t = TextOf(rows[i]);
            g.DrawString(t.title, -1, &fS, RectF(r.X + 12 * K, r.Y + 4 * K, r.Width - 24 * K, 19 * K), &one, &tB);
            g.DrawString(t.desc, -1, &fX, RectF(r.X + 12 * K, r.Y + 22 * K, r.Width - 24 * K, 17 * K), &one, &mB);
        }
    } else {
        RectF box(body.X + kPad * K, body.Y + kPad * K, kQr * K, kQr * K);
        DrawQr(g, box);
        float x = box.GetRight() + kPad * K, tw = body.GetRight() - kPad * K - x, y = body.Y + kPad * K;
        g.DrawString(v_.title.c_str(), -1, &fT, RectF(x, y - K, tw - 22 * K, 22 * K), &one, &tB);
        y += 26 * K;
        for (const std::wstring* s : {&v_.what, &v_.status, &v_.hint, &v_.fp}) {
            if (s->empty()) continue;
            float th = TextHeight(g, *s, fS, tw);
            g.DrawString(s->c_str(), -1, &fS, RectF(x, y, tw, th + 1), &wrap, s == &v_.hint ? &hB : &mB);
            y += th + 6 * K;
        }
    }
    std::wstring la = ways_ ? (v_.lan ? L"Назад к QR" : L"Стоп") : L"Другая сеть";
    std::wstring lb = ways_ ? L"" : (v_.pageMode && v_.connected ? L"Стоп" : L"Ссылка");
    DrawButton(g, a_, la, false, H_A);
    if (!lb.empty()) DrawButton(g, b_, lb, false, H_B);
}

int PhoneCard::HitContent(const PointF& p) const {
    if (a_.Contains(p)) return H_A;
    if (b_.Width > 0 && b_.Contains(p)) return H_B;
    if (ways_)
        for (int i = 0; i < 4; ++i)
            if (rows_[i].Width > 0 && rows_[i].Contains(p)) return H_ROW0 + i;
    return H_NONE;
}

void PhoneCard::Click(int id) {
    if (id == H_A) {
        if (ways_ && !v_.lan) {
            StopSession(L"остановлено");
            return;
        }
        ways_ = !ways_;
        Invalidate();
    } else if (id == H_B) {
        if (v_.pageMode && v_.connected)
            StopSession(L"остановлено");
        else
            CopyUrl();
    } else if (id >= H_ROW0 && id < H_ROW0 + 4) {
        std::vector<Way> rows = Rows();
        size_t i = (size_t)(id - H_ROW0);
        if (i < rows.size()) RunWay(rows[i]);
    }
}

}  // namespace phone::detail
