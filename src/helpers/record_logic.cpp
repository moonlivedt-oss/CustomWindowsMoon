// Запись области экрана: область и команда ffmpeg (под тестами).
#include <algorithm>

#include "helpers/record.h"

namespace record {

RECT Normalize(POINT a, POINT b) {
    RECT r{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
    LONG w = std::max<LONG>(16, (r.right - r.left) & ~1L), h = std::max<LONG>(16, (r.bottom - r.top) & ~1L);
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

std::wstring GrabArgs(const RECT& r, const std::wstring& out, int maxSec) {
    return L"-hide_banner -y -f gdigrab -framerate 30 -draw_mouse 1 -offset_x " + std::to_wstring(r.left) +
           L" -offset_y " + std::to_wstring(r.top) + L" -video_size " + std::to_wstring(r.right - r.left) + L"x" +
           std::to_wstring(r.bottom - r.top) + L" -i desktop -t " + std::to_wstring(maxSec) +
           L" -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -movflags +faststart \"" + out + L"\"";
}

std::wstring Clock(int sec) {
    wchar_t b[16];
    swprintf(b, 16, L"%d:%02d", sec / 60, sec % 60);
    return b;
}

}  // namespace record
