#pragma once
// «Жидкий» индикатор выбора: подложка не прыгает, а перетекает к новой кнопке.
// Здесь только геометрия и время, без рисования: так поведение проверяется тестами без окна.

#include <cstdint>

#include "view/geometry.h"

namespace view {

struct Liquid {
    // Новая длительность анимации не дольше этого: пока она идёт, окну нужны кадры
    static constexpr double kSettleSeconds = .62;

    // Вызывать в каждом кадре, где группа кнопок рисуется. target - где сейчас выбранная кнопка,
    // slot - какая она по счёту, frame - номер кадра. Перетекание запускает только смена slot:
    // кнопка, переехавшая из-за прокрутки или размера окна, тянет каплю за собой целиком.
    Rect Update(Rect target, int slot, double clock, uint64_t frame, bool reducedMotion);
    Rect Now(double clock) const;
    bool Animating(double clock) const { return clock - start_ < kSettleSeconds; }

private:
    Rect from_, to_;
    double start_ = 0;
    uint64_t seenFrame_ = 0;
    int slot_ = -1;
    bool ready_ = false;
};

}  // namespace view
