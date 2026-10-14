#pragma once

struct Config;

// Ночной режим: с вечернего часа до утреннего стекло карточек темнее, свечение углов
// спокойнее, звуки тише. Состояние общее для всей программы, пересчитывается из опроса.
namespace night {

bool Due(const Config& c);  // по настройкам и часам сейчас ночь
void Set(bool on);
bool On();
// множитель яркости свечений: 1 днём, меньше ночью
inline float Glow() { return On() ? 0.55f : 1.0f; }

}  // namespace night
