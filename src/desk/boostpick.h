#pragma once
#include <functional>
#include <string>
#include <vector>

#include "desk/toast.h"

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

    void Create(HINSTANCE inst);
    void Show(const std::wstring& title, const std::wstring& sub, std::vector<Row> rows, const ThemeInfo& theme);

    static constexpr int kRows = 6;

private:
    enum { H_SEG = H_USER, H_APPLY = H_USER + kRows * 3, H_SKIP };
    float BodyHeight(Gdiplus::Graphics& g) override;
    void Paint(Gdiplus::Graphics& g, const Gdiplus::RectF& body) override;
    int HitContent(const Gdiplus::PointF& p) const override;
    void Click(int id) override;
    double Freed() const;

    std::wstring title_, sub_;
    std::vector<Row> rows_;
    float subH_ = 0, rowsY_ = 0;  // dip от верха карточки
    float slide_[kRows] = {};     // где сейчас плашка выбора (0..2, плавно)
    ULONGLONG lastPaint_ = 0;
    Gdiplus::RectF apply_, skip_, seg_[kRows][3];
};
