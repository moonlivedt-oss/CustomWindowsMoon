#pragma once
#include <algorithm>
#include "view/geometry.h"

namespace view {
// Shared by the painted search field and its native edit, in device-independent pixels.
struct HomeLayout {
    float sidebar = 200;
    float bottom, mainX, mainW, end;
    bool queue;
    Rect search, searchText, trackText, transport, seek, volume;
    explicit HomeLayout(Size size, bool collection = true) {
        bottom = size.h - 112;
        queue = collection && size.w >= 1180;
        mainX = sidebar + 28;
        end = size.w - (queue ? 284.f : 28.f);
        mainW = end - mainX;
        search = {mainX, 58, mainW, 44};
        searchText = {mainX + 42, 69, mainW - 60, 22};
        // Кнопки и полоса времени - по центру окна; ширина полосы ограничена так, чтобы слева
        // оставалось место названию трека, а справа - громкости
        const float center = size.w * .5f;
        const float half = std::clamp(std::min(center - 300.f, size.w - 230.f - center) - 42.f, 120.f, 520.f);
        transport = {center - 126, bottom + 12, 252, 44};
        seek = {center - half, bottom + 72, half * 2, 20};
        trackText = {94, bottom + 30, std::max(120.f, seek.x - 42 - 94 - 20), 48};
        volume = {size.w - 116, bottom + 45, 88, 22};
    }
};
}  // namespace view
