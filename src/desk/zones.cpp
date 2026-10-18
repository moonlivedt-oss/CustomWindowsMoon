#include "desk/zones.h"

#include <algorithm>

namespace zones {

namespace {

// доли рабочей области: x, y, ширина, высота в долях от 0 до 1
struct Frac {
    double x, y, w, h;
};

RECT Place(const RECT& work, Frac f, int gap) {
    double W = work.right - work.left, H = work.bottom - work.top;
    // зазор: половина между соседями, целый - у краёв
    auto edge = [&](double v, bool startEdge) {
        bool atEdge = startEdge ? v <= 1e-6 : v >= 1 - 1e-6;
        return atEdge ? gap : gap / 2;
    };
    RECT r;
    r.left = work.left + (LONG)(f.x * W + 0.5) + edge(f.x, true);
    r.top = work.top + (LONG)(f.y * H + 0.5) + edge(f.y, true);
    r.right = work.left + (LONG)((f.x + f.w) * W + 0.5) - edge(f.x + f.w, false);
    r.bottom = work.top + (LONG)((f.y + f.h) * H + 0.5) - edge(f.y + f.h, false);
    return r;
}

struct OneDef {
    const wchar_t* key;
    Frac f;
};
const OneDef kOne[] = {
    {L"z:left", {0, 0, 0.5, 1}},
    {L"z:right", {0.5, 0, 0.5, 1}},
    {L"z:top", {0, 0, 1, 0.5}},
    {L"z:bottom", {0, 0.5, 1, 0.5}},
    {L"z:left3", {0, 0, 1.0 / 3, 1}},
    {L"z:mid3", {1.0 / 3, 0, 1.0 / 3, 1}},
    {L"z:right3", {2.0 / 3, 0, 1.0 / 3, 1}},
    {L"z:left23", {0, 0, 2.0 / 3, 1}},
    {L"z:right23", {1.0 / 3, 0, 2.0 / 3, 1}},
    {L"z:tl", {0, 0, 0.5, 0.5}},
    {L"z:tr", {0.5, 0, 0.5, 0.5}},
    {L"z:bl", {0, 0.5, 0.5, 0.5}},
    {L"z:br", {0.5, 0.5, 0.5, 0.5}},
    {L"z:center", {0.15, 0.1, 0.7, 0.8}},
    {L"z:full", {0, 0, 1, 1}},
};

struct LayDef {
    const wchar_t* key;
    std::vector<Frac> f;
};
const std::vector<LayDef>& Lays() {
    static const std::vector<LayDef> v = {
        {L"l:2", {{0, 0, 0.5, 1}, {0.5, 0, 0.5, 1}}},
        {L"l:3", {{0, 0, 1.0 / 3, 1}, {1.0 / 3, 0, 1.0 / 3, 1}, {2.0 / 3, 0, 1.0 / 3, 1}}},
        {L"l:main2", {{0, 0, 0.6, 1}, {0.6, 0, 0.4, 0.5}, {0.6, 0.5, 0.4, 0.5}}},
        {L"l:4", {{0, 0, 0.5, 0.5}, {0.5, 0, 0.5, 0.5}, {0, 0.5, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}}},
    };
    return v;
}

}  // namespace

const std::vector<Zone>& OneWindow() {
    static const std::vector<Zone> v = {
        {L"z:left", L"Слева", L"Левая половина экрана"},
        {L"z:right", L"Справа", L"Правая половина экрана"},
        {L"z:center", L"Середина", L"Посередине, 70% экрана"},
        {L"z:left3", L"Треть слева", L"Левая треть - для широкого монитора"},
        {L"z:mid3", L"Треть в центре", L"Средняя треть"},
        {L"z:right3", L"Треть справа", L"Правая треть"},
        {L"z:left23", L"Две трети слева", L"Две трети слева, рядом место под ещё одно окно"},
        {L"z:tl", L"Четверть вверху слева", L"Левая верхняя четверть"},
        {L"z:tr", L"Четверть вверху справа", L"Правая верхняя четверть"},
        {L"z:bl", L"Четверть внизу слева", L"Левая нижняя четверть"},
        {L"z:br", L"Четверть внизу справа", L"Правая нижняя четверть"},
        {L"z:top", L"Сверху", L"Верхняя половина (для вертикального монитора)"},
        {L"z:bottom", L"Снизу", L"Нижняя половина"},
    };
    return v;
}

const std::vector<Zone>& Layouts() {
    static const std::vector<Zone> v = {
        {L"l:2", L"Два рядом", L"Два последних окна этого монитора - пополам"},
        {L"l:3", L"Три колонки", L"Три последних окна - колонками"},
        {L"l:main2", L"Главное и два", L"Последнее окно слева на 60%, два других справа друг над другом"},
        {L"l:4", L"Четыре четверти", L"Четыре последних окна - по четвертям"},
    };
    return v;
}

bool Rect(const std::wstring& key, const RECT& work, int gap, RECT& out) {
    for (const OneDef& d : kOne)
        if (key == d.key) {
            out = Place(work, d.f, gap);
            return true;
        }
    return false;
}

std::vector<RECT> Layout(const std::wstring& key, const RECT& work, int gap) {
    std::vector<RECT> out;
    for (const LayDef& d : Lays())
        if (key == d.key)
            for (const Frac& f : d.f) out.push_back(Place(work, f, gap));
    return out;
}

void ResizeEdges(const RECT& win, POINT at, int& ex, int& ey) {
    LONG w = std::max<LONG>(1, win.right - win.left), h = std::max<LONG>(1, win.bottom - win.top);
    ex = (int)std::clamp((at.x - win.left) * 3 / w, 0L, 2L);
    ey = (int)std::clamp((at.y - win.top) * 3 / h, 0L, 2L);
    if (ex == 1 && ey == 1) ex = ey = 2;
}

RECT Resized(const RECT& s, int ex, int ey, int dx, int dy, int minW, int minH) {
    RECT r = s;
    if (ex == 0) r.left = std::min<LONG>(s.left + dx, s.right - minW);
    if (ex == 2) r.right = std::max<LONG>(s.right + dx, s.left + minW);
    if (ey == 0) r.top = std::min<LONG>(s.top + dy, s.bottom - minH);
    if (ey == 2) r.bottom = std::max<LONG>(s.bottom + dy, s.top + minH);
    return r;
}

}  // namespace zones
