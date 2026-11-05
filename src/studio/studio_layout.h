#pragma once
// Раскладка страниц настроек из карточек («Поведение», «Система», «Красота»).
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>

#include "core/profile.h"
#include "studio/studio_help.h"
#include "ui/ui.h"

namespace studio {

// Колонка шириной не больше maxW стоит по центру страницы, на широком окне карточки идут в две колонки.
// Высота карточки = её строки + всё, что не влезло из пояснений: Note меряет текст, и со следующего
// кадра карточка вырастает (или снова сжимается) ровно под него, поэтому длинный текст не обрезается.
class CardColumns {
public:
    using RectF = Gdiplus::RectF;

    CardColumns(ui::Ui& ui, std::map<std::wstring, float>& grow, const RectF& view, float off, float maxW,
                float rowH = 50)
        : ui_(ui), grow_(grow), rowH_(ui.S(rowH)) {
        w_ = std::min(view.Width - ui.S(8), maxW);
        x_ = view.X + (view.Width - w_) / 2;
        gap_ = ui.S(16);
        two_ = w_ > ui.S(820);
        cw_ = two_ ? (w_ - gap_) / 2 : w_;
        y0_ = view.Y - off;
    }

    float RowH() const { return rowH_; }
    float X() const { return x_; }
    float Top() const { return y0_; }
    float Width() const { return w_; }
    float Bottom() const { return y0_ + std::max(colY_[0], colY_[1]); }
    float Height() const { return std::max(colY_[0], colY_[1]); }  // для прокрутки

    // Карточка с заголовком; body получает прямоугольник под rows строк.
    template <class Body>
    void Card(const wchar_t* title, float rows, Body&& body) {
        if (profile::Effective() < profile::CardMin(title)) return;
        const UiColors& c = ui_.C();
        // высоты колонок - от начала страницы: на экранных координатах при почти равных высотах карточка из-за
        // округления перескакивала в другую колонку в зависимости от прокрутки
        int col = two_ && colY_[1] + 0.25f < colY_[0] ? 1 : 0;
        float base = rows * rowH_;
        float& extra = grow_[title];
        RectF cr(x_ + col * (cw_ + gap_), y0_ + colY_[col], cw_, ui_.S(52) + base + extra);
        ui_.MarkCard(cr);
        ui_.TrackHover(cr);
        // край карточки под курсором теплеет, ключ анимации - по заголовку (карточки не имеют своих id)
        float hov = ui_.Anim(0x50000000 | (int)(std::hash<std::wstring>{}(title) & 0x0FFFFFFF), ui_.DetailHot(cr), 12);
        ui_.Fill(cr, c.card, ui_.S(12));
        ui_.Stroke(cr, MixColor(c.border, c.accent, 0.22f * hov), ui_.S(12) );
        ui_.Spotlight(cr, ui_.S(12));
        ui_.Text(title, RectF(cr.X + ui_.S(18), cr.Y + ui_.S(14), cr.Width, ui_.S(24)), c.text, 14, true);
        RectF b(cr.X + ui_.S(18), cr.Y + ui_.S(48), cr.Width - ui_.S(36), base);
        Begin(b, base);
        body(b);
        Finish(title, cr, b, base, extra);
        colY_[col] += cr.Height + gap_;
    }

    // Карточка на всю ширину (длинные списки): встаёт под обе колонки.
    template <class Body>
    void Wide(const wchar_t* title, float rows, Body&& body) {
        if (profile::Effective() < profile::CardMin(title)) return;
        const UiColors& c = ui_.C();
        float y = std::max(colY_[0], colY_[1]);
        float base = rows * rowH_;
        float& extra = grow_[title];
        RectF cr(x_, y0_ + y, w_, ui_.S(52) + base + extra);
        ui_.MarkCard(cr);
        ui_.TrackHover(cr);
        // край карточки под курсором теплеет, ключ анимации - по заголовку (карточки не имеют своих id)
        float hov = ui_.Anim(0x50000000 | (int)(std::hash<std::wstring>{}(title) & 0x0FFFFFFF), ui_.DetailHot(cr), 12);
        ui_.Fill(cr, c.card, ui_.S(12));
        ui_.Stroke(cr, MixColor(c.border, c.accent, 0.22f * hov), ui_.S(12) );
        ui_.Spotlight(cr, ui_.S(12));
        ui_.Text(title, RectF(cr.X + ui_.S(18), cr.Y + ui_.S(14), cr.Width, ui_.S(24)), c.text, 14, true);
        RectF b(cr.X + ui_.S(18), cr.Y + ui_.S(48), cr.Width - ui_.S(36), base);
        Begin(b, base);
        body(b);
        Finish(title, cr, b, base, extra);
        colY_[0] = colY_[1] = y + cr.Height + gap_;
    }

    // Строка с подписью слева; справа - место под переключатель или кнопку.
    RectF Row(const RectF& b, float n, const std::wstring& label, float labelShare = 0.6f) {
        RectF rr(b.X, b.Y + n * rowH_, b.Width, ui_.S(44));
        RectF lr(rr.X, rr.Y, rr.Width * labelShare, rr.Height);
        ui_.RowGlow(RectF(rr.X - ui_.S(8), rr.Y + ui_.S(1), rr.Width + ui_.S(16), ui_.S(42)));
        const Help* h = FindHelp(label);
        // у строки есть подробности - подпись откликается на курсор
        bool hot = h && ui_.DetailHot(lr);
        ui_.Text(label, lr, hot ? ui_.C().accent : ui_.C().text, 12.5f);
        if (h) ui_.Detail(lr, label, h->what, h->links);
        return rr;
    }

    // Пояснение: длинный спокойный текст уходит в карточку подробностей у заголовка («i»), а место под него
    // освобождается. Предупреждения и короткое остаются на месте; состояние, которое меняется, - см. Status.
    void Note(const RectF& b, float n, const std::wstring& s, Gdiplus::Color col) {
        const UiColors& c = ui_.C();
        bool calm = col.GetValue() == c.faint.GetValue() || col.GetValue() == c.muted.GetValue();
        if (calm && s.size() >= 60) {
            float y = b.Y + n * rowH_;
            tuckAt_ = tucked_ ? std::min(tuckAt_, y) : y;
            tucked_ = true;
            info_ += (info_.empty() ? L"" : L"\n") + s;
            return;
        }
        Status(b, n, s, col);
    }

    // Текст, который надо видеть всегда: ход работы, состояние, предупреждение.
    void Status(const RectF& b, float n, const std::wstring& s, Gdiplus::Color col) {
        float y = b.Y + n * rowH_;
        float h = ui_.MeasureWrapped(s, 11.5f, false, b.Width) + ui_.S(4);
        ui_.Text(s, RectF(b.X, y, b.Width, h), col, 11.5f, false, ui::Left, true);
        need_ = std::max(need_, y + h + ui_.S(6));
        used_ = std::max(used_, y + h + ui_.S(6));
    }

private:
    void Begin(const RectF& b, float base) {
        need_ = b.Y + base;
        used_ = b.Y;
        tucked_ = false;
        info_.clear();
    }

    // Высота карточки и подробности у заголовка: свой текст из Note, иначе из общей таблицы.
    void Finish(const wchar_t* title, const RectF& cr, const RectF& b, float base, float& extra) {
        const UiColors& c = ui_.C();
        std::wstring body = info_, links;
        if (const Help* h = FindHelp(title)) {
            if (body.empty()) body = h->what;
            links = h->links;
        }
        if (!body.empty()) {
            float tw = ui_.Measure(title, 14, true), d = ui_.S(15);
            RectF dot(cr.X + ui_.S(18) + tw + ui_.S(8), cr.Y + ui_.S(19), d, d);
            RectF zone(cr.X + ui_.S(10), cr.Y + ui_.S(8), tw + d + ui_.S(30), ui_.S(36));
            bool hot = ui_.DetailHot(zone);
            ui_.Stroke(dot, hot ? c.accent : c.faint, d / 2);
            ui_.Text(L"i", dot, hot ? c.accent : c.faint, 9.5f, true, ui::Center);
            ui_.Detail(zone, title, body, links);
        }
        float end = b.Y + base;
        if (tucked_)  // не по знаку tuckAt_: карточка, прокрученная выше окна, стоит на отрицательных координатах
            end = std::max(tuckAt_ - ui_.S(4), used_);  // ниже убранного пояснения ничего нет: карточка короче
        else
            end = need_;
        float want = end - (b.Y + base);
        if (std::fabs(want - extra) > 0.5f) {
            extra = want;
            ui_.Invalidate();
        }
    }

    ui::Ui& ui_;
    std::map<std::wstring, float>& grow_;
    float rowH_, x_ = 0, w_ = 0, gap_ = 0, cw_ = 0, y0_ = 0, need_ = 0, used_ = 0, tuckAt_ = 0;
    bool tucked_ = false;
    std::wstring info_;
    float colY_[2] = {0, 0};
    bool two_ = false;
};

// Общее для вкладок «Утилит», разнесённых по файлам: колонки карточек, размеры строк, ход фоновой работы.
struct ToolsKit {
    using RectF = Gdiplus::RectF;
    ui::Ui& ui;
    CardColumns& cols;
    RectF view;
    float rowH, lineH;
    bool busy;
    ULONGLONG now;
    std::function<std::wstring()> Log;  // последние строки хода работы
    std::function<void()> Refresh;      // перечитать данные вкладки
    template <class Body>
    void Card(const wchar_t* title, float rows, Body&& body) {
        cols.Card(title, rows, body);
    }
    template <class Body>
    void Wide(const wchar_t* title, float rows, Body&& body) {
        cols.Wide(title, rows, body);
    }
    // строка списка: подсветка при наведении, по клику - действие (если есть)
    bool RowHover(int id, const RectF& rr) {
        bool click = ui.Click(id, rr);
        if (ui.Hot(id)) ui.Fill(rr, WithAlpha(ui.C().cardHi, 200), ui.S(6));
        return click;
    }
    // курсор над строкой: кнопки строки показываются только тогда, иначе список - лес одинаковых кнопок
    bool RowHot(const RectF& rr) const {
        POINT m = ui.Mouse();
        return ui.interactive && m.x >= rr.X && m.x < rr.X + rr.Width && m.y >= rr.Y && m.y < rr.Y + rr.Height;
    }
    void Note(const RectF& b, float n, const std::wstring& s, Gdiplus::Color col) { cols.Note(b, n, s, col); }
    void Status(const RectF& b, float n, const std::wstring& s, Gdiplus::Color col) { cols.Status(b, n, s, col); }
    float RowsFor(size_t n) const { return (float)(n * lineH / rowH); }
};

}  // namespace studio
