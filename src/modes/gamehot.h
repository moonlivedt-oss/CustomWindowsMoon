#pragma once
// Температура в игре (Ж2): слежка утилит в игре раз в 10 с меряет видеокарту (nvidia-smi). Горячо (от 85 °C или
// упор в перегрев) 5 минут подряд - во время игры карточка, один раз за игру: «Освободить» (Boost закроет фоновое) и
// «Подробнее» - как остудить: ограничить FPS в игре или в приложении NVIDIA, режим вентиляторов (у ASUS TUF -
// Fn+F5 или Armoury Crate), подставка. Короткие провалы ниже 80 °C сбрасывают отсчёт. Под тестами.
#include <cstdint>

namespace gamehot {

class HotWatch {
public:
    // замер: true - пора сказать (один раз)
    bool Feed(uint64_t nowMs, int temp, bool thermal);
    void Reset() { since_ = 0, said_ = false; }

private:
    uint64_t since_ = 0;
    bool said_ = false;
};

}  // namespace gamehot
