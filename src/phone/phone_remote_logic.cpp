// «Телефон рядом», пульт: касание по картинке экрана -> точка на мониторе, размер кадра, сочетания клавиш, операции с
// файлами (под тестами).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

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

WORD KeyVk(const std::string& name) {
    static const struct {
        const char* name;
        WORD vk;
    } kKeys[] = {{"enter", VK_RETURN}, {"back", VK_BACK},   {"esc", VK_ESCAPE},  {"tab", VK_TAB},
                 {"left", VK_LEFT},    {"right", VK_RIGHT}, {"up", VK_UP},       {"down", VK_DOWN},
                 {"space", VK_SPACE},  {"del", VK_DELETE},  {"home", VK_HOME},   {"end", VK_END},
                 {"win", VK_LWIN},     {"pgup", VK_PRIOR},  {"pgdn", VK_NEXT},   {"shift", VK_SHIFT},
                 {"ctrl", VK_CONTROL}, {"alt", VK_MENU},    {"caps", VK_CAPITAL}};
    for (const auto& k : kKeys)
        if (name == k.name) return k.vk;
    if (name.size() == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9')))
        return (WORD)toupper((unsigned char)name[0]);  // у букв и цифр код клавиши - сама заглавная буква
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'f') {
        int n = atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12 && std::to_string(n) == name.substr(1)) return (WORD)(VK_F1 + n - 1);
    }
    return 0;
}

bool FsOp(const std::string& op) {
    for (const char* o : {"open", "copy", "move", "rename", "mkdir", "delete"})
        if (op == o) return true;
    return false;
}

}  // namespace phone
