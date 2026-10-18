#pragma once
// Росчерк (marking menu): зажали Alt у края и сразу махнули мышью - это не «посмотреть», а «выбрать». Пока Alt
// держат, кольцо выбирает сектор по направлению, не дожидаясь отрисовки; отпустили Alt над пунктом - он
// открывается. Вложенные кольца - ломаной: первый отрезок выбирает сектор, поворот уводит во второе кольцо.
// Здесь только распознавание движения (без окон, под тестами).
#include <vector>

namespace stroke {

struct Tracker {
    void Start(unsigned long long t, double x, double y);
    void Feed(unsigned long long t, double x, double y);
    // Махнули: за первые kWindowMs ушли от начала дальше minPx (масштаб уже учтён) со скоростью не меньше
    // kMinSpeed пикселей в мс. Медленный подход к кольцу росчерком не считается.
    bool Flick(double minPx) const;
    int Turns() const;     // сколько раз ломаная повернула больше чем на kTurnDeg (отрезки от kSegPx)
    double Angle() const;  // направление первого отрезка, градусы (0 - вправо, 90 - вниз, как у экрана)

    static constexpr unsigned long long kWindowMs = 350;
    static constexpr double kMinSpeed = 0.35, kTurnDeg = 35, kSegPx = 24;

private:
    struct P {
        unsigned long long t;
        double x, y;
    };
    std::vector<P> pts_;
};

}  // namespace stroke
