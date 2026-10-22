// «Состояние»: пропущенные карточки (Л10) - вопросы, ушедшие без нажатия, с теми же кнопками; заглушённые виды
// карточек (Л11) - вернуть одной кнопкой. Данные - desk/cardlog.
#include <windows.h>

#include "desk/cardlog.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

// для снимков (RADIAL_DEMO): два пропущенных вопроса без действий
std::vector<cardlog::Missed> DemoMissed() {
    std::vector<cardlog::Missed> v(2);
    v[0].id = 1;
    v[0].title = L"Загрузки: 2,4 ГБ лишнего";
    v[0].sub = L"6 старых установщиков и 2 распакованных архива.";
    v[0].yes = L"В Корзину";
    v[0].no = L"Показать";
    v[0].onNo = [] {};
    v[0].at = {2026, 10, 4, 23, 9, 41};
    v[1].id = 2;
    v[1].title = L"Освободить ~1,2 ГБ перед игрой?";
    v[1].sub = L"Закрыть браузер и Discord, вернуть после игры.";
    v[1].yes = L"Освободить";
    v[1].no = L"Не сейчас";
    v[1].at = {2026, 10, 4, 23, 8, 15};
    return v;
}

}  // namespace

float Studio::MissedCards(float x, float y, float w) {
    const UiColors& c = ui.C();
    if (missedGen_ != cardlog::Gen() || Demo()) {
        missedGen_ = cardlog::Gen();
        missed_ = Demo() ? DemoMissed() : cardlog::List();
        muted_ = Demo() ? std::set<std::wstring>{L"Выход сменился"} : cardlog::MutedKinds();
    }
    if (missed_.empty() && muted_.empty()) return 0;
    float rh = ui.S(46), mh = muted_.empty() ? 0 : ui.S(40);
    float h = ui.S(52) + missed_.size() * rh + mh + ui.S(8);
    RectF cr(x, y, w, h);
    Card(cr);
    ui.Text(missed_.empty() ? L"Карточки" : L"Пропущенные карточки", RectF(x + ui.S(18), y + ui.S(14), w, ui.S(24)),
            c.text, 14, true);
    if (!missed_.empty())
        ui.Text(
            L"ушли без ответа за сутки - кнопки работают и сейчас",
            RectF(x + ui.S(18) + ui.Measure(L"Пропущенные карточки", 14, true) + ui.S(14), y + ui.S(17), w, ui.S(20)),
            c.faint, 11);
    float ry = y + ui.S(50);
    for (size_t i = 0; i < missed_.size(); ++i, ry += rh) {
        const cardlog::Missed& m = missed_[i];
        wchar_t t[16];
        swprintf(t, 16, L"%02d:%02d", m.at.wHour, m.at.wMinute);
        ui.Text(t, RectF(x + ui.S(18), ry + ui.S(2), ui.S(50), ui.S(22)), c.faint, 11.5f);
        float bx = x + w - ui.S(18);
        int id = ID_STATE_MISSED + (int)i * 3;
        // «Убрать», вторая кнопка (если у неё было своё действие), главная
        float fw = ui.S(34);
        if (ui.Button(id + 2, RectF(bx - fw, ry + ui.S(6), fw, ui.S(32)), L"✕", Kind::Ghost)) {
            if (!Demo()) cardlog::Forget(m.id);
        }
        ui.Tip(id + 2, L"Убрать из списка");
        bx -= fw + ui.S(8);
        if (m.onNo) {
            float nw = ui.Measure(m.no, 12, true) + ui.S(32);
            if (ui.Button(id + 1, RectF(bx - nw, ry + ui.S(6), nw, ui.S(32)), m.no, Kind::Secondary) && !Demo())
                cardlog::Press(m.id, false);
            bx -= nw + ui.S(8);
        }
        float yw = ui.Measure(m.yes, 12, true) + ui.S(32);
        if (ui.Button(id, RectF(bx - yw, ry + ui.S(6), yw, ui.S(32)), m.yes, Kind::Primary) && !Demo())
            cardlog::Press(m.id, true);
        bx -= yw + ui.S(12);
        float tx = x + ui.S(72), tw = bx - tx;
        ui.Text(m.title, RectF(tx, ry + ui.S(2), tw, ui.S(22)), c.text, 12, true);
        std::wstring sub = m.sub;
        for (wchar_t& ch : sub)
            if (ch == L'\n') ch = L' ';
        ui.Text(Ellip(sub, 120), RectF(tx, ry + ui.S(23), tw, ui.S(20)), c.muted, 10.5f);
    }
    if (!muted_.empty()) {
        // Л11: виды, что больше не выезжают, - вернуть
        float mx = x + ui.S(18), my = ry + ui.S(6);
        ui.Text(L"Не показываются:", RectF(mx, my + ui.S(6), ui.S(150), ui.S(20)), c.faint, 11);
        mx += ui.Measure(L"Не показываются:", 11) + ui.S(10);
        int i = 0;
        for (const std::wstring& k : muted_) {
            std::wstring label = L"«" + k + L"»  вернуть";
            float kw = ui.Measure(label, 11.5f) + ui.S(24);
            if (mx + kw > x + w - ui.S(18)) break;
            if (ui.Button(ID_STATE_MUTED + i, RectF(mx, my, kw, ui.S(30)), label, Kind::Secondary) && !Demo())
                cardlog::Mute(k, false);
            ui.Tip(ID_STATE_MUTED + i, L"Снова показывать карточки этого вида");
            mx += kw + ui.S(8);
            ++i;
        }
    }
    return h;
}

}  // namespace studio
