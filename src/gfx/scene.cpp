// Живые сцены утилит (см. scene.h): лента событий, частицы, вихрь очистки, щит проверки.
#include "gfx/scene.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "core/fx.h"
#include "core/text.h"
#include "gfx/color.h"
#include "tools/tools.h"

using namespace Gdiplus;

namespace scene {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kSquish = 0.72f;  // вихрь сплюснут: смотрим на него чуть сверху

SRWLOCK g_lock = SRWLOCK_INIT;
CleanFeed g_clean;
GuardFeed g_guard;

float Rnd(float a, float b) { return a + (b - a) * (float)(rand() % 10000) / 10000.0f; }

float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    float t = l > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l, 0.0f, 1.0f) : 0;
    float qx = ax + dx * t - px, qy = ay + dy * t - py;
    return std::sqrt(qx * qx + qy * qy);
}

// Галочка в единицах половины ширины знака: три точки ломаной.
const float kCheck[3][2] = {{-0.42f, -0.02f}, {-0.1f, 0.3f}, {0.45f, -0.32f}};

// Щит: прямые бока, пологая арка сверху, снизу сходится в острие. Середина (cx, cy), полуразмеры w, h.
void ShieldPath(GraphicsPath& p, float cx, float cy, float w, float h) {
    auto P = [&](float x, float y) { return PointF(cx + x * w, cy + y * h); };
    p.StartFigure();
    p.AddBezier(P(-0.92f, -0.8f), P(-0.55f, -0.8f), P(-0.25f, -1.0f), P(0, -1.0f));
    p.AddBezier(P(0, -1.0f), P(0.25f, -1.0f), P(0.55f, -0.8f), P(0.92f, -0.8f));
    p.AddLine(P(0.92f, -0.8f), P(0.92f, 0.0f));
    p.AddBezier(P(0.92f, 0.0f), P(0.92f, 0.5f), P(0.42f, 0.82f), P(0, 1.0f));
    p.AddBezier(P(0, 1.0f), P(-0.42f, 0.82f), P(-0.92f, 0.5f), P(-0.92f, 0.0f));
    p.CloseFigure();
}

Font* FontOf(float px, bool bold) {
    // живут до конца процесса: GDI+ может закрыться раньше статических объектов
    static auto* fonts = new std::map<int, Font*>;
    static auto* fam = new FontFamily(L"Segoe UI");
    static auto* famBold = new FontFamily(L"Segoe UI Semibold");
    int key = (int)(px * 10) * 2 + (bold ? 1 : 0);
    Font*& f = (*fonts)[key];
    if (!f) f = new Font(bold ? famBold : fam, px, FontStyleRegular, UnitPixel);
    return f;
}

}  // namespace

// ---------------- частицы ----------------

void Spring(Dot& d, float dt, float stiff, float damp) {
    d.px = d.x, d.py = d.y;
    d.vx += ((d.tx - d.x) * stiff - d.vx * damp) * dt;
    d.vy += ((d.ty - d.y) * stiff - d.vy * damp) * dt;
    d.x += d.vx * dt;
    d.y += d.vy * dt;
}

void Batch::Add(int col, float x, float y, float size) {
    rects_[col & 7].push_back(RectF(x - size / 2, y - size / 2, size, size));
}

void Batch::Flush(Graphics& g, const Color* cols, int n, BYTE alpha) {
    for (int i = 0; i < 8; ++i) {
        if (rects_[i].empty()) continue;
        if (i < n) {
            Color c = cols[i];
            SolidBrush b(Color((BYTE)(c.GetA() * alpha / 255), c.GetR(), c.GetG(), c.GetB()));
            g.FillRectangles(&b, rects_[i].data(), (INT)rects_[i].size());
        }
        rects_[i].clear();
    }
}

void Label(Graphics& g, const std::wstring& s, const RectF& r, Color c, float px, bool bold, int align) {
    if (s.empty() || r.Width <= 0) return;
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetAlignment(align == 1 ? StringAlignmentCenter : align == 2 ? StringAlignmentFar : StringAlignmentNear);
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), FontOf(px, bold), r, &sf, &b);
}

float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

// ---------------- ленты ----------------

void CleanBegin(const std::vector<CleanItem>& items) {
    AcquireSRWLockExclusive(&g_lock);
    int gen = g_clean.gen + 1;
    g_clean = CleanFeed{};
    g_clean.gen = gen;
    g_clean.active = true;
    g_clean.items = items;
    ReleaseSRWLockExclusive(&g_lock);
}

void CleanMark(size_t i, int state, ULONGLONG freed) {
    AcquireSRWLockExclusive(&g_lock);
    if (i < g_clean.items.size()) {
        g_clean.items[i].state = state;
        if (state == 2) g_clean.items[i].freed = freed;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void CleanEnd() {
    AcquireSRWLockExclusive(&g_lock);
    g_clean.done = true;
    g_clean.doneAt = GetTickCount64();
    for (CleanItem& it : g_clean.items) it.state = 2;
    ReleaseSRWLockExclusive(&g_lock);
}

CleanFeed Clean() {
    AcquireSRWLockShared(&g_lock);
    CleanFeed f = g_clean;
    ReleaseSRWLockShared(&g_lock);
    return f;
}

bool CleanRecent(ULONGLONG ms) {
    AcquireSRWLockShared(&g_lock);
    bool r = g_clean.active && (!g_clean.done || GetTickCount64() - g_clean.doneAt < ms);
    ReleaseSRWLockShared(&g_lock);
    return r;
}

void GuardBegin(int expect) {
    AcquireSRWLockExclusive(&g_lock);
    int gen = g_guard.gen + 1;
    g_guard = GuardFeed{};
    g_guard.gen = gen;
    g_guard.active = true;
    g_guard.expect = expect;
    ReleaseSRWLockExclusive(&g_lock);
}

void GuardEnd(int checked, const std::vector<GuardFind>& finds) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_guard.active) {  // итог без начала (фоновая проверка): сцена сразу с итогом
        g_guard.gen++;
        g_guard.active = true;
        g_guard.expect = checked;
    }
    g_guard.done = true;
    g_guard.doneAt = GetTickCount64();
    g_guard.checked = checked;
    g_guard.finds = finds;
    ReleaseSRWLockExclusive(&g_lock);
}

GuardFeed Guard() {
    AcquireSRWLockShared(&g_lock);
    GuardFeed f = g_guard;
    ReleaseSRWLockShared(&g_lock);
    return f;
}

// ---------------- очистка ----------------

float CleanScene::Height(size_t items, float k) {
    size_t rows = std::min<size_t>(items, 5);
    return std::max(rows * 38.0f + 12, 178.0f) * k;
}

void CleanScene::Build(const CleanFeed& f) {
    float k = k_;
    rows_.clear();
    dots_.clear();
    sparks_.clear();
    map_.assign(f.items.size(), 0);
    size_t n = f.items.size();
    size_t own = n <= 5 ? n : 4;
    for (size_t i = 0; i < n; ++i) {
        size_t m = std::min(i, own);
        map_[i] = (int)m;
        if (m >= rows_.size()) rows_.push_back(Row{});
        Row& row = rows_[m];
        row.bytes += f.items[i].bytes;
        if (i < own) row.name = f.items[i].name;
    }
    if (n > own) rows_.back().name = L"Ещё " + Plural((int)(n - own), L"место", L"места", L"мест");
    // вихрь справа, строки слева
    vr_ = std::min(rect_.Height * 0.36f, 64 * k);
    // в широкой рамке (студия) строки не тянутся на всю ширину: вся картинка собирается посередине
    float w0 = std::min(rect_.Width - 2 * vr_ - 44 * k, 330 * k);
    float x0 = std::max(0.0f, (rect_.Width - (w0 + 44 * k + 2 * vr_)) / 2);
    cx_ = x0 + w0 + 30 * k + vr_;
    cy_ = std::min(rect_.Height * 0.42f, vr_ * kSquish + 30 * k);
    x0_ = x0, w0_ = w0;
    ULONGLONG top = 0;
    for (const Row& r : rows_) top = std::max(top, r.bytes);
    for (size_t i = 0; i < rows_.size(); ++i) {
        Row& r = rows_[i];
        r.y = 4 * k + i * 38 * k;
        r.w = top ? std::max(18 * k, (float)(w0 * (double)r.bytes / top)) : w0 * 0.7f;
        float pitch = std::max(4.5f * k, r.w / 60);
        int cols = std::max(3, (int)(r.w / pitch));
        r.first = (int)dots_.size();
        r.count = cols * 3;
        // по порядку осыпания: сначала правый край полоски
        for (int c = cols - 1; c >= 0; --c)
            for (int q = 0; q < 3; ++q) {
                Dot d;
                d.x = d.px = d.tx = x0 + c * pitch + pitch / 2;
                d.y = d.py = d.ty = r.y + 23 * k + q * 4.5f * k;
                d.row = (int)i;
                d.col = (BYTE)(i % 6);
                dots_.push_back(d);
            }
    }
    t_ = 0;
    finAt_ = burstAt_ = -1;
    pulse_ = 0;
    settled_ = false;
}

void CleanScene::Burst() {
    float k = k_;
    burstAt_ = t_;
    sparks_.clear();
    float s = vr_ * 0.62f;
    for (int i = 0; i < 46; ++i) {
        Dot d;
        d.x = d.px = cx_;
        d.y = d.py = cy_;
        float a = Rnd(0, 2 * kPi), v = Rnd(110, 300) * k;
        d.vx = std::cos(a) * v;
        d.vy = std::sin(a) * v * kSquish;
        // точка на галочке: первая половина - короткий штрих, вторая - длинный
        float u = i / 45.0f;
        int seg = u < 0.36f ? 0 : 1;
        float lt = seg == 0 ? u / 0.36f : (u - 0.36f) / 0.64f;
        d.tx = cx_ + (kCheck[seg][0] + (kCheck[seg + 1][0] - kCheck[seg][0]) * lt) * s;
        d.ty = cy_ + (kCheck[seg][1] + (kCheck[seg + 1][1] - kCheck[seg][1]) * lt) * s;
        d.delay = 0.2f + i * 0.005f;
        d.col = (BYTE)(i % 3 == 0 ? 7 : 6);
        sparks_.push_back(d);
    }
}

bool CleanScene::Draw(Graphics& g, const RectF& r, float k, const Palette& p, POINT mouse, bool click) {
    CleanFeed f = Clean();
    if (f.gen != gen_ || std::fabs(r.Width - rect_.Width) > 1 || std::fabs(r.Height - rect_.Height) > 1 || k != k_) {
        bool same = f.gen == gen_;
        std::vector<Row> old = rows_;
        gen_ = f.gen;
        rect_ = r;
        k_ = k;
        Build(f);
        if (same)  // окно поменяло размер посреди чистки - уже ушедшее не возвращаем
            for (size_t i = 0; i < rows_.size() && i < old.size(); ++i)
                if (old[i].since >= 0) rows_[i].since = 0;
    }
    rect_ = r;
    ULONGLONG now = GetTickCount64();
    float dt = last_ && now - last_ < 200 ? (now - last_) / 1000.0f : 0.016f;
    last_ = now;
    bool game = fx::Game();
    dt *= (float)(1000.0 / std::max(250.0, fx::Dur(1000)));  // «спокойно» / «живо»
    t_ += dt;
    float ox = r.X, oy = r.Y;
    float mx = mouse.x - ox, my = mouse.y - oy;

    // состояние строк из ленты
    // строка «ещё» собрана из нескольких мест: готова, когда готовы все, чистится, когда начато хоть одно
    std::vector<bool> allDone(rows_.size(), true), started(rows_.size(), false);
    std::vector<ULONGLONG> freed(rows_.size(), 0);
    for (size_t i = 0; i < f.items.size() && i < map_.size(); ++i) {
        int m = map_[i];
        const CleanItem& it = f.items[i];
        freed[m] += it.freed;
        if (it.state < 2) allDone[m] = false;
        if (it.state >= 1) started[m] = true;
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
        Row& row = rows_[i];
        row.state = f.done || allDone[i] ? 2 : started[i] ? 1 : 0;
        row.freed = freed[i];
        if (row.state >= 1 && row.since < 0) row.since = t_;
        // сколько частиц уже отпущено
        int want = row.released;
        if (row.state == 1)
            want = std::max(want, (int)(row.count * 0.85 * (1 - std::exp(-(t_ - row.since) / 1.1))));
        else if (row.state == 2)
            want = std::min(row.count, row.released + std::max(1, (int)std::ceil(row.count * dt / 0.3f)));
        if (game && row.state == 2) want = row.count;
        for (int q = row.released; q < want && q < row.count; ++q) {
            // сначала частица съезжает по своей строке вправо, как песок, и только у вихря закручивается:
            // так она не пролетает над чужими строками
            Dot& d = dots_[row.first + q];
            d.state = 3;
            d.vx = Rnd(20, 70) * k;
            d.vy = Rnd(-12, 12) * k;
        }
        row.released = std::max(row.released, want);
    }
    // полёт в вихрь
    bool flying = false;
    float edge = x0_ + w0_;  // где кончаются строки
    for (Dot& d : dots_) {
        if (d.state == 3) {
            d.px = d.x, d.py = d.y;
            d.vx += dt * 900 * k;
            if (d.x < edge)
                d.vy -= d.vy * std::min(1.0f, 5 * dt);
            else
                d.vy += ((cy_ - d.y) * 16 - d.vy * 5) * dt;
            d.x += d.vx * dt;
            d.y += d.vy * dt;
            float dx = d.x - cx_, dy = (d.y - cy_) / kSquish;
            if (game || std::sqrt(dx * dx + dy * dy) < vr_ * 1.25f || d.x > cx_) {
                d.state = 1;
                d.a = std::atan2(dy, dx);
                d.r = std::sqrt(dx * dx + dy * dy);
            }
            flying = true;
            continue;
        }
        if (d.state != 1) continue;
        if (game) d.r = 0;
        d.px = d.x, d.py = d.y;
        d.a += dt * (0.8f + 210 * k / std::max(d.r, 16 * k));
        d.r -= dt * (d.r * 1.8f + 30 * k);
        d.x = cx_ + std::cos(d.a) * d.r;
        d.y = cy_ + std::sin(d.a) * d.r * kSquish;
        if (d.r < 5 * k) {
            d.state = 2;
            rows_[d.row].absorbed++;
            pulse_ = std::min(1.0f, pulse_ + 0.05f);
        } else {
            flying = true;
        }
    }
    bool overV = std::hypot(mx - cx_, (my - cy_) / kSquish) < vr_;
    spin_ += dt * (1.4f + pulse_ * 7 + (overV ? 2.5f : 0));
    pulse_ = std::max(0.0f, pulse_ - dt * 1.2f);
    bool all = !flying;
    for (const Row& row : rows_) all = all && row.released >= row.count;
    if (f.done && all && finAt_ < 0) {
        finAt_ = t_;
        Burst();
    }
    if (click && overV && finAt_ >= 0 && t_ - burstAt_ > 0.6) Burst();  // щелчок по вихрю после итога - ещё раз
    for (Dot& d : sparks_) {
        if (game) {
            d.x = d.tx, d.y = d.ty, d.vx = d.vy = 0;
            continue;
        }
        if (t_ - burstAt_ > d.delay) {
            Spring(d, dt, 70, 10);
        } else {
            d.px = d.x, d.py = d.y;
            d.vx *= 1 - std::min(1.0f, 3 * dt);
            d.vy *= 1 - std::min(1.0f, 3 * dt);
            d.x += d.vx * dt;
            d.y += d.vy * dt;
        }
    }
    if (game && finAt_ >= 0) finAt_ = burstAt_ = t_ - 10;
    float fin = finAt_ < 0 ? 0 : Ease((float)(t_ - finAt_) / 0.5f);

    // ---- рисование ----
    const Color cols[8] = {p.accent,
                           p.gold,
                           p.accent2,
                           MixColor(p.accent, p.gold, 0.5),
                           MixColor(p.accent2, p.accent, 0.5),
                           MixColor(p.gold, p.text, 0.45),
                           p.gold,
                           p.accent};
    GraphicsState gs = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    float x0 = x0_, w0 = w0_;
    for (const Row& row : rows_) {
        float y = row.y;
        bool done = row.state == 2 && row.absorbed >= row.count;
        if (row.state == 1 && !game) {
            SolidBrush hl(WithAlpha(p.accent, 16));
            g.FillRectangle(&hl, ox + x0 - 6 * k, oy + y - 2 * k, w0 + 12 * k, 36 * k);
        }
        Label(g, row.name, RectF(ox + x0, oy + y, w0 * 0.62f, 19 * k), done ? p.faint : p.text, 12.5f * k, !done);
        std::wstring right;
        Color rc = p.muted;
        if (done) {
            right = row.freed ? L"−" + tools::Bytes(row.freed) : L"чисто";
            rc = row.freed ? p.accent : p.faint;
        } else if (row.bytes) {
            right = tools::Bytes((ULONGLONG)(row.bytes * (1.0 - (double)row.absorbed / std::max(1, row.count))));
        } else if (row.state == 1) {
            right = L"чищу";
        }
        Label(g, right, RectF(ox + x0 + w0 * 0.6f, oy + y, w0 * 0.4f, 19 * k), rc, 12 * k, done, 2);
    }
    // частицы: лежащие, хвосты, летящие
    Batch lying, tails, flyB;
    for (const Dot& d : dots_) {
        const Row& row = rows_[d.row];
        float size = std::max(2.0f, std::max(4.5f * k, row.w / 60) * 0.66f);
        if (d.state == 0) {
            // край, который вот-вот осыпется, подрагивает
            float jig = row.state == 1 && &d - &dots_[row.first] < row.released + 6
                            ? std::sin((float)t_ * 40 + d.y) * 0.8f * k
                            : 0;
            lying.Add(d.col, ox + d.x + jig, oy + d.y, size);
        } else if (d.state == 1 || d.state == 3) {
            float s = d.state == 3 ? size : size * std::clamp(d.r / (vr_ * 1.3f), 0.45f, 1.0f);
            tails.Add(d.col, ox + (d.x + d.px) / 2 - (d.x - d.px), oy + (d.y + d.py) / 2 - (d.y - d.py), s * 0.8f);
            flyB.Add(d.col, ox + d.x, oy + d.y, s);
        }
    }
    lying.Flush(g, cols, 8);
    tails.Flush(g, cols, 8, 90);
    // вихрь: сплюснутые дуги крутятся, середина вспыхивает от каждой поглощённой частицы
    float fade = 1 - fin;
    if (fade > 0.01f) {
        for (int i = 0; i < 4; ++i) {
            float rr = vr_ * (0.32f + 0.2f * i) * (1 - fin * 0.85f);
            Pen pen(WithAlpha(i % 2 ? p.accent2 : p.accent, (BYTE)((150 - i * 28) * fade)), (2.2f - i * 0.3f) * k);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            float a0 = (spin_ * (1 + i * 0.18f) + i * 1.3f) * 57.29578f;
            g.DrawArc(&pen, ox + cx_ - rr, oy + cy_ - rr * kSquish, rr * 2, rr * 2 * kSquish, a0, 200);
        }
        float core = (4.5f + pulse_ * 3.5f) * k * fade;
        SolidBrush glow(WithAlpha(p.accent, (BYTE)(60 * fade)));
        g.FillEllipse(&glow, ox + cx_ - core * 2, oy + cy_ - core * 2 * kSquish, core * 4, core * 4 * kSquish);
        SolidBrush cb(WithAlpha(MixColor(p.accent, p.gold, pulse_ * 0.6), (BYTE)(230 * fade)));
        g.FillEllipse(&cb, ox + cx_ - core, oy + cy_ - core * kSquish, core * 2, core * 2 * kSquish);
    }
    flyB.Flush(g, cols, 8);
    // итог: искры собираются в галочку, поверх проступает штрих
    if (!sparks_.empty()) {
        float since = (float)(t_ - burstAt_);
        float line = Ease((since - 0.9f) / 0.4f);
        Batch sp;
        for (const Dot& d : sparks_) sp.Add(d.col, ox + d.x, oy + d.y, 3.4f * k);
        sp.Flush(g, cols, 8, (BYTE)(255 * (1 - line * 0.85f)));
        if (line > 0) {
            float s = vr_ * 0.62f;
            Pen pen(WithAlpha(p.accent, (BYTE)(255 * line)), 4.5f * k);
            pen.SetLineJoin(LineJoinRound);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            PointF pts[3];
            for (int i = 0; i < 3; ++i) pts[i] = PointF(ox + cx_ + kCheck[i][0] * s, oy + cy_ + kCheck[i][1] * s);
            g.DrawLines(&pen, pts, 3);
        }
    }
    // счётчик под вихрем
    double total = 0;
    for (const Row& row : rows_) {
        ULONGLONG v = row.state == 2 && row.freed ? row.freed : row.bytes;
        total += (double)v * row.absorbed / std::max(1, row.count);
    }
    float ty = cy_ + vr_ * kSquish + 6 * k;
    RectF tr(ox + cx_ - vr_ - 10 * k, oy + ty, vr_ * 2 + 20 * k, 16 * k);
    Label(g, f.done && fin > 0.5f ? L"освобождено" : L"освобождаю", tr, p.muted, 11 * k, false, 1);
    tr.Y += 16 * k;
    tr.Height = 24 * k;
    Label(g, tools::Bytes((ULONGLONG)total), tr, p.text, (17 + 3 * fin) * k, true, 1);
    g.Restore(gs);

    bool moving = flying || (f.active && !f.done) || pulse_ > 0.01f || (burstAt_ >= 0 && t_ - burstAt_ < 1.6) || !all;
    if (game) moving = false;
    settled_ = !moving && f.done;
    return moving;
}

// ---------------- защита ----------------

float GuardScene::Height(size_t finds, float k) {
    size_t n = std::min<size_t>(finds, 4);
    return (150 + (n ? n * 24 + (finds > 4 ? 18 : 0) + 6 : 22)) * k;
}

void GuardScene::Build(const GuardFeed& f) {
    float k = k_;
    dots_.clear();
    bad_.clear();
    finds_.clear();
    landed_ = false;
    landAt_ = -1;
    t_ = 0;
    float A = 150 * k;
    gx_ = 6 * k, gy_ = 10 * k;
    gw_ = rect_.Width * 0.46f, gh_ = A - 22 * k;
    int n = std::clamp(f.expect > 0 ? f.expect : 120, 30, 168);
    int cols = std::max(4, (int)std::lround(std::sqrt(n * gw_ / gh_)));
    int rows = (n + cols - 1) / cols;
    pitch_ = std::min(gw_ / cols, gh_ / rows);
    for (int i = 0; i < n; ++i) {
        Dot d;
        d.x = d.px = d.tx = gx_ + (i % cols) * pitch_ + pitch_ / 2;
        d.y = d.py = d.ty = gy_ + (i / cols) * pitch_ + pitch_ / 2;
        d.row = i / cols;
        dots_.push_back(d);
    }
    sh_ = A * 0.42f;
    sw_ = std::min(sh_ * 0.82f, (rect_.Width - gx_ - gw_ - 30 * k) / 2);
    sx_ = gx_ + gw_ + (rect_.Width - gx_ - gw_) / 2;
    sy_ = A * 0.46f;
}

void GuardScene::Land(const GuardFeed& f) {
    float k = k_;
    landed_ = true;
    landAt_ = t_;
    checked_ = f.checked;
    finds_ = f.finds;
    int n = (int)dots_.size();
    int nb = std::min<int>((int)finds_.size(), 4);
    for (int j = 0; j < nb; ++j) bad_.push_back(std::clamp((int)((j + 0.5f) * n / nb) + (j * 7) % 5 - 2, 0, n - 1));
    std::vector<int> ok;
    for (int i = 0; i < n; ++i)
        if (std::find(bad_.begin(), bad_.end(), i) == bad_.end()) ok.push_back(i);
    // места внутри щита: сетка с шагом под число точек
    // места внутри щита (чуть меньше контура): сетка с шагом, подогнанным под число точек
    GraphicsPath path;
    ShieldPath(path, sx_, sy_, sw_ * 0.84f, sh_ * 0.86f);
    std::vector<PointF> spots;
    size_t want = std::max<size_t>(1, ok.size());
    float q = std::sqrt(2 * sw_ * 2 * sh_ * 0.5f / want);
    for (int tries = 0; tries < 6; ++tries) {
        spots.clear();
        for (float y = sy_ - sh_ + q / 2; y < sy_ + sh_; y += q)
            for (float x = sx_ - sw_ + q / 2; x < sx_ + sw_; x += q)
                if (path.IsVisible(x, y)) spots.push_back(PointF(x, y));
        if (spots.empty() || std::fabs((float)spots.size() / want - 1) < 0.08f) break;
        q *= std::sqrt((float)spots.size() / want);
    }
    if (spots.empty()) spots.push_back(PointF(sx_, sy_));
    // мест больше, чем точек - недостающие откалываются от соседних: щит всегда целый
    for (size_t c = 0; ok.size() < spots.size() && !ok.empty(); ++c) {
        size_t from = ok[(c * 7919 + 3) % ok.size()];
        dots_.push_back(dots_[from]);
        ok.insert(ok.begin() + (std::find(ok.begin(), ok.end(), (int)from) - ok.begin()), (int)dots_.size() - 1);
    }
    // точки и места по порядку сверху вниз: поток сохраняет форму
    bool alarm = false;
    for (const GuardFind& fd : finds_) alarm = alarm || fd.score >= 6;
    int lastRow = 1;
    for (const Dot& d : dots_) lastRow = std::max(lastRow, d.row);
    for (size_t i = 0; i < ok.size(); ++i) {
        Dot& d = dots_[ok[i]];
        PointF s = spots[std::min(i * spots.size() / ok.size(), spots.size() - 1)];
        d.tx = s.X;
        d.ty = s.Y;
        d.delay = 0.1f + 0.45f * d.row / lastRow + Rnd(0, 0.08f);
        // узор внутри щита: галочка, если всё чисто, иначе «!» (красный, если есть опасное)
        float ux = (s.X - sx_) / sw_, uy = (s.Y - sy_) / sw_;
        bool mark;
        if (finds_.empty())
            mark = std::min(SegDist(ux, uy, kCheck[0][0], kCheck[0][1], kCheck[1][0], kCheck[1][1]),
                            SegDist(ux, uy, kCheck[1][0], kCheck[1][1], kCheck[2][0], kCheck[2][1])) < 0.17f;
        else
            mark = SegDist(ux, uy, 0, -0.62f, 0, 0.12f) < 0.12f || std::hypot(ux, uy - 0.4f) < 0.13f;
        d.col = (BYTE)(mark ? (alarm ? 7 : 5) : 4);
    }
    for (int j = 0; j < nb; ++j) {
        Dot& d = dots_[bad_[j]];
        d.tx = 10 * k;
        d.ty = 150 * k + 4 * k + j * 24 * k + 12 * k;
        d.delay = 0.95f + j * 0.08f;
        d.col = 6;
    }
}

bool GuardScene::Draw(Graphics& g, const RectF& r, float k, const Palette& p, POINT mouse) {
    GuardFeed f = Guard();
    if (f.gen != gen_ || std::fabs(r.Width - rect_.Width) > 1 || k != k_) {
        gen_ = f.gen;
        rect_ = r;
        k_ = k;
        Build(f);
    }
    rect_ = r;
    ULONGLONG now = GetTickCount64();
    float dt = last_ && now - last_ < 200 ? (now - last_) / 1000.0f : 0.016f;
    last_ = now;
    dt *= (float)(1000.0 / std::max(250.0, fx::Dur(1000)));
    t_ += dt;
    bool game = fx::Game();
    if (f.done && !landed_) Land(f);
    float since = landed_ ? (float)(t_ - landAt_) : 0;
    bool moving = !landed_;
    for (Dot& d : dots_) {
        if (!landed_ || since < d.delay) continue;
        if (game) {
            d.x = d.tx, d.y = d.ty, d.vx = d.vy = 0;
            continue;
        }
        Spring(d, dt, 60, 10);
        if (std::fabs(d.vx) + std::fabs(d.vy) > 1 || std::fabs(d.x - d.tx) + std::fabs(d.y - d.ty) > 0.5f)
            moving = true;
        else
            d.x = d.tx, d.y = d.ty;
    }
    if (landed_ && since < 2.4f) moving = true;
    if (game) moving = false;
    float ox = r.X, oy = r.Y;

    GraphicsState gs = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // луч: пока ждём итога - бежит по кругу, после итога - последний проход
    float span = gh_ + 40 * k;
    float beam = landed_ ? gy_ - 20 * k + std::min(1.0f, since / 0.55f) * span
                         : gy_ - 20 * k + (float)std::fmod(t_ * 0.8, 1.0) * span;
    bool beamOn = !game && (!landed_ || since < 0.55f);
    if (beamOn) {
        SolidBrush glow(WithAlpha(p.accent2, 26));
        g.FillRectangle(&glow, ox + gx_ - 4 * k, oy + beam - 9 * k, gw_ + 8 * k, 18 * k);
        SolidBrush line(WithAlpha(p.accent2, 170));
        g.FillRectangle(&line, ox + gx_ - 4 * k, oy + beam - 0.75f * k, gw_ + 8 * k, 1.5f * k);
    }
    // щит
    float fill = landed_ ? Ease((since - 0.5f) / 1.0f) : 0;
    bool alarm = false;
    for (const GuardFind& fd : finds_) alarm = alarm || fd.score >= 6;
    {
        GraphicsPath path;
        ShieldPath(path, ox + sx_, oy + sy_, sw_, sh_);
        SolidBrush back(WithAlpha(alarm ? p.accent : p.accent2, (BYTE)(10 + 14 * fill)));
        g.FillPath(&back, &path);
        Pen edge(
            WithAlpha(alarm ? p.accent : p.accent2, (BYTE)(landed_ ? 60 + 150 * fill : 50 + 30 * std::sin(t_ * 3))),
            (landed_ ? 1.2f + 0.8f * fill : 1.2f) * k);
        if (!landed_) edge.SetDashStyle(DashStyleDash);
        g.DrawPath(&edge, &path);
    }
    if (!landed_)
        Label(g, L"проверяю", RectF(ox + sx_ - sw_, oy + sy_ - 10 * k, sw_ * 2, 20 * k), p.muted, 11 * k, false, 1);
    // точки
    Color cols[8] = {p.faint,
                     MixColor(p.faint, p.gold, 0.33),
                     MixColor(p.faint, p.gold, 0.66),
                     p.gold,
                     MixColor(p.accent2, p.faint, 0.35),
                     MixColor(p.gold, p.text, 0.6),
                     p.accent,
                     p.accent};
    Batch b;
    float size = std::max(2.5f, pitch_ * 0.42f);
    for (size_t i = 0; i < dots_.size(); ++i) {
        const Dot& d = dots_[i];
        bool isBad = std::find(bad_.begin(), bad_.end(), (int)i) != bad_.end();
        int col;
        if (landed_ && (since >= d.delay || (isBad && since > 0.35f))) {
            col = d.col;
        } else {
            float fl = beamOn ? std::max(0.0f, 1 - std::fabs(beam - d.y) / (12 * k)) : 0;
            col = std::min(3, (int)(fl * 4));
        }
        float s = size;
        if (isBad && landed_ && since > 0.35f) s *= 1.3f + (since < 3.5f && !game ? 0.25f * std::sin(t_ * 9) : 0);
        b.Add(col, ox + d.x, oy + d.y, s);
    }
    b.Flush(g, cols, 8);
    // итог под щитом и строки находок
    if (landed_) {
        BYTE a = (BYTE)(255 * Ease((since - 0.9f) / 0.5f));
        if (a) {
            std::wstring cnt = Plural(checked_, L"программа", L"программы", L"программ") + L" проверено";
            Label(g, cnt, RectF(ox + sx_ - sw_ - 30 * k, oy + sy_ + sh_ + 2 * k, sw_ * 2 + 60 * k, 18 * k),
                  WithAlpha(p.muted, a), 11 * k, false, 1);
            float y = 150 * k + 4 * k;
            if (finds_.empty())
                Label(g, L"Ничего странного: подписи и автозапуск в порядке",
                      RectF(ox + 4 * k, oy + y, r.Width - 8 * k, 18 * k), WithAlpha(p.accent2, a), 12 * k, true);
            for (size_t j = 0; j < finds_.size() && j < 4; ++j) {
                RectF row(ox, oy + y + j * 24 * k, r.Width, 24 * k);
                if (mouse.x >= row.X && mouse.x < row.GetRight() && mouse.y >= row.Y && mouse.y < row.GetBottom()) {
                    SolidBrush hl(WithAlpha(p.accent, 22));
                    g.FillRectangle(&hl, row);
                }
                const GuardFind& fd = finds_[j];
                float nw = std::min(r.Width * 0.42f, 150 * k);
                Label(g, fd.name, RectF(row.X + 22 * k, row.Y, nw, row.Height), WithAlpha(p.text, a), 12 * k, true);
                Label(g, fd.why, RectF(row.X + 28 * k + nw, row.Y, r.Width - nw - 32 * k, row.Height),
                      WithAlpha(p.muted, a), 11 * k);
            }
            if (finds_.size() > 4)
                Label(g, L"и ещё " + std::to_wstring(finds_.size() - 4),
                      RectF(ox + 22 * k, oy + y + 4 * 24 * k, r.Width, 18 * k), WithAlpha(p.faint, a), 11 * k);
        }
    }
    g.Restore(gs);
    settled_ = landed_ && !moving;
    return moving;
}

}  // namespace scene
