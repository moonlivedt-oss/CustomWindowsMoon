#pragma once
// Общее для картинки блокировки, перехода и портала (lockscreen.cpp, lock_reveal.cpp, lock_portal.cpp).
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lockscreen::detail {

double InOut(double t);  // плавно в начале и в конце
RECT PrimaryRect();      // основной монитор целиком
// Картинка, растянутая с обрезкой на весь прямоугольник w x h; пиксели BGRA, непрозрачные.
bool LoadCover(const std::wstring& file, int w, int h, std::vector<uint32_t>& px);

}  // namespace lockscreen::detail
