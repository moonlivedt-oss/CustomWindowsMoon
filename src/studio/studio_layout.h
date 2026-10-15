#pragma once
// Раскладка страниц настроек из карточек («Поведение», «Система», «Красота»).
#include <algorithm>
#include <cmath>
#include <map>
#include <string>

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
        y0_ = colY_[0] = colY_[1] = view.Y - off;
    }

    float RowH() const { return rowH_; }
    float X() const { return x_; }
    float Top() const { return y0_; }
    float Width() const { return w_; }
    float Bottom() const { return std::max(colY_[0], colY_[1]); }
    float Height() const { return std::max(colY_[0], colY_[1]) - y0_; }  // для прокрутки

    // Карточка с заголовком; body получает прямоугольник под rows строк.
    template <class Body>
    void Card(const wchar_t* title, float rows, Body&& body) {
        const UiColors& c = ui_.C();
        int col = two_ && colY_[1] < colY_[0] ? 1 : 0;
        float base = rows * rowH_;
        float& extra = grow_[title];
        RectF cr(x_ + col * (cw_ + gap_), colY_[col], cw_, ui_.S(52) + base + extra);
        ui_.Fill(cr, c.card, ui_.S(12));
        ui_.Stroke(cr, c.border, ui_.S(12));
        ui_.Text(title, RectF(cr.X + ui_.S(18), cr.Y + ui_.S(14), cr.Width, ui_.S(24)), c.text, 14, true);
        RectF b(cr.X + ui_.S(18), cr.Y + ui_.S(48), cr.Width - ui_.S(36), base);
        need_ = b.Y + base;
        body(b);
        float want = std::max(0.0f, need_ - (b.Y + base));
        if (std::fabs(want - extra) > 0.5f) {
            extra = want;
            ui_.Invalidate();
        }
        colY_[col] += cr.Height + gap_;
    }

    // Карточка на всю ширину (длинные списки): встаёт под обе колонки.
    template <class Body>
    void Wide(const wchar_t* title, float rows, Body&& body) {
        const UiColors& c = ui_.C();
        float y = std::max(colY_[0], colY_[1]);
        float base = rows * rowH_;
        float& extra = grow_[title];
        RectF cr(x_, y, w_, ui_.S(52) + base + extra);
        ui_.Fill(cr, c.card, ui_.S(12));
        ui_.Stroke(cr, c.border, ui_.S(12));
        ui_.Text(title, RectF(cr.X + ui_.S(18), cr.Y + ui_.S(14), cr.Width, ui_.S(24)), c.text, 14, true);
        RectF b(cr.X + ui_.S(18), cr.Y + ui_.S(48), cr.Width - ui_.S(36), base);
        need_ = b.Y + base;
        body(b);
        float want = std::max(0.0f, need_ - (b.Y + base));
        if (std::fabs(want - extra) > 0.5f) {
            extra = want;
            ui_.Invalidate();
        }
        colY_[0] = colY_[1] = cr.Y + cr.Height + gap_;
    }

    // Строка с подписью слева; справа - место под переключатель или кнопку.
    RectF Row(const RectF& b, float n, const std::wstring& label, float labelShare = 0.6f) {
        RectF rr(b.X, b.Y + n * rowH_, b.Width, ui_.S(44));
        ui_.Text(label, RectF(rr.X, rr.Y, rr.Width * labelShare, rr.Height), ui_.C().text, 12.5f);
        return rr;
    }

    // Пояснение с переносом строк с n-й строки карточки, высотой по самому тексту.
    void Note(const RectF& b, float n, const std::wstring& s, Gdiplus::Color col) {
        float y = b.Y + n * rowH_;
        float h = ui_.MeasureWrapped(s, 11.5f, false, b.Width) + ui_.S(4);
        ui_.Text(s, RectF(b.X, y, b.Width, h), col, 11.5f, false, ui::Left, true);
        need_ = std::max(need_, y + h + ui_.S(6));
    }

private:
    ui::Ui& ui_;
    std::map<std::wstring, float>& grow_;
    float rowH_, x_ = 0, w_ = 0, gap_ = 0, cw_ = 0, y0_ = 0, need_ = 0;
    float colY_[2] = {0, 0};
    bool two_ = false;
};

}  // namespace studio
