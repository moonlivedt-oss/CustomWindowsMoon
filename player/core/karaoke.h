#pragma once
// Караоке и доли такта: маленькая переносимая обработка звука, проверяется тестами.
//
// VocalReducer - «минус голос» без нейросетей: голос почти всегда в центре стереопанорамы,
// поэтому из середины (L+R)/2 убирается всё, кроме басов (бочка и бас тоже в центре, их
// оставляем через однополюсный НЧ-фильтр ~160 Гц). Боковая часть (L-R)/2 - инструменты по
// краям - не трогается. amount плавно идёт к цели, чтобы включение не щёлкало.
//
// BeatDetector - доли такта по энергии низких частот: удар, когда энергия заметно выше
// скользящего среднего, не чаще ~4 раз в секунду.

#include <algorithm>
#include <cmath>

namespace pm {

class VocalReducer {
public:
    void SetSampleRate(float rate) {
        const float cutoff = 160.f;
        lowCoef_ = 1.f - std::exp(-2.f * 3.14159265f * cutoff / std::max(8000.f, rate));
        step_ = 1.f / (0.08f * std::max(8000.f, rate));  // 80 мс на переход
    }
    void SetTarget(float amount) { target_ = amount < 0 ? 0 : amount > 1 ? 1 : amount; }
    bool Idle() const { return amount_ == 0 && target_ == 0; }
    void Process(float& l, float& r) {
        if (amount_ != target_)
            amount_ += amount_ < target_ ? std::fmin(step_, target_ - amount_) : -std::fmin(step_, amount_ - target_);
        const float mid = (l + r) * .5f, side = (l - r) * .5f;
        low_ += (mid - low_) * lowCoef_;
        if (amount_ <= 0) return;
        // Середина без голоса: басы остаются, верх середины уходит; сторону чуть подтягиваем, чтобы не провалилась
        // громкость
        const float m = low_ + (mid - low_) * (1 - amount_);
        const float s = side * (1 + .35f * amount_);
        l = m + s;
        r = m - s;
    }

private:
    float target_ = 0, amount_ = 0, low_ = 0, lowCoef_ = .02f, step_ = .0003f;
};

class BeatDetector {
public:
    // energy - уровень низких частот 0..1 в момент now (секунды). true - доля такта
    // follow - скорость среднего за этот кадр (для кадра длиннее одного обновления экрана - больше)
    bool Feed(float energy, double now, float follow = .06f) {
        const float rise = energy - average_;
        average_ += (energy - average_) * follow;
        if (energy > .12f && rise > std::fmax(.06f, average_ * .35f) && now - last_ > .24) {
            last_ = now;
            return true;
        }
        return false;
    }
    double Last() const { return last_; }

private:
    float average_ = 0;
    double last_ = -10;
};

}  // namespace pm
