// Раздел «Кольца», правая часть живая: путь разбирается на звенья с подсказками похожих папок, второе
// кольцо наполняется плашками из папки (закреплённые - первыми, красная черта - сколько влезет сразу),
// значки программы - док: ближние к курсору крупнее, перетаскиваемый притягивается к квадрату.
#include <windows.h>

#include <algorithm>
#include <cmath>

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

constexpr int kRingFit = 12;  // у края в полукруг влезает около 12 пунктов, дальше кольцо крутится

float Back(float t) { return (float)EaseOutBack(t, 1.7); }

bool Pull(Mover& m, float tx, float ty, float dt, float k, float d) {
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

bool Fade(float& v, float to, float dt, float speed) {
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

// для сравнения имён: строчные, ё как е
std::wstring Fold(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    for (auto& ch : s)
        if (ch == L'ё') ch = L'е';
    return s;
}

int Lev(const std::wstring& a, const std::wstring& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        prev.swap(cur);
    }
    return prev[b.size()];
}

RectF Scaled(const RectF& r, float s) {
    return RectF(r.X + r.Width * (1 - s) / 2, r.Y + r.Height * (1 - s) / 2, r.Width * s, r.Height * s);
}

bool Launchable(const Item& it) {
    return it.dir || EndsWithI(it.full, L".lnk") || EndsWithI(it.full, L".url") || EndsWithI(it.full, L".exe");
}

}  // namespace

void Studio::IconSwap(const std::wstring& oldFile) {
    iconOld_ = oldFile;
    iconSwapAt_ = clock_;
    ui.Invalidate();
}

// Путь по звеньям: какие есть - зелёные, с того, где оборвался, - жёлтые; рядом похожие папки, щелчок чинит.
float Studio::PathChain(float x, float y, float w) {
    Item* it = Sel();
    if (!it) return 0;
    const UiColors& c = ui.C();
    std::wstring full = it->full.empty() ? ExpandPath(it->path) : it->full;
    if (full.size() < 2 || full.rfind(L"shell:", 0) == 0 || full.find(L"://") != std::wstring::npos ||
        full.rfind(L"\\\\", 0) == 0 || full[1] != L':')
        return 0;
    if (full != chainKey_ || clock_ - chainCheck_ > 3) {  // диск - не чаще раза в 3 с
        if (full != chainKey_) chainAt_ = clock_;
        chainKey_ = full;
        chainCheck_ = clock_;
        chainParts_.clear();
        size_t s = 0;
        while (s <= full.size()) {
            size_t e = full.find_first_of(L"\\/", s);
            if (e == std::wstring::npos) e = full.size();
            if (e > s) chainParts_.push_back(full.substr(s, e - s));
            s = e + 1;
        }
        chainBad_ = -1;
        std::wstring acc;
        for (int i = 0; i < (int)chainParts_.size(); ++i) {
            acc = i ? acc + L"\\" + chainParts_[i] : chainParts_[i];
            if (!Exists(i ? acc : acc + L"\\")) {
                chainBad_ = i;
                break;
            }
        }
        chainSugg_.clear();
        if (chainBad_ > 0) {
            std::wstring base;
            for (int i = 0; i < chainBad_; ++i) base += (i ? L"\\" : L"") + chainParts_[i];
            std::wstring want = Fold(chainParts_[chainBad_]);
            bool last = chainBad_ + 1 == (int)chainParts_.size();
            std::vector<std::pair<int, std::wstring>> found;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    std::wstring n = fd.cFileName;
                    if (n == L"." || n == L"..") continue;
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !last) continue;
                    std::wstring f = Fold(n);
                    int d = Lev(f, want);
                    bool pre = want.size() >= 3 && (f.rfind(want, 0) == 0 || want.rfind(f, 0) == 0);
                    if (d <= std::max(1, (int)want.size() / 3) || pre) found.push_back({pre ? std::min(d, 1) : d, n});
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < found.size() && i < 4; ++i) chainSugg_.push_back(found[i].second);
        }
    }
    float y0 = y, h = ui.S(26), cx = x;
    for (int i = 0; i < (int)chainParts_.size(); ++i) {
        float u = fx::Game() ? 1.0f : (float)std::clamp((clock_ - chainAt_ - 0.04 - i * 0.07) / 0.3, 0.0, 1.0);
        if (u < 1) ui.Invalidate();
        float sw = i ? ui.Measure(L"\\", 12) + ui.S(4) : 0;
        float tw = ui.Measure(chainParts_[i], 12.5f) + ui.S(18);
        if (cx + sw + tw > x + w && cx > x) {
            cx = x;
            y += h + ui.S(6);
            sw = 0;
        }
        BYTE a = (BYTE)(255 * u);
        if (sw > 0) ui.Text(L"\\", RectF(cx, y, sw, h), WithAlpha(c.faint, a), 12, false, ui::Center);
        cx += sw;
        bool ok = chainBad_ < 0 || i < chainBad_;
        Color green(255, 120, 205, 160);
        Color bg = ok ? MixColor(c.card, green, 0.2f) : MixColor(c.card, c.warn, 0.24f);
        Color fg = ok ? MixColor(c.text, green, 0.55f) : c.warn;
        RectF sr(cx, y + ui.S(8) * (1 - Back(u)), tw, h);
        ui.Fill(sr, WithAlpha(bg, a), ui.S(7));
        ui.Text(chainParts_[i], sr, WithAlpha(fg, a), 12.5f, false, ui::Center);
        cx += tw + ui.S(4);
    }
    y += h + ui.S(8);
    float lh = ui.S(26);
    if (chainBad_ >= 0) {
        std::wstring base;
        for (int i = 0; i < chainBad_; ++i) base += (i ? L"\\" : L"") + chainParts_[i];
        if (chainSugg_.empty()) {
            ui.Text(chainBad_ == 0 ? L"такого диска нет" : L"в «" + base + L"» похожей папки нет", RectF(x, y, w, lh),
                    c.faint, 12);
        } else {
            float lw = ui.Measure(L"похоже на:", 12) + ui.S(8);
            ui.Text(L"похоже на:", RectF(x, y, lw, lh), c.faint, 12);
            float bx = x + lw;
            for (int k = 0; k < (int)chainSugg_.size(); ++k) {
                float bw = ui.Measure(chainSugg_[k], 12) + ui.S(18);
                if (bx + bw > x + w) break;
                RectF br(bx, y, bw, lh);
                int id = ID_PATH_SUGG + k;
                bool hit = ui.Click(id, br);
                ui.Fill(br, MixColor(c.card, c.accent2, ui.Hot(id) ? 0.3f : 0.16f), ui.S(6));
                ui.Text(chainSugg_[k], br, c.accent2, 12, false, ui::Center);
                std::wstring fixed = base + L"\\" + chainSugg_[k];
                for (int i = chainBad_ + 1; i < (int)chainParts_.size(); ++i) fixed += L"\\" + chainParts_[i];
                ui.Tip(id, L"Поставить путь " + fixed);
                if (hit) {
                    Change();
                    it->path = fixed;
                    Resolve(*it);
                    Log(L"Путь исправлен: " + fixed);
                    Save();
                    break;
                }
                bx += bw + ui.S(6);
            }
        }
    } else if (it->dir) {
        int n = Stats(*it).total;
        ui.Text(L"папка есть: " + std::to_wstring(n) + L" внутри" +
                    (it->expand || !n ? L"" : L" - второе кольцо можно собрать"),
                RectF(x, y, w, lh), c.muted, 12);
    } else {
        ui.Text(L"файл на месте", RectF(x, y, w, lh), c.muted, 12);
    }
    y += lh + ui.S(6);
    return y - y0;
}

// Второе кольцо, как его соберёт меню: свои пункты первыми, за ними содержимое папки. Плашки сыплются
// сверху; щелчок по плашке из папки - закрепить первым (станет своим пунктом), по закреплённой - открепить.
float Studio::RingFlow(const RectF& view, float x, float y, float w) {
    Item* it = Sel();
    const UiColors& c = ui.C();
    float y0 = y;
    if (it->dir) {
        if (!it->expand) {
            if (ui.Button(ID_FLOW_FILL, RectF(x, y, ui.S(230), ui.S(34)), L"Второе кольцо из папки", Kind::Primary)) {
                Change();
                it->expand = true;
                Log(L"Второе кольцо наполняется из папки");
                Save();
            }
            ui.Tip(ID_FLOW_FILL, L"Пункты второго кольца возьмутся из содержимого папки сами");
        } else {
            if (ui.Button(ID_FLOW_FILES, RectF(x, y, ui.S(210), ui.S(34)),
                          it->files ? L"Только папки и ярлыки" : L"И обычные файлы")) {
                Change();
                it->files = !it->files;
                Log(it->files ? L"Во втором кольце и обычные файлы" : L"Во втором кольце только папки и ярлыки");
                Save();
            }
            ui.Tip(ID_FLOW_FILES, L"Обычные файлы во втором кольце - с пунктирной рамкой");
        }
        y += ui.S(46);
    }
    struct Entry {
        const Item* it;
        int child;  // свой пункт: номер, иначе -1
        bool listed, file;
        std::wstring key;
    };
    std::vector<Entry> list;
    auto& ch = it->children;
    static const std::vector<Item> kNone;
    const std::vector<Item>& folder = it->expand ? Listing(*it) : kNone;
    for (int j = 0; j < (int)ch.size(); ++j) {
        bool listed = false;
        for (const Item& li : folder)
            if (!ch[j].full.empty() && _wcsicmp(li.full.c_str(), ch[j].full.c_str()) == 0) listed = true;
        list.push_back({&ch[j], j, listed, !ch[j].full.empty() && !Launchable(ch[j]),
                        ch[j].full.empty() ? L"own#" + std::to_wstring(j) : Fold(ch[j].full)});
    }
    for (const Item& li : folder) {
        bool pinned = false;
        for (const Item& o : ch)
            if (!o.full.empty() && _wcsicmp(li.full.c_str(), o.full.c_str()) == 0) pinned = true;
        if (!pinned) list.push_back({&li, -1, true, !Launchable(li), Fold(li.full)});
    }
    if (list.empty()) return y - y0;
    int total = (int)list.size(), shown = std::min(total, 80);
    std::wstring fk = std::to_wstring(ring_) + L"|" + std::to_wstring(sel_) + L"|" + it->full;
    if (fk != flowKey_) {
        flowKey_ = fk;
        flowChips_.clear();
        flowAt_ = clock_;
    }
    // раскладка строками по ширине
    float pad = ui.S(10), ch_ = ui.S(28), pitch = ui.S(36);
    std::vector<RectF> slot(shown);
    float cx = pad, cy = pad;
    for (int n = 0; n < shown; ++n) {
        float cw = ui.S(7 + 16 + 6 + 9) + ui.Measure(list[n].it->name, 12) + (list[n].child >= 0 ? ui.S(10) : 0);
        cw = std::min(cw, w - 2 * pad);
        if (cx + cw > w - pad && cx > pad) {
            cx = pad;
            cy += pitch;
        }
        slot[n] = RectF(cx, cy, cw, ch_);
        cx += cw + ui.S(6);
    }
    float boxH = cy + ch_ + pad + ui.S(30);
    RectF box(x, y, w, boxH);
    Reveal(box);
    ui.Fill(box, MixColor(c.win, c.card, 0.55f), ui.S(12));
    ui.Stroke(box, c.border, ui.S(12));
    bool moving = false;
    GraphicsState st = ui.G().Save();
    ui.G().IntersectClip(box);
    int toggled = -1;
    for (int n = 0; n < shown; ++n) {
        const Entry& e = list[n];
        auto f = flowChips_.find(e.key);
        if (f == flowChips_.end()) {
            Mover mv;
            int q = std::min(n, 24);
            mv.x = scene::Rnd(pad, std::max(pad, w - ui.S(140)));
            mv.y = -ui.S(40) - q * ui.S(30) * 0.4f;
            mv.wait = (float)std::max(0.0, flowAt_ + q * 0.06 - clock_);
            f = flowChips_.emplace(e.key, mv).first;
        }
        Mover& mv = f->second;
        if (mv.wait > 0 && !fx::Game()) {
            mv.wait -= ldt_;
            moving = true;
            continue;
        }
        moving |= Pull(mv, slot[n].X, slot[n].Y, ldt_, 120, 13);
        RectF r(box.X + mv.x, box.Y + mv.y, slot[n].Width, ch_);
        int id = ID_FLOW + n;
        RectF vis = Intersect(Intersect(r, view), box);
        if (vis.Height > 1 && ui.Click(id, vis)) toggled = n;
        bool hot = ui.Hot(id);
        if (e.file) {
            ui.Fill(r, MixColor(c.card, c.win, 0.3f), ui.S(7));
            ui.Stroke(r, c.border, ui.S(7), ui.S(1), true);
        } else {
            ui.Fill(r, hot ? c.cardHi : MixColor(c.card, c.cardHi, 0.5f), ui.S(7));
            ui.Stroke(r, e.child >= 0 ? c.accent : c.border, ui.S(7), e.child >= 0 ? ui.S(1.3f) : ui.S(1));
        }
        IconDrawn(IconFile(*e.it), e.it->full, RectF(r.X + ui.S(7), r.Y + (ch_ - ui.S(16)) / 2, ui.S(16), ui.S(16)));
        float tw = r.Width - ui.S(7 + 16 + 6 + 6) - (e.child >= 0 ? ui.S(10) : 0);
        ui.Text(e.it->name, RectF(r.X + ui.S(29), r.Y, tw, ch_), e.file ? c.muted : c.text, 12);
        if (e.child >= 0) {
            SolidBrush pin(c.accent);
            ui.G().FillEllipse(&pin, r.X + r.Width - ui.S(13), r.Y + ch_ / 2 - ui.S(3), ui.S(6), ui.S(6));
        }
        if (n == kRingFit) ui.Fill(RectF(r.X - ui.S(4), r.Y + ui.S(2), ui.S(2), ch_ - ui.S(4)), c.accent, ui.S(1));
        if (n >= kRingFit) ui.Fill(Scaled(r, 1.04f), WithAlpha(MixColor(c.win, c.card, 0.55f), 165), ui.S(7));
        ui.Tip(id, e.child >= 0 ? (e.listed ? L"Закреплён первым. Щелчок - открепить"
                                            : L"Свой пункт: подпись и путь правятся ниже")
                                : (e.it->full + L"\nЩелчок - закрепить первым"));
    }
    ui.G().Restore(st);
    int over = total - kRingFit;
    ui.Text(over > 0 ? L"за красной чертой не влезет сразу: ещё " + std::to_wstring(over) + L", кольцо крутится колесом"
                     : L"влезет целиком: " + std::to_wstring(total) + L" из " + std::to_wstring(kRingFit),
            RectF(box.X + pad, box.Y + boxH - ui.S(30), w - 2 * pad, ui.S(24)),
            over > 0 ? MixColor(c.accent, c.text, 0.4f) : c.faint, 11.5f, false, ui::Right);
    if (total > shown)
        ui.Text(L"показаны первые " + std::to_wstring(shown), RectF(box.X + pad, box.Y + boxH - ui.S(30), w, ui.S(24)),
                c.faint, 11.5f);
    if (toggled >= 0) {
        const Entry& e = list[toggled];
        if (e.child >= 0 && e.listed) {
            Change();
            std::wstring n = ch[e.child].name;
            ch.erase(ch.begin() + e.child);
            Log(L"«" + n + L"» откреплён");
            Save();
        } else if (e.child < 0) {
            Change();
            Item pin = *e.it;
            pin.path = pin.full;
            ch.push_back(pin);
            Log(L"«" + pin.name + L"» закреплён первым");
            Save();
        }
    }
    if (moving) ui.Invalidate();
    return y - y0 + boxH + ui.S(14);
}

// Значки программы доком: ближние к курсору крупнее; значок тянется мышью, у квадрата его притягивает,
// отпустил рядом - старый в квадрате растворяется, новый подпрыгивает.
float Studio::IconDock(const RectF& view, float x, float y, float w) {
    Item* it = Sel();
    const UiColors& c = ui.C();
    if (programIcons_.empty()) {  // папка значков не меняется, пока открыта студия
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((ExeDir() + L"\\icons\\*.png").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n != L"app.png" && n != L"hub.png") programIcons_.push_back(n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(programIcons_.begin(), programIcons_.end());
    }
    const std::vector<std::wstring>& files = programIcons_;
    float tile = ui.S(64), gap = ui.S(10);
    int cols = std::max(1, (int)((w + gap) / (tile + gap)));
    int rows = ((int)files.size() + cols - 1) / cols;
    RectF grid(x, y, w, rows * (tile + gap));
    RectF gv = Intersect(grid, view);
    if (gv.Height > 1) {
        ui.Hover(ID_ICON_DOCK, gv);
        ui.Track(ID_ICON_DOCK);
    }
    POINT m = ui.Mouse();
    bool inside =
        ui.interactive && Grow(grid, ui.S(24)).Contains((REAL)m.x, (REAL)m.y) && view.Contains((REAL)m.x, (REAL)m.y);
    int dragK = -1;
    for (int k = 0; k < (int)files.size(); ++k)
        if (ui.Dragging(ID_GRID_ICON + k)) dragK = k;
    if (!ui.Down()) dockDrag_ = -1;
    PointF sq(iconTile_.X + iconTile_.Width / 2 - x, iconTile_.Y + iconTile_.Height / 2 - y);  // в рамке сетки
    float reach = ui.S(120);
    bool moving = false;
    auto apply = [&](int k) {
        std::wstring rel = L"icons/" + files[k];
        if (_wcsicmp(it->icon.c_str(), rel.c_str()) == 0) return;
        std::wstring old = IconFile(*it);
        Change();
        it->icon = rel;
        Resolve(*it);
        Log(L"Значок заменён");
        Save();
        IconSwap(old);
    };
    std::vector<int> order;
    for (int k = 0; k < (int)files.size(); ++k) {
        float hx = (k % cols) * (tile + gap), hy = (k / cols) * (tile + gap);
        auto f = dockTiles_.find(files[k]);
        if (f == dockTiles_.end()) {
            Mover mv;
            mv.x = hx, mv.y = hy;
            f = dockTiles_.emplace(files[k], mv).first;
        }
        Mover& mv = f->second;
        float cxk = x + hx + tile / 2, cyk = y + hy + tile / 2;
        float nb = inside && dragK < 0 ? std::max(0.0f, 1 - std::hypot(m.x - cxk, m.y - cyk) / ui.S(90)) : 0;
        moving |= Fade(mv.s, 1 + nb * 0.35f, ldt_, 12);
        int id = ID_GRID_ICON + k;
        RectF home(x + hx, y + hy, tile, tile);
        RectF vis = Intersect(home, view);
        bool clicked = vis.Height > 1 && ui.Click(id, vis);
        if (vis.Height > 1) ui.Track(id);
        if (ui.Active(id) && dockDrag_ != k) {
            dockDrag_ = k;
            grab_ = PointF(ui.PressPt().x - (x + mv.x), ui.PressPt().y - (y + mv.y));
        }
        if (k == dragK) {
            float rx = m.x - grab_.X - x, ry = m.y - grab_.Y - y;  // где был бы без притяжения
            float d = std::hypot(rx + tile / 2 - sq.X, ry + tile / 2 - sq.Y);
            moving |= Fade(mv.pull, d < reach ? 1.0f : 0.0f, ldt_, 6);
            float q = mv.pull * std::clamp(1 - d / reach, 0.0f, 1.0f) * 0.9f;
            mv.x = rx + (sq.X - tile / 2 - rx) * q;
            mv.y = ry + (sq.Y - tile / 2 - ry) * q;
            mv.vx = mv.vy = 0;
            moving = true;
        } else {
            mv.pull = 0;
            moving |= Pull(mv, hx, hy, ldt_, 160, 16);
        }
        if (clicked && std::abs(m.x - ui.PressPt().x) + std::abs(m.y - ui.PressPt().y) <= ui.S(6)) apply(k);
        if (ui.Released(id) && std::abs(m.x - ui.PressPt().x) + std::abs(m.y - ui.PressPt().y) > ui.S(6)) {
            float d = std::hypot(mv.x + tile / 2 - sq.X, mv.y + tile / 2 - sq.Y);
            if (d < reach) {
                apply(k);
                mv.x = hx, mv.y = hy, mv.s = 0.4f;  // в сетку возвращается с места, вырастает
            }
        }
        order.push_back(k);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if ((a == dragK) != (b == dragK)) return b == dragK;
        return dockTiles_[files[a]].s < dockTiles_[files[b]].s;
    });
    for (int k : order) {
        Mover& mv = dockTiles_[files[k]];
        float s = k == dragK ? 1.15f : mv.s;
        RectF tr = Scaled(RectF(x + mv.x, y + mv.y, tile, tile), s);
        std::wstring rel = L"icons/" + files[k];
        bool on = _wcsicmp(it->icon.c_str(), rel.c_str()) == 0 ||
                  _wcsicmp(it->icon.c_str(), (L"icons\\" + files[k]).c_str()) == 0;
        int id = ID_GRID_ICON + k;
        if (k == dragK)
            ui.Fill(RectF(tr.X + ui.S(3), tr.Y + ui.S(8), tr.Width, tr.Height), Color(100, 0, 0, 0), ui.S(14));
        ui.Fill(tr, on || ui.Hot(id) || k == dragK ? c.cardHi : c.card, ui.S(12) * s);
        if (on) ui.Stroke(tr, c.accent, ui.S(12) * s, ui.S(2));
        IconDrawn(ExeDir() + L"\\icons\\" + files[k], L"", Inset(tr, ui.S(10) * s, ui.S(10) * s));
        ui.Tip(id, L"Щелчок - поставить; или тяни к квадрату значка");
    }
    if (moving) ui.Invalidate();
    return grid.Height;
}

}  // namespace studio
