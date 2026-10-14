#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// Сотовое стекло: общая геометрия сот для портала, углов, кольца, заряда и игрового режима.
// Соты с плоским верхом, s - радиус от середины до угла.
namespace hex {

struct Cell {
    int q = 0, r = 0;     // осевые координаты соты
    double x = 0, y = 0;  // её середина
    double m = 0;         // где точка внутри: 0 - середина, 1 - грань
};

Cell At(double px, double py, double s);
Cell Center(int q, int r, double s);  // сота по координатам (m = 0)
// Шестиугольник соты; inset < 1 - уменьшенный к середине (зазор между сотами).
void Path(Gdiplus::GraphicsPath& p, double x, double y, double s, double inset = 1.0);
// Все соты, чьи середины попадают в прямоугольник (с запасом в одну соту): вызывает f(Cell).
template <class F>
void ForEach(double x0, double y0, double x1, double y1, double s, F&& f) {
    const double w = s * 1.5, h = s * 1.7320508075688772;
    int q0 = (int)(x0 / w) - 1, q1 = (int)(x1 / w) + 1;
    for (int q = q0; q <= q1; ++q) {
        double off = q / 2.0;
        int r0 = (int)(y0 / h - off) - 1, r1 = (int)(y1 / h - off) + 1;
        for (int r = r0; r <= r1; ++r) f(Center(q, r, s));
    }
}

}  // namespace hex
