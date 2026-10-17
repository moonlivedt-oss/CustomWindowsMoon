// Раздел «Кольца» живой: оба края - две дорожки рядом, секторы - карточки на них. Карточки тянутся мышью
// (соседи расступаются), переезжают на другой край, слипаются в группу, тонут в корзине крошками; у групп
// стопка, щелчок раскрывает веер пунктов; лента правок внизу откатывает правки на глазах. Набросок -
// art/rings_sketch.html. Пружины считаются в координатах рамки дорожек: прокрутка двигает рамку, не пружины.
#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "core/files.h"
#include "core/fx.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

namespace {

constexpr wchar_t kNewGroup[] = L"Новая группа";

float Back(float t) { return (float)EaseOutBack(t, 1.7); }

// пружина мелкими шагами (явная схема на длинном шаге раскачивается); true - ещё едет
bool Pull(Mover& m, float tx, float ty, float dt, float k = 170, float d = 20) {
    if (fx::Game()) {
        m.x = tx, m.y = ty, m.vx = m.vy = 0;
        return false;
    }
    for (float left = dt; left > 0;) {
        float h = std::min(left, 1 / 240.0f);
        left -= h;
        m.vx += ((tx - m.x) * k - m.vx * d) * h;
        m.vy += ((ty - m.y) * k - m.vy * d) * h;
        m.x += m.vx * h;
        m.y += m.vy * h;
    }
    if (std::fabs(tx - m.x) < 0.3f && std::fabs(ty - m.y) < 0.3f && std::fabs(m.vx) < 4 && std::fabs(m.vy) < 4) {
        m.x = tx, m.y = ty, m.vx = m.vy = 0;
        return false;
    }
    return true;
}

// v тянется к to; true - ещё не дотянулось
bool Fade(float& v, float to, float dt, float speed = 9) {
    if (fx::Game()) {
        v = to;
        return false;
    }
    v += (to - v) * std::min(1.0f, dt * speed);
    if (std::fabs(to - v) < 0.01f) {
        v = to;
        return false;
    }
    return true;
}

// ключ карточки: один и тот же сектор после перестановки и переезда на другой край
std::wstring KeyOf(const Item& it) { return it.name + L"\x1f" + it.path + L"\x1f" + it.icon; }
std::wstring BaseOf(const std::wstring& key) {
    size_t at = key.rfind(L'#');
    return at != std::wstring::npos && key.find(L'\x1f', at) == std::wstring::npos ? key.substr(0, at) : key;
}

int CardId(int L, int i) { return ID_SECTOR + L * 100 + i; }

bool Simple(const Item& it) { return it.children.empty() && !it.expand; }

}  // namespace

void Studio::LaneTick() {
    ULONGLONG now = GetTickCount64();
    float dt = clockAt_ && now - clockAt_ < 250 ? (now - clockAt_) / 1000.0f : 0.016f;
    clockAt_ = now;
    dt *= (float)(1000.0 / std::max(250.0, fx::Dur(1000)));  // «спокойно» / «живо»
    ldt_ = std::min(dt, 0.1f);  // пружины идут мелкими шагами, длинный кадр им не страшен
    clock_ += std::min(dt, 0.25f);  // часы - честные: перелёт и печать имени не тянутся при редких кадрах
}

void Studio::RingsPage(const RectF& r) {
    LaneTick();
    float listW = std::clamp(r.Width * 0.42f, ui.S(420), ui.S(520));
    SectorList(RectF(r.X, r.Y, listW, r.Height));
    ui.Fill(RectF(r.X + listW, r.Y + ui.S(16), 1, r.Height - ui.S(32)), ui.C().border);
    SectorMain(RectF(r.X + listW + 1, r.Y, r.Width - listW - 1, r.Height));
    MorphGhost();
    CrumbsDraw();
}

// Значок с масштабом, поворотом и прозрачностью: готовый размер побольше, вниз - плавно.
void Studio::IconDrawn(const std::wstring& file, const std::wstring& shell, const RectF& r, float alpha) {
    if (r.Width < 1 || alpha <= 0.01f) return;
    int px = r.Width <= ui.S(36) ? (int)ui.S(48) : r.Width <= ui.S(72) ? (int)ui.S(80) : (int)ui.S(128);
    Bitmap* b = ui.IconBitmap(file, shell, px);
    if (!b) return;
    Graphics& g = ui.G();
    InterpolationMode im = g.GetInterpolationMode();
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    if (alpha >= 0.99f) {
        g.DrawImage(b, r);
    } else {
        ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
        ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        g.DrawImage(b, r, 0, 0, (REAL)b->GetWidth(), (REAL)b->GetHeight(), UnitPixel, &ia);
    }
    g.SetInterpolationMode(im);
}

// Карточка сектора: значок, имя, вид и что внутри; у группы - значки пунктов с краю; пропавший путь -
// трещина и жёлтая рамка. smooth - карточку крутят и растят, значок рисуется с пересчётом.
void Studio::LaneCard(const Item& it, int L, int i, const RectF& r, const std::wstring& name, float hot, bool on,
                      int prob, float crack, bool smooth) {
    const UiColors& c = ui.C();
    float rad = ui.S(11);
    ui.Fill(r, on ? c.cardHi : MixColor(c.card, c.cardHi, hot * 0.6f), rad);
    if (on)
        ui.Stroke(r, c.accent, rad, ui.S(1.5f));
    else if (crack > 0.01f)
        ui.Stroke(r, MixColor(c.border, c.warn, 0.7f * crack), rad, ui.S(1.2f));
    else
        ui.Stroke(r, c.border, rad);
    RectF ir(r.X + ui.S(9), r.Y + (r.Height - ui.S(32)) / 2, ui.S(32), ui.S(32));
    if (smooth)
        IconDrawn(IconFile(it), it.full, ir);
    else
        ui.Icon(IconFile(it), ir, it.full);
    std::wstring kind, meta;
    if (it.expand) {
        kind = L"папка";
        meta = std::to_wstring(Stats(it).total) + L" внутри";
    } else if (!it.children.empty()) {
        kind = L"группа";
        meta = Plural((int)it.children.size(), L"пункт", L"пункта", L"пунктов");
    } else if (it.full.empty()) {
        kind = L"пусто";
    } else if (it.full.rfind(L"shell:", 0) == 0) {
        kind = L"система";
    } else if (it.dir) {
        kind = L"папка";
        meta = L"открывается";
    } else {
        kind = EndsWithI(it.full, L".lnk") || EndsWithI(it.full, L".exe") || EndsWithI(it.full, L".url") ? L"программа"
                                                                                                         : L"файл";
    }
    if (prob == 0) meta = L"путь пропал";
    if (prob == 1) meta = L"ничего не открывает";
    if (prob == 2) meta = L"не папка";
    float tx = r.X + ui.S(50), right = r.X + r.Width - ui.S(8);
    if (prob >= 0) right -= ui.S(56);  // место под «найти»
    // значки пунктов группы с краю, если имя не тесно
    float nameW = ui.Measure(name, 13, true);
    int peek = std::min<int>(3, (int)it.children.size());
    float peekW = peek ? ui.S(14) * peek + ui.S(8) : 0;
    if (peek && prob < 0 && tx + nameW + ui.S(8) + peekW <= right) {
        for (int k = peek - 1; k >= 0; --k) {
            const Item& ch = it.children[k];
            RectF pr(right - ui.S(20) - k * ui.S(14), r.Y + (r.Height - ui.S(20)) / 2, ui.S(20), ui.S(20));
            IconDrawn(IconFile(ch), ch.full, pr, 0.85f);
        }
        right -= peekW;
    }
    ui.Text(name, RectF(tx, r.Y + ui.S(6), right - tx, ui.S(20)), c.text, 13, true);
    if (prob >= 0) kind.clear();  // сломанной карточке важнее, что с путём
    float cw = kind.empty() ? -ui.S(6) : ui.Measure(kind, 10.5f, true) + ui.S(12);
    RectF chip(tx, r.Y + ui.S(28), cw, ui.S(16));
    if (!kind.empty()) ui.Chip(chip, kind, MixColor(c.card, c.accent, 0.22f), c.accent);
    ui.Text(meta, RectF(chip.X + cw + ui.S(6), chip.Y - ui.S(1), right - chip.X - cw - ui.S(6), chip.Height + ui.S(2)),
            prob >= 0 ? c.warn : c.faint, 11);
    if (crack > 0.01f) {  // трещина поперёк карточки
        float h = r.Height / 46.0f;
        PointF pts[] = {PointF(r.X + ui.S(60), r.Y), PointF(r.X + ui.S(70), r.Y + 18 * h),
                        PointF(r.X + ui.S(64), r.Y + 26 * h), PointF(r.X + ui.S(76), r.Y + r.Height)};
        Pen pen(WithAlpha(c.warn, (BYTE)(180 * crack)), ui.S(1.4f));
        ui.G().DrawLines(&pen, pts, 4);
    }
    (void)L;
    (void)i;
}

void Studio::SectorList(const RectF& r) {
    const UiColors& c = ui.C();
    float x = r.X + ui.S(18), w = r.Width - ui.S(36);
    float y = r.Y + ui.S(18);
    ui.Text(L"Мои кольца", RectF(x, y, w, ui.S(32)), c.text, 21, true);
    y += ui.S(32);
    ui.Text(L"Тяни карточки: порядок, другой край, на другую - группа", RectF(x, y, w, ui.S(20)), c.muted, 12);
    y += ui.S(30);

    float bottomH = ui.S(132);
    RectF area(r.X + ui.S(10), y, r.Width - ui.S(20), r.Y + r.Height - bottomH - y);
    float gap = ui.S(10), lw = (area.Width - gap) / 2, labelH = ui.S(28);
    for (int L = 0; L < 2; ++L) laneRect_[L] = RectF(area.X + L * (lw + gap), area.Y, lw, area.Height);
    RectF view(area.X, area.Y + labelH, area.Width, area.Height - labelH);
    lanesView_ = view;
    laneCardW_ = lw - ui.S(12);
    laneCardH_ = ui.S(50);
    lanePitch_ = ui.S(58);
    float off = ui.Scroll(ID_LIST_SCROLL, view, laneContentH_);
    lanesOrg_ = PointF(area.X, view.Y - off);
    auto laneX = [&](int L) { return L * (lw + gap) + ui.S(6); };

    // ключи карточек: имя, путь и значок; одинаковые - с номером
    std::vector<std::wstring> keys[2];
    std::map<std::wstring, int> dup;
    for (int L = 0; L < 2; ++L)
        for (const Item& it : Ring(L)) {
            std::wstring b = KeyOf(it);
            int n = dup[b]++;
            keys[L].push_back(n ? b + L"#" + std::to_wstring(n) : b);
        }
    auto problemOf = [&](int L, int i) {
        int kind = -1;
        bool child = false;
        for (const Problem& p : problems_)
            if (p.ring == L && p.i == i) {
                if (p.j < 0)
                    kind = p.kind;
                else
                    child = true;
            }
        return std::make_pair(kind, child);
    };

    // кого тянут: карточку, пункт раскрытой стопки или пункт из вкладки «Второе кольцо»
    POINT m = ui.Mouse();
    int dragL = -1, dragI = -1, openL = -1, openI = -1, dragJ = -1;
    for (int L = 0; L < 2; ++L)
        for (int i = 0; i < (int)Ring(L).size() && i < 100; ++i) {
            if (ui.Dragging(CardId(L, i))) dragL = L, dragI = i;
            if (keys[L][i] == openStack_ && !Ring(L)[i].children.empty()) openL = L, openI = i;
        }
    if (openL < 0) openStack_.clear();
    if (dragL >= 0 && dragL == openL && dragI == openI) openL = openI = -1;  // стопку унесли - веер закрыт
    if (openL >= 0)
        for (int j = 0; j < (int)Ring(openL)[openI].children.size(); ++j)
            if (ui.Dragging(ID_LANE_MINI + j)) dragJ = j;
    // кадр отпускания: Dragging уже погас, а цель (дорожка, щель, группа, корзина) нужна сейчас
    bool released = !ui.Down() && dragL < 0 && dragJ < 0;
    if (released && lastDragL_ >= 0 && lastDragL_ < 2 && lastDragI_ < (int)Ring(lastDragL_).size())
        dragL = lastDragL_, dragI = lastDragI_;
    if (released && lastDragJ_ >= 0 && openL >= 0 && lastDragJ_ < (int)Ring(openL)[openI].children.size())
        dragJ = lastDragJ_;
    lastDragL_ = released ? -1 : dragL;
    lastDragI_ = released ? -1 : dragI;
    lastDragJ_ = released ? -1 : dragJ;
    bool tabChild = childDragShown_ >= 0;
    bool anyDrag = dragL >= 0 || dragJ >= 0 || tabChild;
    if (!ui.Down()) pressKey_.clear();

    // раскладка: y каждой карточки без той, что тянут; ins - щель, куда она встанет
    float miniH = ui.S(34), miniPitch = ui.S(40), miniW = (laneCardW_ - ui.S(8)) / 2;
    auto fanRows = [&](int L, int i) {
        return L == openL && i == openI ? ((int)Ring(L)[i].children.size() + 1) / 2 : 0;
    };
    auto place = [&](int L, int ins, std::vector<float>& ys) {
        int n = (int)Ring(L).size();
        ys.assign(n, -1);
        float yy = ui.S(4);
        int slot = 0;
        for (int i = 0; i < n; ++i) {
            if (L == dragL && i == dragI) continue;
            if (slot == ins) yy += lanePitch_;
            ys[i] = yy;
            yy += lanePitch_;
            int rows = fanRows(L, i);
            if (rows) yy += rows * miniPitch + ui.S(4);
            ++slot;
        }
        if (slot == ins) yy += lanePitch_;
        return yy;
    };
    laneHot_ = -1;
    laneIns_ = -1;
    mergeKey_.clear();
    overBin_ = false;
    std::vector<float> ys[2];
    for (int L = 0; L < 2; ++L) {
        place(L, -1, ys[L]);
        laneSlotY_[L] = ys[L];
    }
    if (anyDrag) {
        float bx = binRect_.X + binRect_.Width / 2, by = binRect_.Y + binRect_.Height / 2;
        overBin_ = !tabChild && std::hypot(m.x - bx, m.y - by) < ui.S(62);
        if (!overBin_)
            for (int L = 0; L < 2; ++L)
                if (Grow(laneRect_[L], ui.S(6)).Contains((REAL)m.x, (REAL)m.y)) laneHot_ = L;
        if (laneHot_ >= 0) {
            int L = laneHot_;
            float center = dragL >= 0 ? m.y - grab_.Y + laneCardH_ / 2 - lanesOrg_.Y : m.y - lanesOrg_.Y;
            float my = m.y - lanesOrg_.Y;
            int ins = 0;
            for (int i = 0; i < (int)Ring(L).size(); ++i) {
                if (ys[L][i] < 0) continue;
                float rel = (my - ys[L][i]) / laneCardH_;
                if (dragL >= 0 && Simple(Ring(dragL)[dragI]) && rel > 0.26f && rel < 0.74f) {
                    mergeKey_ = keys[L][i];
                    break;
                }
                if (ys[L][i] + laneCardH_ / 2 < center) ++ins;
            }
            if (mergeKey_.empty()) laneIns_ = ins;
        }
    }
    if (laneIns_ >= 0) place(laneHot_, laneIns_, ys[laneHot_]);
    laneContentH_ = 0;
    for (int L = 0; L < 2; ++L) {
        float h = ui.S(8);
        for (int i = 0; i < (int)Ring(L).size(); ++i)
            if (ys[L][i] >= 0) h = std::max(h, ys[L][i] + lanePitch_ + fanRows(L, i) * miniPitch);
        if (laneHot_ == L && laneIns_ >= 0) h += lanePitch_;
        laneContentH_ = std::max(laneContentH_, h + ui.S(8));
    }

    // пружины: новые ключи - по подсказке (влетела из Проводника, вытащили из стопки), по месту
    // (переименовали) или проступают на месте; ушедшие - забываются
    bool first = laneCards_.empty();
    for (auto& [k, mv] : laneCards_) mv.live = false;
    std::map<std::pair<int, int>, std::wstring> gone;
    std::set<std::wstring> now;
    for (int L = 0; L < 2; ++L)
        for (auto& k : keys[L]) now.insert(k);
    for (auto& [k, mv] : laneCards_)
        if (!now.count(k)) gone[{mv.ring, mv.idx}] = k;
    bool moving = false;
    for (int L = 0; L < 2; ++L)
        for (int i = 0; i < (int)Ring(L).size(); ++i) {
            const std::wstring& key = keys[L][i];
            float tx = laneX(L), ty = ys[L][i] >= 0 ? ys[L][i] : 0;
            auto f = laneCards_.find(key);
            if (f == laneCards_.end()) {
                Mover mv;
                auto h = laneHints_.find(BaseOf(key));
                auto g = gone.find({L, i});
                if (h != laneHints_.end()) {
                    mv = h->second;
                    laneHints_.erase(h);
                } else if (g != gone.end() && laneCards_.count(g->second)) {
                    mv = laneCards_[g->second];
                    laneCards_.erase(g->second);
                    gone.erase(g);
                } else {
                    mv.x = tx, mv.y = ty;
                    mv.a = first ? 1.0f : 0.0f;
                    mv.pop = first ? 0.0f : 1.0f;
                }
                if (key == binKey_) {  // вернули отменой: собирается из крошек
                    mv.x = tx, mv.y = ty, mv.a = 0, mv.pop = 0, mv.wait = fx::Game() ? 0.0f : 0.7f;
                    binKey_.clear();
                    crumbsBack_ = true;
                    crumbsAt_ = clock_;
                    RectF to(lanesOrg_.X + tx, lanesOrg_.Y + ty, laneCardW_, laneCardH_);
                    float bx = binRect_.X + binRect_.Width / 2, by = binRect_.Y + binRect_.Height / 2;
                    crumbs_.clear();
                    for (int q = 0; q < 110; ++q)
                        crumbs_.push_back({bx + scene::Rnd(-10, 10) * (float)ui.K(), by,
                                           scene::Rnd(-150, 150) * (float)ui.K(), -scene::Rnd(200, 400) * (float)ui.K(),
                                           to.X + scene::Rnd(0, to.Width), to.Y + scene::Rnd(0, to.Height), 0,
                                           (BYTE)(q % 5)});
                }
                f = laneCards_.emplace(key, mv).first;
            }
            Mover& mv = f->second;
            mv.live = true;
            mv.ring = L, mv.idx = i;
            bool broken = problemOf(L, i).first == 0;
            if (L == dragL && i == dragI) {
                mv.x = m.x - grab_.X - lanesOrg_.X;
                mv.y = m.y - grab_.Y - lanesOrg_.Y;
                mv.vx = mv.vy = 0;
            } else {
                moving |= Pull(mv, tx, ty, ldt_, broken ? 140.0f : 170.0f, broken ? 14.0f : 20.0f);
            }
            if (mv.wait > 0) {
                mv.wait -= ldt_;
                moving = true;
                if (mv.wait <= 0) mv.a = 1, mv.pop = 1;
            } else {
                moving |= Fade(mv.a, 1, ldt_, 8);
            }
            if (!broken && mv.crack > 0.95f) {  // нашёлся путь: карточка срастается с толчком
                mv.vx -= 60 * (float)ui.K();
                mv.pop = 1;
            }
            moving |= Fade(mv.crack, broken ? 1.0f : 0.0f, ldt_, 6);
            if (mv.pop > 0) {
                mv.pop = std::max(0.0f, mv.pop - ldt_ * 2.2f);
                moving = true;
            }
        }
    for (auto it = laneCards_.begin(); it != laneCards_.end();)
        it = it->second.live ? std::next(it) : laneCards_.erase(it);
    if (laneHints_.size() > 16) laneHints_.clear();

    // дорожки
    for (int L = 0; L < 2; ++L) {
        RectF lr = laneRect_[L];
        bool hot = laneHot_ == L && (dragL < 0 || dragL != L || dragJ >= 0 || tabChild) && mergeKey_.empty();
        float ha = ui.Anim(ID_LANE_ANIM + 900 + L, hot, 10);
        ui.Fill(lr, MixColor(MixColor(c.win, c.card, 0.45f), c.accent2, 0.07f * ha), ui.S(12));
        if (ha > 0.01f) ui.Stroke(lr, WithAlpha(c.accent2, (BYTE)(140 * ha)), ui.S(12), ui.S(1.2f), true);
        ui.Text(L ? L"Правый край" : L"Левый край", RectF(lr.X + ui.S(12), lr.Y + ui.S(4), lr.Width, ui.S(22)), c.muted,
                12, true);
        std::wstring cnt = std::to_wstring(Ring(L).size());
        ui.Text(cnt, RectF(lr.X, lr.Y + ui.S(4), lr.Width - ui.S(12), ui.S(22)), c.faint, 12, false, ui::Right);
    }

    // карточки
    float t = (float)clock_;
    bool burst = std::sin(t * 1.3f) > 0.92f;
    bool anyBroken = false;
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    // слипшиеся: карточка сжимается в ту, на которую её бросили
    for (auto it = sinks_.begin(); it != sinks_.end();) {
        it->t += ldt_ / 0.28f;
        auto to = laneCards_.find(it->into);
        if (it->t >= 1 || fx::Game()) {
            it = sinks_.erase(it);
            continue;
        }
        if (to != laneCards_.end()) {
            it->x += (to->second.x - it->x) * std::min(1.0f, ldt_ * 14);
            it->y += (to->second.y - it->y) * std::min(1.0f, ldt_ * 14);
        }
        float s = 1 - 0.25f * it->t;
        RectF cr(lanesOrg_.X + it->x, lanesOrg_.Y + it->y, laneCardW_, laneCardH_);
        GraphicsState cs = ui.G().Save();
        ui.G().TranslateTransform(cr.X + cr.Width / 2, cr.Y + cr.Height / 2);
        ui.G().ScaleTransform(s, s);
        ui.G().TranslateTransform(-(cr.X + cr.Width / 2), -(cr.Y + cr.Height / 2));
        LaneCard(it->it, -1, -1, cr, it->it.name, 0, false, -1, 0, true);
        ui.G().Restore(cs);
        moving = true;
        ++it;
    }
    int clickL = -1, clickI = -1;
    RectF clickRect;
    for (int L = 0; L < 2; ++L)
        for (int i = 0; i < (int)Ring(L).size() && i < 100; ++i) {
            if (L == dragL && i == dragI) continue;
            const Item& it = Ring(L)[i];
            Mover& mv = laneCards_[keys[L][i]];
            auto [prob, childProb] = problemOf(L, i);
            float sx = 0;
            if (mv.crack > 0.5f && !fx::Game()) {
                anyBroken = true;
                if (burst) sx = std::sin(t * 30) * ui.S(2.5f);
            }
            RectF cr(lanesOrg_.X + mv.x + sx, lanesOrg_.Y + mv.y, laneCardW_, laneCardH_);
            RectF vis = Intersect(cr, view);
            int id = CardId(L, i);
            bool live = vis.Height > 1 && mv.a > 0.5f;
            bool on = ring_ == L && sel_ == i;
            // стопка: тени карточек под группой; раскрыли - тени уходят под неё
            if (!it.children.empty()) {
                float open = ui.Anim(ID_LANE_ANIM + L * 100 + i, L == openL && i == openI, 12);
                for (int k = 2; k >= 1; --k) {
                    float d = ui.S(4) * k * (1 - open);
                    float al = (1 - k * 0.35f) * (1 - open) * mv.a;
                    if (al < 0.02f) continue;
                    RectF gr(cr.X + d, cr.Y + d, cr.Width, cr.Height);
                    ui.Fill(gr, WithAlpha(MixColor(c.card, c.win, 0.25f), (BYTE)(255 * al)), ui.S(11));
                    ui.Stroke(gr, WithAlpha(MixColor(c.border, c.accent2, 0.25f), (BYTE)(255 * al)), ui.S(11));
                }
            }
            if (live && ui.Click(id, vis) && std::abs(m.x - ui.PressPt().x) + std::abs(m.y - ui.PressPt().y) <= ui.S(6))
                clickL = L, clickI = i, clickRect = cr;
            if (live && ui.Active(id) && pressKey_ != keys[L][i]) {
                pressKey_ = keys[L][i];
                grab_ = PointF(ui.PressPt().x - cr.X, ui.PressPt().y - cr.Y);
            }
            if (live && ui.RightClick(id, vis)) {
                ring_ = L, sel_ = i;
                ov_ = OV_MORE;
                moreAtMouse_ = true;
                moreAnchor_ = RectF((REAL)m.x, (REAL)m.y, 0, 0);
            }
            if (live && ui.DoubleClick(id)) {
                ring_ = L, sel_ = i;
                renaming_ = i;
                renameFocus_ = true;
            }
            float hov = ui.Anim(id, ui.Hot(id));
            float sc = 1 + std::sin((1 - mv.pop) * 3.14159f) * mv.pop * 0.07f;
            bool tf = sc != 1 || mv.a < 1;
            GraphicsState cs = 0;
            if (tf) {
                cs = ui.G().Save();
                ui.G().TranslateTransform(cr.X + cr.Width / 2, cr.Y + cr.Height / 2);
                ui.G().ScaleTransform(sc * (0.9f + 0.1f * mv.a), sc * (0.9f + 0.1f * mv.a));
                ui.G().TranslateTransform(-(cr.X + cr.Width / 2), -(cr.Y + cr.Height / 2));
            }
            std::wstring name = it.name;
            if (L == typingRing_ && i == typingIdx_) {  // имя новой группы печатается само
                int n = (int)((clock_ - typingAt_) / 0.055);
                if (n < (int)name.size()) {
                    name = name.substr(0, std::max(0, n)) + L"|";
                    moving = true;
                } else {
                    typingRing_ = -1;
                    if (ring_ == L && sel_ == i) {
                        renaming_ = i;
                        renameFocus_ = true;
                    }
                }
            }
            if (renaming_ == i && ring_ == L) name.clear();
            LaneCard(it, L, i, cr, name, hov, on, prob, mv.crack, tf);
            if (childProb && prob < 0) {
                SolidBrush wb(c.warn);
                ui.G().FillEllipse(&wb, cr.X + cr.Width - ui.S(14), cr.Y + ui.S(8), ui.S(7), ui.S(7));
            }
            if (keys[L][i] == mergeKey_) {  // ореол: отпусти - будет группа
                ui.Stroke(Grow(cr, ui.S(2)), WithAlpha(c.accent2, 170), ui.S(13), ui.S(2));
                ui.Stroke(Grow(cr, ui.S(6)), WithAlpha(c.accent2, 50), ui.S(17), ui.S(5));
            }
            if (mv.a < 1) ui.Fill(Grow(cr, ui.S(1)), WithAlpha(c.win, (BYTE)(255 * (1 - mv.a))), ui.S(12));
            if (tf) ui.G().Restore(cs);
            if (prob >= 0 && live) {
                RectF fb(cr.X + cr.Width - ui.S(60), cr.Y + (cr.Height - ui.S(24)) / 2, ui.S(52), ui.S(24));
                int fid = ID_LANE_FIND + L * 100 + i;
                bool fh = ui.Click(fid, Intersect(fb, view));
                ui.Fill(fb, MixColor(c.warnBg, c.warn, ui.Hot(fid) ? 0.32f : 0.18f), ui.S(7));
                ui.Text(prob == 0 ? L"найти" : L"указать", fb, c.warn, 11, true, ui::Center);
                ui.Tip(fid, prob == 0 ? L"Поищу папку с тем же именем выше по пути и на рабочем столе"
                                      : L"Открою этот сектор, чтобы задать путь");
                if (fh) {
                    Problem p{L, i, -1, it.path, prob};
                    ring_ = L, sel_ = i;
                    if (prob == 0) {
                        FixProblem(p);
                    } else {
                        tab_ = 0;
                        focusPath_ = true;
                    }
                }
            }
            if (renaming_ == i && ring_ == L) {
                RectF fr(cr.X + ui.S(46), cr.Y + ui.S(3), cr.Width - ui.S(54), ui.S(26));
                int rr = L, ii = i;
                if (Inside(fr, view)) {
                    ui.Field(ID_RENAME, fr, it.name, [this, rr, ii](const std::wstring& s) {
                        FieldEdit(ID_RENAME);
                        Ring(rr)[ii].name = s;
                    });
                    if (renameFocus_) {
                        ui.FocusField(ID_RENAME);
                        renameFocus_ = false;
                        renameActive_ = true;
                    } else if (renameActive_ && !ui.FieldFocused(ID_RENAME)) {
                        renaming_ = -1;  // ввод закончен: Enter, Esc или клик мимо
                        renameActive_ = false;
                    }
                } else {
                    renaming_ = -1;
                }
            }
            if (live)
                ui.Tip(id, (it.full.empty() ? std::wstring() : it.full + L"\n") +
                               (it.children.empty() ? L"Щелчок - выбрать, двойной - переименовать, тяни - порядок, "
                                                      L"край, группа; ПКМ - действия"
                                                    : L"Щелчок - раскрыть стопку, тяни пункт на дорожку - станет "
                                                      L"сектором; ПКМ - действия"));
        }

    // веер раскрытой стопки; пункты закрытой стопки уходят обратно под карточку
    std::set<std::wstring> fanSeen;
    if (openL >= 0) {
        const Item& g = Ring(openL)[openI];
        const std::wstring& gk = keys[openL][openI];
        Mover& gm = laneCards_[gk];
        for (int j = 0; j < (int)g.children.size(); ++j) {
            const Item& ch = g.children[j];
            std::wstring mk = gk + L"\x1e" + std::to_wstring(j) + L"\x1e" + ch.name;
            fanSeen.insert(mk);
            auto f = laneMinis_.find(mk);
            if (f == laneMinis_.end()) {
                Mover mv;
                mv.x = gm.x + laneCardW_ - miniW, mv.y = gm.y + ui.S(8), mv.a = 0;
                f = laneMinis_.emplace(mk, mv).first;
            }
            Mover& mv = f->second;
            float tx = gm.x + (j % 2) * (miniW + ui.S(8)),
                  ty = ys[openL][openI] + laneCardH_ + ui.S(6) + (j / 2) * miniPitch;
            int id = ID_LANE_MINI + j;
            RectF mr(lanesOrg_.X + mv.x, lanesOrg_.Y + mv.y, miniW, miniH);
            if (j == dragJ) {
                mv.x = m.x - grab_.X - lanesOrg_.X;
                mv.y = m.y - grab_.Y - lanesOrg_.Y;
                mv.vx = mv.vy = 0;
                continue;  // рисуется поверх всех
            }
            moving |= Pull(mv, tx, ty, ldt_, 190, 17);
            moving |= Fade(mv.a, 1, ldt_, 10);
            RectF vis = Intersect(mr, view);
            if (vis.Height > 1) {
                ui.Hover(id, vis);
                if (ui.Active(id) && pressKey_ != mk) {
                    pressKey_ = mk;
                    grab_ = PointF(ui.PressPt().x - mr.X, ui.PressPt().y - mr.Y);
                }
                ui.Tip(id, ch.full.empty() ? ch.name : ch.full + L"\nТяни на дорожку - станет отдельным сектором");
            }
            bool bad = false;
            for (const Problem& p : problems_)
                if (p.ring == openL && p.i == openI && p.j == j) bad = true;
            ui.Fill(mr, WithAlpha(ui.Hot(id) ? c.cardHi : MixColor(c.card, c.cardHi, 0.5f), (BYTE)(255 * mv.a)),
                    ui.S(9));
            ui.Stroke(mr, WithAlpha(bad ? c.warn : c.border, (BYTE)(255 * mv.a)), ui.S(9));
            IconDrawn(IconFile(ch), ch.full, RectF(mr.X + ui.S(7), mr.Y + (miniH - ui.S(22)) / 2, ui.S(22), ui.S(22)),
                      mv.a);
            ui.Text(ch.name, RectF(mr.X + ui.S(34), mr.Y, miniW - ui.S(40), miniH),
                    WithAlpha(c.text, (BYTE)(255 * mv.a)), 11.5f, true);
        }
    }
    for (auto it = laneMinis_.begin(); it != laneMinis_.end();) {
        if (fanSeen.count(it->first)) {
            ++it;
            continue;
        }
        std::wstring gk = it->first.substr(0, it->first.find(L'\x1e'));
        auto g = laneCards_.find(gk);
        Mover& mv = it->second;
        if (g != laneCards_.end())
            moving |= Pull(mv, g->second.x + laneCardW_ - miniW, g->second.y + ui.S(8), ldt_, 190, 17);
        if (!Fade(mv.a, 0, ldt_, 12) || g == laneCards_.end()) {
            it = laneMinis_.erase(it);
            continue;
        }
        moving = true;
        RectF mr(lanesOrg_.X + mv.x, lanesOrg_.Y + mv.y, miniW, miniH);
        ui.Fill(mr, WithAlpha(c.card, (BYTE)(255 * mv.a)), ui.S(9));
        ++it;
    }

    ui.G().Restore(st);

    // корзина и лента правок внизу
    float by = r.Y + r.Height - bottomH + ui.S(12);
    Tape(RectF(x, by, w, ui.S(44)));
    by += ui.S(56);
    binRect_ = RectF(x + w - ui.S(60), by, ui.S(60), ui.S(60));
    {
        float nb = ui.Anim(ID_LANE_BIN, overBin_, 14);
        float pop = std::max(0.0f, 1 - (float)(clock_ - binPopAt_) / 0.3f);
        if (pop > 0) moving = true;
        float s = 1 + 0.18f * nb + 0.2f * std::sin(pop * 3.14159f);
        bool showBin = anyDrag || !crumbs_.empty() || nb > 0.01f;
        float ba = ui.Anim(ID_LANE_ANIM + 991, showBin, 10);
        ui.Fill(binRect_, MixColor(MixColor(c.win, c.card, 0.6f), c.accent, 0.16f * nb), ui.S(16));
        ui.Stroke(binRect_, MixColor(c.border, c.accent, 0.6f * std::max(nb, ba * 0.4f)), ui.S(16), ui.S(1.2f),
                  ba < 0.5f);
        RectF ir(binRect_.X + ui.S(10), binRect_.Y + ui.S(10), ui.S(40), ui.S(40));
        GraphicsState bs = ui.G().Save();
        ui.G().TranslateTransform(ir.X + ir.Width / 2, ir.Y + ir.Height / 2);
        ui.G().RotateTransform(-6 * nb);
        ui.G().ScaleTransform(s, s);
        ui.G().TranslateTransform(-(ir.X + ir.Width / 2), -(ir.Y + ir.Height / 2));
        IconDrawn(ExeDir() + L"\\icons\\recycle.png", L"", ir, 0.55f + 0.45f * std::max(nb, ba));
        ui.G().Restore(bs);
        ui.Hover(ID_LANE_BIN, binRect_);
        ui.Tip(ID_LANE_BIN,
               L"Брось сюда сектор или пункт стопки - удалится. «Вернуть» на всплывашке или Ctrl+Z "
               L"соберёт его на прежнем месте.");
    }
    if (ui.Button(ID_ADD_SECTOR, RectF(x, by, w - ui.S(72), ui.S(36)), L"+ Сектор", Kind::Secondary)) {
        Change();
        Item it;
        it.name = L"Новый сектор";
        Ring(ring_).push_back(it);
        sel_ = (int)Ring(ring_).size() - 1;
        tab_ = 0;
        renaming_ = sel_;
        renameFocus_ = true;
        Log(L"Добавлен сектор");
        Save();
    }
    ui.Tip(ID_ADD_SECTOR,
           L"Новый сектор в конце выбранного края. Ещё быстрее - перетащить папку из Проводника "
           L"прямо на дорожку.");
    ui.Text(L"или перетащи папку из Проводника на дорожку", RectF(x, by + ui.S(40), w - ui.S(72), ui.S(20)), c.faint,
            11, false, ui::Center);

    // поверх всего: карточка или пункт в руке
    if (dragL >= 0) {
        const Item& it = Ring(dragL)[dragI];
        Mover& mv = laneCards_[keys[dragL][dragI]];
        RectF cr(lanesOrg_.X + mv.x, lanesOrg_.Y + mv.y, laneCardW_, laneCardH_);
        float binA = ui.Anim(ID_LANE_ANIM + 990, overBin_, 14);
        ui.Fill(RectF(cr.X + ui.S(3), cr.Y + ui.S(9), cr.Width, cr.Height), Color(110, 0, 0, 0), ui.S(12));
        GraphicsState cs = ui.G().Save();
        ui.G().TranslateTransform(cr.X + cr.Width / 2, cr.Y + cr.Height / 2);
        ui.G().RotateTransform(-1.5f + 3.5f * binA);
        float s = 1.03f - 0.06f * binA - (mergeKey_.empty() ? 0 : 0.03f);
        ui.G().ScaleTransform(s, s);
        ui.G().TranslateTransform(-(cr.X + cr.Width / 2), -(cr.Y + cr.Height / 2));
        LaneCard(it, dragL, dragI, cr, it.name, 1, true, problemOf(dragL, dragI).first, mv.crack, true);
        ui.G().Restore(cs);
    }
    if (dragJ >= 0) {
        const Item& ch = Ring(openL)[openI].children[dragJ];
        std::wstring mk = keys[openL][openI] + L"\x1e" + std::to_wstring(dragJ) + L"\x1e" + ch.name;
        Mover& mv = laneMinis_[mk];
        RectF mr(lanesOrg_.X + mv.x, lanesOrg_.Y + mv.y, miniW, miniH);
        ui.Fill(RectF(mr.X + ui.S(3), mr.Y + ui.S(7), mr.Width, mr.Height), Color(100, 0, 0, 0), ui.S(9));
        GraphicsState cs = ui.G().Save();
        ui.G().TranslateTransform(mr.X + mr.Width / 2, mr.Y + mr.Height / 2);
        ui.G().ScaleTransform(1.06f, 1.06f);
        ui.G().TranslateTransform(-(mr.X + mr.Width / 2), -(mr.Y + mr.Height / 2));
        ui.Fill(mr, c.cardHi, ui.S(9));
        ui.Stroke(mr, c.accent, ui.S(9), ui.S(1.2f));
        IconDrawn(IconFile(ch), ch.full, RectF(mr.X + ui.S(7), mr.Y + (miniH - ui.S(22)) / 2, ui.S(22), ui.S(22)));
        ui.Text(ch.name, RectF(mr.X + ui.S(34), mr.Y, miniW - ui.S(40), miniH), c.text, 11.5f, true);
        ui.G().Restore(cs);
    }
    if (anyDrag) moving = true;

    // щелчок: выбрать; по группе - ещё и раскрыть или закрыть стопку
    if (clickL >= 0) {
        bool same = ring_ == clickL && sel_ == clickI;
        const Item& it = Ring(clickL)[clickI];
        if (!it.children.empty())
            openStack_ = same && openStack_ == keys[clickL][clickI] ? L"" : keys[clickL][clickI];
        else if (!same)
            openStack_.clear();
        if (!same) {
            ring_ = clickL, sel_ = clickI;
            tab_ = 0;
            ui.ResetScroll(ID_MAIN_SCROLL);
            morphFrom_ = clickRect;
            morphAt_ = clock_;
        }
    }

    // отпустили карточку
    for (int L = 0; L < 2; ++L)
        for (int i = 0; i < (int)Ring(L).size() && i < 100; ++i) {
            if (!ui.Released(CardId(L, i))) continue;
            if (std::abs(m.y - ui.PressPt().y) + std::abs(m.x - ui.PressPt().x) <= ui.S(6)) break;
            auto& from = Ring(L);
            Mover& mv = laneCards_[keys[L][i]];
            std::wstring name = from[i].name;
            if (overBin_) {
                ring_ = L, sel_ = i;
                LaneCrumble(L, i);
                Change();
                from.erase(from.begin() + i);
                sel_ = std::clamp(sel_, 0, std::max(0, (int)from.size() - 1));
                Toast(L"Сектор «" + name + L"» удалён");
                toastBtn_ = L"Вернуть";
                Save();
            } else if (!mergeKey_.empty() && laneHot_ >= 0) {
                int T = laneHot_, ti = (int)(std::find(keys[T].begin(), keys[T].end(), mergeKey_) - keys[T].begin());
                if (ti >= (int)Ring(T).size()) break;
                Change();
                Item moving = from[i];
                sinks_.push_back({moving, L"", mv.x, mv.y, 0});
                Item& t = Ring(T)[ti];
                Mover tm = laneCards_[mergeKey_];
                tm.pop = 1;
                if (Simple(t)) {
                    Item g;
                    g.name = kNewGroup;
                    g.icon = L"icons/hub.png";
                    g.children = {t, moving};
                    Resolve(g);
                    t = g;
                    Toast(L"Группа из 2: переименуй, как удобно");
                } else {
                    t.children.push_back(moving);
                    Toast(L"«" + moving.name + L"» теперь пункт в «" + t.name + L"»");
                }
                laneHints_[KeyOf(t)] = tm;
                sinks_.back().into = KeyOf(t);
                bool typing = t.name == kNewGroup && t.children.size() == 2;
                from.erase(from.begin() + i);
                if (T == L && i < ti) --ti;
                ring_ = T, sel_ = ti, tab_ = 1;
                openStack_.clear();
                if (typing) typingRing_ = T, typingIdx_ = ti, typingAt_ = clock_;
                Save();
            } else if (laneHot_ >= 0 && laneIns_ >= 0) {
                int T = laneHot_, to = laneIns_;
                if (T == L && to == i) break;
                Change();
                Item moving = from[i];
                from.erase(from.begin() + i);
                Ring(T).insert(Ring(T).begin() + std::min(to, (int)Ring(T).size()), moving);
                ring_ = T, sel_ = std::min(to, (int)Ring(T).size() - 1);
                if (T != L)
                    Toast(L"«" + name + L"» перенесён на " + (T ? L"правый край" : L"левый край"));
                else
                    Log(L"Порядок секторов изменён");
                Save();
            }
            break;
        }
    // отпустили пункт стопки: на дорожке - отдельный сектор, в корзине - удалён
    if (openL >= 0)
        for (int j = 0; j < (int)Ring(openL)[openI].children.size(); ++j) {
            if (!ui.Released(ID_LANE_MINI + j)) continue;
            if (std::abs(m.y - ui.PressPt().y) + std::abs(m.x - ui.PressPt().x) <= ui.S(6)) break;
            Item& g = Ring(openL)[openI];
            std::wstring mk = keys[openL][openI] + L"\x1e" + std::to_wstring(j) + L"\x1e" + g.children[j].name;
            Mover mv = laneMinis_[mk];
            if (overBin_) {
                RectF mr(lanesOrg_.X + mv.x, lanesOrg_.Y + mv.y, miniW, miniH);
                crumbs_.clear();
                crumbsBack_ = false;
                crumbsAt_ = clock_;
                for (int q = 0; q < 60; ++q) {
                    float ox = scene::Rnd(0, mr.Width), oy = scene::Rnd(0, mr.Height);
                    crumbs_.push_back({mr.X + ox, mr.Y + oy, scene::Rnd(-80, 80) * (float)ui.K(),
                                       -scene::Rnd(0, 160) * (float)ui.K(), mr.X + ox, mr.Y + oy, scene::Rnd(0, 0.25f),
                                       (BYTE)(q % 5)});
                }
                Change();
                std::wstring n = g.children[j].name;
                g.children.erase(g.children.begin() + j);
                laneMinis_.erase(mk);
                Toast(L"Пункт «" + n + L"» удалён");
                Save();
            } else if (laneHot_ >= 0 && laneIns_ >= 0) {
                Change();
                Item moving = g.children[j];
                g.children.erase(g.children.begin() + j);
                laneMinis_.erase(mk);
                mv.a = 1, mv.pop = 1;
                laneHints_[KeyOf(moving)] = mv;
                int T = laneHot_, to = std::min(laneIns_, (int)Ring(T).size());
                Ring(T).insert(Ring(T).begin() + to, moving);
                ring_ = T, sel_ = to, tab_ = 0;
                Toast(L"«" + moving.name + L"» теперь отдельный сектор");
                Save();
            }
            break;
        }

    if (anyBroken && !fx::Game()) {  // трещина вздрагивает урывками: кадры только на время дрожи
        if (burst) {
            moving = true;
        } else {
            double ph = std::fmod(clock_ * 1.3, 2 * 3.14159265), start = 1.1681;
            double wait = (ph < start ? start - ph : 2 * 3.14159265 - ph + start) / 1.3;
            RepaintIn((UINT)std::max(16.0, wait * 1000 / std::max(0.25, 1000.0 / fx::Dur(1000))));
        }
    }
    if (moving) ui.Invalidate();
}

// Сектор рассыпается крошками, их затягивает в корзину; ключ запоминается - отмена соберёт его обратно.
void Studio::LaneCrumble(int L, int i) {
    if (L < 0 || L > 1 || i < 0 || i >= (int)Ring(L).size()) return;
    std::map<std::wstring, int> dup;
    std::wstring key;
    for (int r = 0; r < 2; ++r)
        for (int q = 0; q < (int)Ring(r).size(); ++q) {
            std::wstring b = KeyOf(Ring(r)[q]);
            int n = dup[b]++;
            if (r == L && q == i) key = n ? b + L"#" + std::to_wstring(n) : b;
        }
    binKey_ = key;
    auto f = laneCards_.find(key);
    if (f == laneCards_.end() || laneCardW_ <= 0) return;
    RectF cr(lanesOrg_.X + f->second.x, lanesOrg_.Y + f->second.y, laneCardW_, laneCardH_);
    crumbs_.clear();
    crumbsBack_ = false;
    crumbsAt_ = clock_;
    for (int q = 0; q < 110; ++q) {
        float ox = scene::Rnd(0, cr.Width), oy = scene::Rnd(0, cr.Height);
        crumbs_.push_back({cr.X + ox, cr.Y + oy, scene::Rnd(-80, 80) * (float)ui.K(),
                           -scene::Rnd(0, 160) * (float)ui.K(), cr.X + ox, cr.Y + oy, scene::Rnd(0, 0.25f),
                           (BYTE)(q % 5)});
    }
    laneCards_.erase(f);
    ui.Invalidate();
}

void Studio::CrumbsDraw() {
    if (crumbs_.empty()) return;
    if (fx::Game()) {
        crumbs_.clear();
        return;
    }
    const UiColors& c = ui.C();
    Color cols[5] = {c.accent, scene::Pale(ScenePalette()), c.accent2, c.border, c.cardHi};
    float bx = binRect_.X + binRect_.Width / 2, by = binRect_.Y + binRect_.Height / 2;
    double age = clock_ - crumbsAt_;
    scene::Batch batch;
    bool arrived = false;
    for (auto it = crumbs_.begin(); it != crumbs_.end();) {
        Crumb& p = *it;
        float tx = crumbsBack_ ? p.hx : bx, ty = crumbsBack_ ? p.hy : by;
        float k = crumbsBack_ ? 60.0f : 14.0f, d = crumbsBack_ ? 9.0f : 2.5f;
        if (p.delay > 0) {
            p.delay -= ldt_;
        } else {
            for (float left = ldt_; left > 0;) {
                float h = std::min(left, 1 / 240.0f);
                left -= h;
                p.vx += ((tx - p.x) * k - p.vx * d) * h;
                p.vy += ((ty - p.y) * k - p.vy * d) * h;
                p.x += p.vx * h;
                p.y += p.vy * h;
            }
        }
        float dist = std::hypot(p.x - bx, p.y - by);
        if (!crumbsBack_ && (dist < ui.S(5) || age > 2.6)) {
            arrived = true;
            it = crumbs_.erase(it);
            continue;
        }
        float size = ui.S(3) * (crumbsBack_ ? 1.0f : std::min(1.0f, dist / ui.S(40)));
        batch.Add(p.col, p.x, p.y, std::max(1.0f, size));
        ++it;
    }
    if (arrived && clock_ - binPopAt_ > 0.3) binPopAt_ = clock_;
    if (crumbsBack_ && age > 0.75) crumbs_.clear();
    batch.Flush(ui.G(), cols, 5);
    ui.Invalidate();
}

// Выбранная карточка взлетает и растёт в заголовок справа: фон тает, значок и имя едут на свои места.
void Studio::MorphGhost() {
    float t = (float)((clock_ - morphAt_) / 0.5);
    if (t >= 1 || fx::Game() || headIcon_.Width <= 0) return;
    Item* it = Sel();
    if (!it) return;
    const UiColors& c = ui.C();
    float u = Back(std::max(0.0f, t)), e = (float)EaseOut(t);
    auto lerp = [](float a, float b, float q) { return a + (b - a) * q; };
    RectF to(headIcon_.X - ui.S(10), headIcon_.Y - ui.S(8), headTitle_.X + headTitle_.Width - headIcon_.X + ui.S(20),
             headIcon_.Height + ui.S(16));
    RectF from = morphFrom_;
    RectF bg(lerp(from.X, to.X, u), lerp(from.Y, to.Y, u), lerp(from.Width, to.Width, e),
             lerp(from.Height, to.Height, e));
    BYTE ba = (BYTE)(255 * (1 - e));
    ui.Fill(bg, WithAlpha(c.cardHi, ba), ui.S(11));
    ui.Stroke(bg, WithAlpha(c.accent, ba), ui.S(11), ui.S(1.5f));
    RectF fi(from.X + ui.S(9), from.Y + (from.Height - ui.S(32)) / 2, ui.S(32), ui.S(32));
    RectF ir(lerp(fi.X, headIcon_.X, u), lerp(fi.Y, headIcon_.Y, u), lerp(fi.Width, headIcon_.Width, e),
             lerp(fi.Height, headIcon_.Height, e));
    IconDrawn(IconFile(*it), it->full, ir);
    float size = std::round(lerp(13, 21, e) * 2) / 2;  // шрифт по полпункта: кэш шрифтов не пухнет
    RectF tr(lerp(from.X + ui.S(50), headTitle_.X, u), lerp(from.Y + ui.S(4), headTitle_.Y, u),
             lerp(ui.S(160), headTitle_.Width, e), lerp(ui.S(22), headTitle_.Height, e));
    ui.Text(it->name.empty() ? L"Без названия" : it->name, tr, c.text, size, true);
    ui.Invalidate();
}

// Лента правок: точки - состояния до и после каждой правки; тянешь точку назад - отмена по шагу,
// карточки разъезжаются, как было; вперёд - возврат.
void Studio::Tape(const RectF& r) {
    const UiColors& c = ui.C();
    int nU = (int)undo_.size(), nR = (int)redo_.size(), N = nU + nR;
    ui.Hover(ID_TAPE, r);
    ui.Tip(ID_TAPE,
           L"Лента правок: тяни красную точку назад - правки откатываются на глазах, вперёд - "
           L"возвращаются");
    float x0 = r.X + ui.S(10), x1 = r.X + r.Width - ui.S(10), ly = r.Y + r.Height - ui.S(11);
    ui.Fill(RectF(x0, ly - ui.S(1), x1 - x0, ui.S(2)), c.border, ui.S(1));
    if (N == 0) {
        ui.Text(L"Лента правок: здесь встанут точки, когда что-нибудь поменяешь", RectF(r.X, r.Y, r.Width, ui.S(22)),
                c.faint, 11.5f, false, ui::Center);
        return;
    }
    auto dotX = [&](int i) { return x0 + (x1 - x0) * i / N; };
    int at = nU;
    bool drag = ui.Active(ID_TAPE) && ui.Down();
    POINT m = ui.Mouse();
    if (drag) {
        tapeKnob_.x = std::clamp((float)m.x, x0, x1) - r.X;
        tapeKnob_.vx = 0;
        at = std::clamp((int)std::lround((m.x - x0) / (x1 - x0) * N), 0, N);
        if (at != nU) {
            tapeQuiet_ = true;
            while ((int)undo_.size() > at && !undo_.empty()) Undo();
            while ((int)undo_.size() < at && !redo_.empty()) Redo();
            tapeQuiet_ = false;
            nU = (int)undo_.size();
            nR = (int)redo_.size();
        }
    } else {
        if (tapeKnob_.x == 0 && tapeKnob_.y == 0) tapeKnob_.x = dotX(nU) - r.X, tapeKnob_.y = 1;
        if (Pull(tapeKnob_, dotX(nU) - r.X, 1, ldt_, 200, 22)) ui.Invalidate();
    }
    float kx = r.X + tapeKnob_.x;
    ui.Fill(RectF(x0, ly - ui.S(1), kx - x0, ui.S(2)), c.accent, ui.S(1));
    float dr = (x1 - x0) / N < ui.S(9) ? ui.S(2.5f) : ui.S(3.5f);
    for (int i = 0; i <= N; ++i) {
        SolidBrush b(i <= nU ? c.accent : c.faint);
        ui.G().FillEllipse(&b, dotX(i) - dr, ly - dr, dr * 2, dr * 2);
    }
    SolidBrush halo(WithAlpha(c.accent, 52)), knob(c.accent);
    ui.G().FillEllipse(&halo, kx - ui.S(13), ly - ui.S(13), ui.S(26), ui.S(26));
    ui.G().FillEllipse(&knob, kx - ui.S(8), ly - ui.S(8), ui.S(16), ui.S(16));
    std::wstring what =
        nU == 0 ? L"было так"
                : (undoWhat_.size() >= (size_t)nU && !undoWhat_[nU - 1].empty() ? undoWhat_[nU - 1] : L"правка");
    if (nR) what += L"  |  впереди " + std::to_wstring(nR);
    float tw = std::min(r.Width, ui.Measure(what, 11.5f) + ui.S(8));
    float lx = std::clamp(kx - tw / 2, r.X, r.X + r.Width - tw);
    ui.Text(what, RectF(lx, r.Y, tw, ui.S(20)), drag ? c.text : c.muted, 11.5f, false, ui::Center);
}

// Папки из Проводника, брошенные на дорожку: встают туда, куда отпустили, и влетают с края.
bool Studio::DropOnLanes(POINT pt, const std::vector<std::wstring>& paths) {
    int L = -1;
    for (int q = 0; q < 2; ++q)
        if (laneRect_[q].Contains((REAL)pt.x, (REAL)pt.y)) L = q;
    if (L < 0 || paths.empty()) return false;
    int at = 0;
    for (int i = 0; i < (int)laneSlotY_[L].size() && i < (int)Ring(L).size(); ++i)
        if (laneSlotY_[L][i] >= 0 && lanesOrg_.Y + laneSlotY_[L][i] + laneCardH_ / 2 < pt.y) ++at;
    Change();
    int to = at;
    for (const auto& p : paths) {
        Item it;
        it.path = p;
        std::wstring n = PathFindFileNameW(p.c_str());
        if (!IsFolder(p)) {
            PathRemoveExtensionW(n.data());
            n.resize(wcslen(n.c_str()));
        }
        it.name = n;
        Resolve(it);
        it.expand = it.dir;
        Mover mv;
        mv.x = laneRect_[1].X + laneRect_[1].Width + ui.S(30) - lanesOrg_.X;
        mv.y = pt.y - lanesOrg_.Y - ui.S(60) - (to - at) * ui.S(20);
        mv.vx = -900 * (float)ui.K();
        mv.vy = 300 * (float)ui.K();
        mv.pop = 1;
        laneHints_[KeyOf(it)] = mv;
        Ring(L).insert(Ring(L).begin() + std::min(to, (int)Ring(L).size()), it);
        ++to;
    }
    ring_ = L, sel_ = at, tab_ = 0;
    Log((paths.size() == 1 ? L"«" + std::wstring(PathFindFileNameW(paths[0].c_str())) + L"» влетела"
                           : L"Секторы влетели: " + std::to_wstring(paths.size())) +
        (L ? L" в правый край" : L" в левый край"));
    Save();
    return true;
}

// Поля сектора выезжают по очереди после выбора: сдвиг вниз и пелена, которая тает.
Studio::Arrival Studio::Arrive(int n) {
    double t = (clock_ - arriveAt_ - 0.1 - 0.07 * n) / 0.42;
    if (fx::Game() || t >= 1) return {};
    ui.Invalidate();
    float u = (float)std::clamp(t, 0.0, 1.0);
    return {ui.S(16) * (1 - Back(u)), 1 - (float)EaseOut(u)};
}

void Studio::BlockIn(int n) {
    arr_ = Arrive(n);
    if (arr_.veil <= 0) return;
    arrWas_ = ui.interactive;
    ui.interactive = false;  // пока едет - без полей ввода и нажатий
    ui.G().TranslateTransform(0, arr_.dy);
}

void Studio::BlockOut(const RectF& r) {
    if (arr_.veil <= 0) return;
    ui.Fill(Grow(r, ui.S(6)), WithAlpha(ui.C().win, (BYTE)(255 * arr_.veil)));
    ui.G().TranslateTransform(0, -arr_.dy);
    ui.interactive = arrWas_;
    arr_ = {};
}

}  // namespace studio
