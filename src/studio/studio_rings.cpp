// Раздел «Кольца»: список секторов слева и шапка выбранного сектора; вкладки - в studio_tabs.cpp.
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "core/files.h"
#include "core/perf.h"
#include "app/autostart.h"
#include "core/text.h"
#include "gfx/accent.h"
#include "gfx/draw.h"
#include "gfx/icons.h"
#include "studio/studio.h"
#include "studio/studio_internal.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

void Studio::RingsPage(const RectF& r) {
    float listW = ui.S(312);
    SectorList(RectF(r.X, r.Y, listW, r.Height));
    ui.Fill(RectF(r.X + listW, r.Y + ui.S(16), 1, r.Height - ui.S(32)), ui.C().border);
    SectorMain(RectF(r.X + listW + 1, r.Y, r.Width - listW - 1, r.Height));
}

void Studio::SectorList(const RectF& r) {
    const UiColors& c = ui.C();
    float x = r.X + ui.S(18), w = r.Width - ui.S(36);
    float y = r.Y + ui.S(18);
    ui.Text(L"Мои кольца", RectF(x, y, w, ui.S(32)), c.text, 21, true);
    y += ui.S(32);
    ui.Text(L"слева " + Plural((int)cfg.left.size(), L"сектор", L"сектора", L"секторов") + L"  |  справа " +
                Plural((int)cfg.right.size(), L"сектор", L"сектора", L"секторов"),
            RectF(x, y, w, ui.S(20)), c.muted, 12);
    y += ui.S(30);
    int side = ring_;
    RectF seg(x, y, w, ui.S(36));
    sideRect_[0] = RectF(seg.X, seg.Y, seg.Width / 2, seg.Height);
    sideRect_[1] = RectF(seg.X + seg.Width / 2, seg.Y, seg.Width / 2, seg.Height);
    if (ui.Segmented(ID_SIDE, seg, {L"Левый край", L"Правый край"}, side)) {
        ring_ = side;
        sel_ = 0;
        tab_ = 0;
        renaming_ = -1;
        ui.ResetScroll(ID_LIST_SCROLL);
    }
    ui.Tip(ID_SIDE * 16 + 0,
           L"Секторы у левого края экрана. Сюда можно "
           L"перетащить сектор с правого края.");
    ui.Tip(ID_SIDE * 16 + 1,
           L"Секторы у правого края экрана. Сюда можно "
           L"перетащить сектор с левого края.");
    y += ui.S(48);

    // плашка о проблемах: пропавший путь, пустой сектор
    if (!problems_.empty() && !problemsHidden_) {
        const Problem& p = problems_[0];
        const Item& it = p.j < 0 ? Ring(p.ring)[p.i] : Ring(p.ring)[p.i].children[p.j];
        std::wstring where =
            (p.ring == 0 ? L"слева, " : L"справа, ") + (p.j < 0 ? it.name : Ring(p.ring)[p.i].name + L" / " + it.name);
        std::wstring text = (p.kind == 0   ? L"Путь не найден: "
                             : p.kind == 1 ? L"Ничего не открывает: "
                                           : L"Раскрывается, но это не папка: ") +
                            where;
        if (problems_.size() > 1) text += L" (и ещё " + std::to_wstring(problems_.size() - 1) + L")";
        float th = ui.MeasureWrapped(text, 12, false, w - ui.S(24));
        RectF b(x, y, w, th + ui.S(62));
        ui.Fill(b, c.warnBg, ui.S(12));
        ui.Stroke(b, c.warn, ui.S(12));
        ui.Text(text, RectF(b.X + ui.S(12), b.Y + ui.S(10), b.Width - ui.S(24), th + ui.S(4)), c.text, 12, false,
                ui::Left, true);
        if (ui.Button(ID_PROBLEM_FIND, RectF(b.X + ui.S(12), b.Y + b.Height - ui.S(40), ui.S(104), ui.S(30)),
                      p.kind == 0 ? L"Найти" : L"Указать")) {
            if (p.kind == 0) {
                FixProblem(p);
            } else {
                ring_ = p.ring;
                sel_ = p.i;
                tab_ = p.j < 0 ? 0 : 1;
                focusPath_ = p.j < 0;
            }
        }
        ui.Tip(ID_PROBLEM_FIND, p.kind == 0 ? L"Поищу папку с тем же именем выше по пути и на рабочем столе"
                                            : L"Открою этот сектор, чтобы задать путь");
        if (ui.Button(ID_PROBLEM_HIDE, RectF(b.X + ui.S(122), b.Y + b.Height - ui.S(40), ui.S(96), ui.S(30)), L"Скрыть",
                      Kind::Ghost))
            problemsHidden_ = true;
        y += b.Height + ui.S(10);
    }

    auto& ring = Ring(ring_);
    int n = (int)ring.size();
    float bottomH = ui.S(84);
    RectF view(r.X + ui.S(10), y, r.Width - ui.S(20), r.Y + r.Height - bottomH - y);
    float itemH = ui.S(58);
    float off = ui.Scroll(ID_LIST_SCROLL, view, n * itemH);
    listView_ = view;
    listOff_ = off;
    itemH_ = itemH;

    // перетаскивание сектора: куда он сейчас попадёт
    int dragFrom = -1;
    for (int i = 0; i < n; ++i)
        if (ui.Dragging(ID_SECTOR + i)) dragFrom = i;
    POINT m = ui.Mouse();
    bool overSide = false;
    int into = -1, at = -1;
    if (dragFrom >= 0) {
        overSide = sideRect_[1 - ring_].Contains((REAL)m.x, (REAL)m.y);
        if (!overSide && view.Contains((REAL)m.x, (REAL)m.y)) {
            float rel = (m.y - view.Y + off) / itemH;
            int idx = (int)std::floor(rel);
            float frac = rel - idx;
            const Item& src = ring[dragFrom];
            bool simple = src.children.empty() && !src.expand;
            if (simple && idx >= 0 && idx < n && idx != dragFrom && frac > 0.28f && frac < 0.72f)
                into = idx;
            else
                at = std::clamp((int)std::floor(rel + 0.5f), 0, n);
        }
    }
    int childAt = -1;  // пункт второго кольца тянут в список - станет сектором
    if (childDragShown_ >= 0 && view.Contains((REAL)m.x, (REAL)m.y))
        childAt = std::clamp((int)std::floor((m.y - view.Y + off) / itemH + 0.5f), 0, n);

    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    for (int i = 0; i < n; ++i) {
        const Item& it = ring[i];
        RectF ir(view.X, view.Y + i * itemH - off, view.Width, itemH - ui.S(6));
        RectF vis = Intersect(ir, view);
        int id = ID_SECTOR + i;
        bool live = vis.Height > 1;
        bool clicked = live && ui.Click(id, vis);
        if (clicked && dragFrom < 0) {
            sel_ = i;
            tab_ = 0;
            ui.ResetScroll(ID_MAIN_SCROLL);
        }
        if (live && ui.RightClick(id, vis)) {
            sel_ = i;
            ov_ = OV_MORE;
            moreAtMouse_ = true;
            moreAnchor_ = RectF((REAL)m.x, (REAL)m.y, 0, 0);
        }
        if (live && ui.DoubleClick(id)) {
            sel_ = i;
            renaming_ = i;
            renameFocus_ = true;
        }
        bool on = sel_ == i;
        float hov = ui.Anim(id, ui.Hot(id) || on);
        if (on) {
            ui.Fill(ir, c.cardHi, ui.S(12));
            ui.Stroke(ir, c.accent, ui.S(12), ui.S(1.5f));
        } else if (hov > 0.01f) {
            ui.Fill(ir, Color((BYTE)(c.card.GetA() * hov), c.card.GetR(), c.card.GetG(), c.card.GetB()), ui.S(12));
        }
        if (into == i) {
            ui.Fill(ir, MixColor(c.card, c.gold, 0.12f), ui.S(12));
            ui.Stroke(ir, c.gold, ui.S(12), ui.S(2));
        }
        if (dragFrom == i) ui.Fill(ir, ToColor(RGB(0, 0, 0), 70), ui.S(12));
        ui.Icon(IconFile(it), RectF(ir.X + ui.S(10), ir.Y + (ir.Height - ui.S(34)) / 2, ui.S(34), ui.S(34)), it.full);
        float tx = ir.X + ui.S(54);
        RectF nameR(tx, ir.Y + ui.S(5), ir.Width - ui.S(64), ui.S(22));
        if (renaming_ == i) {
            RectF fr(tx - ui.S(4), ir.Y + ui.S(3), ir.Width - ui.S(60), ui.S(28));
            int rr = ring_, ii = i;
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
        } else {
            ui.Text(it.name, nameR, c.text, 13.5f, on);
        }
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
            kind = EndsWithI(it.full, L".lnk") || EndsWithI(it.full, L".exe") || EndsWithI(it.full, L".url")
                       ? L"программа"
                       : L"файл";
        }
        if (into == i) meta = L"отпусти: станет пунктом";
        float cw = ui.Measure(kind, 10.5f, true) + ui.S(12);
        RectF chip(tx, ir.Y + ui.S(29), cw, ui.S(18));
        ui.Chip(chip, kind, MixColor(c.card, c.accent, 0.22f), c.accent);
        ui.Text(meta, RectF(chip.X + cw + ui.S(8), chip.Y, ir.Width, chip.Height), into == i ? c.gold : c.muted, 11);
        for (const Problem& p : problems_)
            if (p.ring == ring_ && p.i == i) {
                SolidBrush wb(c.warn);
                ui.G().FillEllipse(&wb, ir.X + ir.Width - ui.S(18), ir.Y + ui.S(12), ui.S(8), ui.S(8));
                break;
            }
        if (live)
            ui.Tip(id, it.full.empty() ? L"Двойной клик - переименовать, ПКМ - действия, тяни - "
                                         L"поменять порядок"
                                       : it.full + L"\nДвойной клик - переименовать, ПКМ - действия");
    }
    // линия места вставки: при перетаскивании сектора или пункта из второго
    // кольца
    int line = at >= 0 ? at : childAt;
    if (line >= 0) {
        float ly = view.Y + line * itemH - off - ui.S(3);
        ui.Fill(RectF(view.X + ui.S(6), ly - ui.S(1.5f), view.Width - ui.S(12), ui.S(3)), c.gold, ui.S(2));
    }
    ui.G().Restore(st);
    if (overSide) {
        RectF sr = sideRect_[1 - ring_];
        ui.Fill(sr, MixColor(c.field, c.gold, 0.25f), ui.S(9));
        ui.Stroke(sr, c.gold, ui.S(9), ui.S(2));
        ui.Text(ring_ == 0 ? L"на правый край" : L"на левый край", sr, c.text, 12, true, ui::Center);
    }

    // отпустили сектор
    for (int i = 0; i < n; ++i) {
        if (!ui.Released(ID_SECTOR + i)) continue;
        if (std::abs(m.y - ui.PressPt().y) + std::abs(m.x - ui.PressPt().x) <= ui.S(6)) break;
        if (sideRect_[1 - ring_].Contains((REAL)m.x, (REAL)m.y)) {
            sel_ = i;
            SectorOp(OP_OTHER_SIDE);
        } else if (view.Contains((REAL)m.x, (REAL)m.y)) {
            float rel = (m.y - view.Y + off) / itemH;
            int idx = (int)std::floor(rel);
            float frac = rel - idx;
            bool simple = ring[i].children.empty() && !ring[i].expand;
            if (simple && idx >= 0 && idx < n && idx != i && frac > 0.28f && frac < 0.72f) {
                Change();
                Item moving = ring[i];
                std::wstring target = ring[idx].name;
                ring[idx].children.push_back(moving);
                ring.erase(ring.begin() + i);
                sel_ = idx > i ? idx - 1 : idx;
                tab_ = 1;
                Toast(L"«" + moving.name + L"» теперь пункт в «" + target + L"»");
                Save();
            } else {
                int to = std::clamp((int)std::floor(rel + 0.5f), 0, n);
                if (to != i && to != i + 1) {
                    Change();
                    Item moving = ring[i];
                    ring.erase(ring.begin() + i);
                    if (to > i) --to;
                    ring.insert(ring.begin() + to, moving);
                    sel_ = to;
                    Log(L"Порядок секторов изменён");
                    Save();
                }
            }
        }
        break;
    }

    float by = r.Y + r.Height - bottomH + ui.S(12);
    if (ui.Button(ID_ADD_SECTOR, RectF(x, by, w, ui.S(38)), L"+ Сектор", Kind::Secondary)) {
        Change();
        Item it;
        it.name = L"Новый сектор";
        ring.push_back(it);
        sel_ = (int)ring.size() - 1;
        tab_ = 0;
        renaming_ = sel_;
        renameFocus_ = true;
        Log(L"Добавлен сектор");
        Save();
    }
    ui.Tip(ID_ADD_SECTOR,
           L"Новый сектор в конце кольца. Ещё быстрее - "
           L"перетащить папку из Проводника в окно.");
    ui.Text(L"или перетащи папку из Проводника в окно", RectF(x, by + ui.S(42), w, ui.S(20)), c.faint, 11, false,
            ui::Center);
}

void Studio::SectorMain(const RectF& r) {
    const UiColors& c = ui.C();
    Item* it = Sel();
    RectF in = Inset(r, ui.S(24), ui.S(18));
    if (!it) {
        Mascot(RectF(in.X, in.Y + in.Height * 0.2f, in.Width, in.Height * 0.6f), L"mascot-peek.webp",
               L"Кольцо пока пустое",
               L"Добавь сектор кнопкой слева или перетащи папку из Проводника прямо в это окно.");
        if (ui.Button(
                ID_EMPTY_ADD,
                RectF(in.X + (in.Width - ui.S(160)) / 2, in.Y + in.Height * 0.2f + ui.S(230), ui.S(160), ui.S(38)),
                L"+ Сектор", Kind::Primary)) {
            Change();
            Item n;
            n.name = L"Новый сектор";
            Ring(ring_).push_back(n);
            sel_ = (int)Ring(ring_).size() - 1;
            Save();
        }
        return;
    }
    // шапка: имя, где стоит, кнопки
    float btnY = in.Y + ui.S(4);
    RectF more(in.X + in.Width - ui.S(44), btnY, ui.S(44), ui.S(38));
    bool moreClicked = ui.Button(ID_MORE, more, L"", Kind::Secondary);
    ui.Dots(more, c.text);
    if (moreClicked) {
        ov_ = OV_MORE;
        moreAnchor_ = more;
    }
    RectF open(more.X - ui.S(128), btnY, ui.S(120), ui.S(38));
    if (ui.Button(ID_OPEN, open, L"Открыть", Kind::Primary, IconFile(*it).empty() ? L"" : IconFile(*it)) &&
        !it->full.empty()) {
        std::wstring p = it->full;
        Defer([p] { ShellExecuteW(nullptr, nullptr, p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    }
    RectF show(open.X - ui.S(168), btnY, ui.S(160), ui.S(38));
    if (ui.Button(ID_SHOW_MENU, show, L"Показать меню", Kind::Secondary)) {
        bool left = ring_ == 0;
        Defer([this, left] {
            Save();
            if (showMenu) showMenu(left, wnd);
        });
    }
    float titleW = show.X - in.X - ui.S(12);
    ui.Text(it->name.empty() ? L"Без названия" : it->name, RectF(in.X, in.Y, titleW, ui.S(32)), c.text, 21, true);
    ui.Text((ring_ == 0 ? L"левый край, сектор " : L"правый край, сектор ") + std::to_wstring(sel_ + 1) + L" из " +
                std::to_wstring(Ring(ring_).size()),
            RectF(in.X, in.Y + ui.S(32), titleW, ui.S(20)), c.muted, 12);

    int tab = tab_;
    if (ui.Segmented(ID_TABS, RectF(in.X, in.Y + ui.S(64), std::min(in.Width, ui.S(470)), ui.S(38)),
                     {L"1  Основное", L"2  Второе кольцо", L"3  Значок"}, tab)) {
        tab_ = tab;
        ui.ResetScroll(ID_MAIN_SCROLL);
    }

    RectF view(in.X - ui.S(4), in.Y + ui.S(116), in.Width + ui.S(8), in.Y + in.Height - (in.Y + ui.S(116)));
    float off = ui.Scroll(ID_MAIN_SCROLL, view, mainContentH_);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    float y0 = view.Y - off, bottom = y0;
    if (tab_ == 0) TabBasic(view, y0, bottom);
    if (tab_ == 1) TabRing(view, y0, bottom);
    if (tab_ == 2) TabIcon(view, y0, bottom);
    mainContentH_ = bottom - y0 + ui.S(16);
    ui.G().Restore(st);
}

}  // namespace studio
