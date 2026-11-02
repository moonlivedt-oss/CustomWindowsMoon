#pragma once
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
        const float controlsLeft = std::max(320.f, size.w * .34f);
        const float controlsRight = size.w - 200;
        const float center = (controlsLeft + controlsRight) * .5f;
        transport = {center - 126, bottom + 12, 252, 44};
        seek = {controlsLeft + 42, bottom + 72, controlsRight - controlsLeft - 84, 20};
        trackText = {94, bottom + 30, controlsLeft - 118, 48};
        volume = {size.w - 116, bottom + 45, 88, 22};
    }
};
}  // namespace view
