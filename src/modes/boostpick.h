#pragma once
#include <functional>
#include <string>
#include <vector>

#include "cards/toast.h"

// Карточка «Освободить память перед игрой?» с выбором по каждой программе: закрыть, усыпить
// (память в подкачку, процессор в экономию - программа остаётся открытой) или оставить как есть.
// Выбор запоминается для игры или редактора: в следующий раз карточка откроется с ним же.
class BoostPick : public Toast {
public:
    struct Row {
        std::wstring title;  // для людей
        std::wstring key;    // имя процесса в нижнем регистре (ключ профиля)
        double mb = 0;
        int act = 0;  // gboost::Act
    };
    std::function<void(const std::vector<Row>&)> onApply;
    std::function<void()> onSkip;  // «Не сейчас», крестик или ушла по таймеру (запуск сборки всё равно идёт)

    void Create(HINSTANCE inst);
    void Show(const std::wstring& title, const std::wstring& sub, std::vector<Row> rows, const ThemeInfo& theme);

    static constexpr int kRows = 6;
    void ExpandRow(int row) {  // раскрыть «Подробнее» строки (снимки --card-snap)
        open_ = row;
        Invalidate();
    }

private:
    enum { H_SEG = H_USER, H_APPLY = H_USER + kRows * 3, H_SKIP, H_NAME };  // H_NAME + строка: «Подробнее»
    float BodyHeight(Gdiplus::Graphics& g) override;
    void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) override;
    int HitContent(const Gdiplus::PointF& p) const override;
    void Click(int id) override;
    void Closed() override;
    double Freed() const;
    float RowY(size_t i) const;      // верх строки в dip: строки под раскрытой опускаются
    std::wstring MoreText() const;  // что станет с программой раскрытой строки при выбранном действии

    std::wstring title_, sub_;
    std::vector<Row> rows_;
    float subH_ = 0, rowsY_ = 0;  // dip от верха карточки
    float slide_[kRows] = {};     // где сейчас плашка выбора (0..2, плавно)
    ULONGLONG lastPaint_ = 0;
    Gdiplus::RectF apply_, skip_, seg_[kRows][3], name_[kRows];
    int open_ = -1;      // строка с раскрытым «Подробнее»; -1 - ни одной
    float moreH_ = 0;    // высота пояснения, px
};
