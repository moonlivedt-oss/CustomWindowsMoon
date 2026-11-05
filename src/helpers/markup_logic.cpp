#include "helpers/markup_logic.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/secrets.h"

namespace markup {

bool Meaningful(const Shape& s) {
    if (s.pts.empty()) return false;
    switch (s.tool) {
        case Tool::Label:
            return s.text.find_first_not_of(L" \t") != std::wstring::npos;
        case Tool::Step:
            return true;
        case Tool::Marker:
            return s.pts.size() >= 2;
        default:
            if (s.pts.size() < 2) return false;
            // короче трёх пикселей - это щелчок, а не фигура
            return std::hypot(s.pts[1].x - s.pts[0].x, s.pts[1].y - s.pts[0].y) >= 3;
    }
}

void Doc::Remember() {
    undo_.push_back(now_);
    redo_.clear();  // новая правка отменяет возможность вернуть прежние
}

void Doc::Add(Shape s) {
    Remember();
    now_.shapes.push_back(std::move(s));
}

void Doc::Replace(size_t index, Shape s) {
    if (index >= now_.shapes.size()) return;
    Remember();
    now_.shapes[index] = std::move(s);
}

void Doc::Remove(size_t index) {
    if (index >= now_.shapes.size()) return;
    Remember();
    now_.shapes.erase(now_.shapes.begin() + (std::ptrdiff_t)index);
}

void Doc::SetCrop(Rc crop) {
    Remember();
    now_.crop = crop;
}

void Doc::SetShapes(std::vector<Shape> shapes) {
    Remember();
    now_.shapes = std::move(shapes);
}

bool Doc::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(now_));
    now_ = std::move(undo_.back());
    undo_.pop_back();
    return true;
}

bool Doc::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(now_));
    now_ = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

Rc View(const Rc& crop, int imageW, int imageH) {
    if (crop.Empty()) return {0, 0, (float)imageW, (float)imageH};
    float x = std::clamp(crop.x, 0.0f, (float)imageW), y = std::clamp(crop.y, 0.0f, (float)imageH);
    float r = std::clamp(crop.Right(), x, (float)imageW), b = std::clamp(crop.Bottom(), y, (float)imageH);
    if (r - x < 1 || b - y < 1) return {0, 0, (float)imageW, (float)imageH};
    // по целым пикселям: итоговая картинка режется по ним
    x = std::floor(x);
    y = std::floor(y);
    return {x, y, std::ceil(r) - x, std::ceil(b) - y};
}

float EditZoom(int imageW, int imageH) {
    const int side = std::max(imageW, imageH);
    if (side <= 0) return 1;
    return (float)std::clamp(480 / side, 1, 4);
}

Fit FitImage(int imageW, int imageH, float areaX, float areaY, float areaW, float areaH, float maxScale) {
    Fit f;
    if (imageW <= 0 || imageH <= 0 || areaW <= 0 || areaH <= 0) return f;
    f.scale = std::min({areaW / imageW, areaH / imageH, maxScale});
    f.w = imageW * f.scale;
    f.h = imageH * f.scale;
    // ровно по пикселям: при масштабе 1 снимок не размывается
    f.x = std::floor(areaX + (areaW - f.w) / 2);
    f.y = std::floor(areaY + (areaH - f.h) / 2);
    return f;
}

Pt ToImage(const Fit& f, float windowX, float windowY, const Rc& view) {
    if (f.scale <= 0) return {view.x, view.y};
    return {std::clamp(view.x + (windowX - f.x) / f.scale, view.x, view.Right()),
            std::clamp(view.y + (windowY - f.y) / f.scale, view.y, view.Bottom())};
}

Pt ToWindow(const Fit& f, Pt image, const Rc& view) {
    return {f.x + (image.x - view.x) * f.scale, f.y + (image.y - view.y) * f.scale};
}

void ArrowGeometry(Pt from, Pt to, float width, Pt out[4]) {
    float dx = to.x - from.x, dy = to.y - from.y, len = std::hypot(dx, dy);
    out[1] = to;
    if (len < 0.001f) {
        out[0] = out[2] = out[3] = to;
        return;
    }
    dx /= len;
    dy /= len;
    // у короткой стрелки наконечник не длиннее её самой
    float head = std::min(std::max(width * 5.0f, 12.0f), len * 0.6f), half = head * 0.55f;
    Pt base{to.x - dx * head, to.y - dy * head};
    out[0] = base;
    out[2] = {base.x - dy * half, base.y + dx * half};
    out[3] = {base.x + dy * half, base.y - dx * half};
}

int BlurCell(int rectW, int rectH) {
    // крупный текст требует клеток крупнее: иначе буквы читаются и сквозь них
    return std::max(12, std::min(rectW, rectH) / 3);
}

void Pixelate(uint32_t* pixels, int imageW, int imageH, int stride, Pt a, Pt b) {
    int x0 = std::clamp((int)std::floor(std::min(a.x, b.x)), 0, imageW);
    int x1 = std::clamp((int)std::ceil(std::max(a.x, b.x)), 0, imageW);
    int y0 = std::clamp((int)std::floor(std::min(a.y, b.y)), 0, imageH);
    int y1 = std::clamp((int)std::ceil(std::max(a.y, b.y)), 0, imageH);
    if (x1 <= x0 || y1 <= y0) return;
    const int cell = BlurCell(x1 - x0, y1 - y0);
    for (int y = y0; y < y1; y += cell) {
        int ye = std::min(y + cell, y1);
        for (int x = x0; x < x1; x += cell) {
            int xe = std::min(x + cell, x1);
            uint64_t sum[3] = {0, 0, 0}, n = 0;
            for (int j = y; j < ye; ++j)
                for (int i = x; i < xe; ++i) {
                    uint32_t p = pixels[j * stride + i];
                    sum[0] += p & 0xFF;
                    sum[1] += (p >> 8) & 0xFF;
                    sum[2] += (p >> 16) & 0xFF;
                    ++n;
                }
            uint32_t avg =
                0xFF000000u | (uint32_t)(sum[2] / n) << 16 | (uint32_t)(sum[1] / n) << 8 | (uint32_t)(sum[0] / n);
            for (int j = y; j < ye; ++j)
                for (int i = x; i < xe; ++i) pixels[j * stride + i] = avg;
        }
    }
}

namespace {

float SegmentDistance(Pt p, Pt a, Pt b) {
    float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

Rc Normalized(Pt a, Pt b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y)};
}

Rc Grow(const Rc& r, float d) { return {r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d}; }

}  // namespace

Rc Bounds(const Shape& s) {
    if (s.pts.empty()) return {};
    switch (s.tool) {
        case Tool::Label:
            return {s.pts[0].x, s.pts[0].y, s.extent.x, s.extent.y};
        case Tool::Step:
            return {s.pts[0].x - s.width / 2, s.pts[0].y - s.width / 2, s.width, s.width};
        case Tool::Marker: {
            float x0 = s.pts[0].x, x1 = x0, y0 = s.pts[0].y, y1 = y0;
            for (const Pt& p : s.pts) {
                x0 = std::min(x0, p.x);
                x1 = std::max(x1, p.x);
                y0 = std::min(y0, p.y);
                y1 = std::max(y1, p.y);
            }
            return {x0, y0, x1 - x0, y1 - y0};
        }
        default:
            return s.pts.size() >= 2 ? Normalized(s.pts[0], s.pts[1]) : Rc{};
    }
}

bool HitTest(const Shape& s, Pt p, float tolerance) {
    if (s.pts.empty()) return false;
    switch (s.tool) {
        case Tool::Arrow:
            return s.pts.size() >= 2 && SegmentDistance(p, s.pts[0], s.pts[1]) <= s.width / 2 + tolerance;
        case Tool::Box: {
            // рамка пустая: сквозь неё можно выбрать то, что внутри
            Rc r = Bounds(s);
            float reach = s.width / 2 + tolerance;
            return Grow(r, reach).Contains(p) && !Grow(r, -reach).Contains(p);
        }
        case Tool::Marker: {
            for (size_t i = 0; i + 1 < s.pts.size(); ++i)
                if (SegmentDistance(p, s.pts[i], s.pts[i + 1]) <= s.width / 2 + tolerance) return true;
            return false;
        }
        default:
            return Grow(Bounds(s), tolerance).Contains(p);
    }
}

int TopShape(const std::vector<Shape>& shapes, Pt p, float tolerance) {
    for (int i = (int)shapes.size() - 1; i >= 0; --i)
        if (HitTest(shapes[(size_t)i], p, tolerance)) return i;
    return -1;
}

std::vector<Pt> Handles(const Shape& s) {
    if (s.pts.empty()) return {};
    Rc r = Bounds(s);
    switch (s.tool) {
        case Tool::Arrow:
            return s.pts.size() >= 2 ? std::vector<Pt>{s.pts[0], s.pts[1]} : std::vector<Pt>{};
        case Tool::Label:
        case Tool::Step:
            return {{r.Right(), r.Bottom()}};
        case Tool::Translation:
            return {};  // только двигать: шрифт уже подобран под рамку
        default:
            return {{r.x, r.y}, {r.Right(), r.y}, {r.x, r.Bottom()}, {r.Right(), r.Bottom()}};
    }
}

int HandleAt(const Shape& s, Pt p, float tolerance) {
    std::vector<Pt> h = Handles(s);
    for (size_t i = 0; i < h.size(); ++i)
        if (std::fabs(p.x - h[i].x) <= tolerance && std::fabs(p.y - h[i].y) <= tolerance) return (int)i;
    return -1;
}

void DragHandle(Shape& s, int handle, Pt to) {
    if (s.pts.empty() || handle < 0) return;
    switch (s.tool) {
        case Tool::Arrow:
            if (handle < (int)s.pts.size()) s.pts[(size_t)handle] = to;
            return;
        case Tool::Label: {
            // шрифт растёт пропорционально: высота подписи идёт за курсором
            if (s.extent.y <= 0) return;
            float k = std::max(4.0f, to.y - s.pts[0].y) / s.extent.y;
            s.width *= k;
            s.extent.x *= k;
            s.extent.y *= k;
            return;
        }
        case Tool::Step:
            s.width = std::max(8.0f, 2 * std::max(std::fabs(to.x - s.pts[0].x), std::fabs(to.y - s.pts[0].y)));
            return;
        case Tool::Box:
        case Tool::Blur: {
            if (s.pts.size() < 2) return;
            Rc r = Bounds(s);
            // противоположный угол стоит на месте
            Pt fixed{handle == 0 || handle == 2 ? r.Right() : r.x, handle == 0 || handle == 1 ? r.Bottom() : r.y};
            s.pts = {fixed, to};
            return;
        }
        case Tool::Marker: {
            Rc r = Bounds(s);
            Pt fixed{handle == 0 || handle == 2 ? r.Right() : r.x, handle == 0 || handle == 1 ? r.Bottom() : r.y};
            Pt from{handle == 0 || handle == 2 ? r.x : r.Right(), handle == 0 || handle == 1 ? r.y : r.Bottom()};
            float kx = std::fabs(from.x - fixed.x) > 0.5f ? (to.x - fixed.x) / (from.x - fixed.x) : 1;
            float ky = std::fabs(from.y - fixed.y) > 0.5f ? (to.y - fixed.y) / (from.y - fixed.y) : 1;
            for (Pt& p : s.pts) {
                p.x = fixed.x + (p.x - fixed.x) * kx;
                p.y = fixed.y + (p.y - fixed.y) * ky;
            }
            return;
        }
        default:
            return;
    }
}

void Translate(Shape& s, float dx, float dy) {
    for (Pt& p : s.pts) {
        p.x += dx;
        p.y += dy;
    }
}

void SnapAxis(Pt a, Pt& b) {
    float dx = std::fabs(b.x - a.x), dy = std::fabs(b.y - a.y);
    // около 4 градусов: случайный наклон; заметно косая линия остаётся такой, как нарисована
    constexpr float kTan = 0.07f;
    if (dy <= dx * kTan)
        b.y = a.y;
    else if (dx <= dy * kTan)
        b.x = a.x;
}

bool StraightenStroke(std::vector<Pt>& pts) {
    if (pts.size() < 3) return false;
    Pt a = pts.front(), b = pts.back();
    float len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < 40) return false;  // короткий штрих - это пометка, а не линия
    float limit = std::max(4.0f, len * 0.04f);
    for (const Pt& p : pts)
        if (SegmentDistance(p, a, b) > limit) return false;
    SnapAxis(a, b);
    pts = {a, b};
    return true;
}

bool IsLight(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return r * 299 + g * 587 + b * 114 > 160000;
}

std::vector<Rc> SecretBoxes(const std::vector<OcrWord>& words, float pad) {
    std::vector<Rc> found;
    std::map<int, std::wstring> lineText;
    std::map<int, Rc> lineBox;
    std::map<int, bool> lineHit;
    auto merge = [](Rc& into, const Rc& r) {
        float x = std::min(into.x, r.x), y = std::min(into.y, r.y);
        float right = std::max(into.Right(), r.Right()), bottom = std::max(into.Bottom(), r.Bottom());
        into = {x, y, right - x, bottom - y};
    };
    for (const OcrWord& w : words) {
        std::wstring& t = lineText[w.line];
        if (t.empty())
            lineBox[w.line] = w.box;
        else {
            t += L' ';
            merge(lineBox[w.line], w.box);
        }
        t += w.text;
        if (!secrets::Kind(w.text).empty()) {
            found.push_back(Grow(w.box, pad));
            lineHit[w.line] = true;
        }
    }
    for (const auto& [line, text] : lineText)
        if (!lineHit[line] && !secrets::Kind(text).empty()) found.push_back(Grow(lineBox[line], pad));
    // два движка распознавания читают одно и то же слово: одна рамка вместо двух
    std::vector<Rc> merged;
    for (const Rc& r : found) {
        bool joined = false;
        for (Rc& m : merged)
            if (r.x < m.Right() && m.x < r.Right() && r.y < m.Bottom() && m.y < r.Bottom()) {
                merge(m, r);
                joined = true;
                break;
            }
        if (!joined) merged.push_back(r);
    }
    return merged;
}

TextFit FitText(const std::wstring& text, float boxW, float boxH, float lineH, float maxW, MeasureFn measure, void* ctx) {
    TextFit fit;
    // рамка слова у движка - по самим буквам, плотнее настоящего размера шрифта
    float size = std::clamp(lineH * 1.05f, 9.0f, 72.0f), width = std::max(boxW, 1.0f);
    const float minSize = std::min(size, 9.0f);
    const float roomH = std::max(boxH, lineH * 1.3f);
    for (;;) {
        Pt need = measure(text, size, width, ctx);
        if (need.y <= roomH + 1) {
            fit = {size, width, std::max(boxH, need.y)};
            return fit;
        }
        // одна строка оригинала, перевод длиннее: сперва шире, а уже потом мельче
        Pt wide = measure(text, size, std::max(maxW, width), ctx);
        if (maxW > width && wide.y <= roomH + 1) {
            Pt tight = measure(text, size, 100000, ctx);
            fit = {size, std::min(std::max(maxW, width), tight.x + 2), std::max(boxH, wide.y)};
            return fit;
        }
        if (size <= minSize) {
            fit = {size, std::max(maxW, width), wide.y};
            return fit;
        }
        size = std::max(minSize, size * 0.92f);
    }
}

float RoomRight(const Rc& box, const std::vector<Rc>& others, float limit) {
    float right = limit;
    for (const Rc& o : others)
        if (o.x >= box.Right() - 1 && o.y < box.Bottom() && box.y < o.Bottom()) right = std::min(right, o.x - 4);
    return std::max(right, box.Right());
}

int EvictIndex(const std::vector<bool>& keep) {
    for (size_t i = 0; i < keep.size(); ++i)
        if (!keep[i]) return (int)i;
    return keep.empty() ? -1 : 0;
}

}  // namespace markup
