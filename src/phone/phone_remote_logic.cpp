// «Телефон рядом», пульт: касание по картинке экрана -> точка на мониторе, размер кадра, сочетания клавиш, операции с
// файлами (под тестами).
#include <algorithm>
#include <cmath>

#include "phone/phone.h"

namespace phone {

POINT TapPoint(double nx, double ny, const RECT& mon) {
    nx = std::clamp(nx, 0.0, 1.0), ny = std::clamp(ny, 0.0, 1.0);
    LONG w = mon.right - mon.left, h = mon.bottom - mon.top;
    return {mon.left + std::min<LONG>(w - 1, (LONG)std::lround(nx * w)),
            mon.top + std::min<LONG>(h - 1, (LONG)std::lround(ny * h))};
}

SIZE FrameSize(int w, int h, int maxW) {
    if (w <= 0 || h <= 0) return {0, 0};
    maxW = std::clamp(maxW, 320, 2560);
    if (w <= maxW) return {w, h};
    return {maxW, std::max(1, (int)std::lround((double)h * maxW / w))};
}

std::vector<WORD> Combo(const std::string& name) {
    static const struct {
        const char* name;
        std::vector<WORD> keys;
    } kCombos[] = {{"copy", {VK_CONTROL, 'C'}},
                   {"paste", {VK_CONTROL, 'V'}},
                   {"cut", {VK_CONTROL, 'X'}},
                   {"undo", {VK_CONTROL, 'Z'}},
                   {"all", {VK_CONTROL, 'A'}},
                   {"save", {VK_CONTROL, 'S'}},
                   {"find", {VK_CONTROL, 'F'}},
                   {"newtab", {VK_CONTROL, 'T'}},
                   {"closetab", {VK_CONTROL, 'W'}},
                   {"alttab", {VK_MENU, VK_TAB}},
                   {"win", {VK_LWIN}},
                   {"desktop", {VK_LWIN, 'D'}},
                   {"explorer", {VK_LWIN, 'E'}},
                   {"close", {VK_MENU, VK_F4}},
                   {"refresh", {VK_F5}},
                   {"zoomin", {VK_CONTROL, VK_OEM_PLUS}},
                   {"zoomout", {VK_CONTROL, VK_OEM_MINUS}},
                   {"back", {VK_MENU, VK_LEFT}},
                   {"fwd", {VK_MENU, VK_RIGHT}},
                   {"taskmgr", {VK_CONTROL, VK_SHIFT, VK_ESCAPE}}};
    for (const auto& c : kCombos)
        if (name == c.name) return c.keys;
    return {};
}

bool FsOp(const std::string& op) {
    for (const char* o : {"open", "copy", "move", "rename", "mkdir", "delete"})
        if (op == o) return true;
    return false;
}

}  // namespace phone
