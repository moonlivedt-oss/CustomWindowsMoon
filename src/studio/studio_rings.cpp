// Раздел «Кольца»: список секторов и три вкладки выбранного сектора.
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

void Studio::TabBasic(const RectF& view, float y, float& bottom) {
    const UiColors& c = ui.C();
    Item* it = Sel();
    bool isSector = true;
    float x = view.X + ui.S(4), w = view.Width - ui.S(8);
    float lw = ui.S(118), fh = ui.S(38);
    int r = ring_, i = sel_;

    ui.Text(L"Подпись", RectF(x, y, lw, fh), c.muted, 12.5f);
    FieldOrText(
        ID_NAME, RectF(x + lw, y, w - lw, fh), view, it->name,
        [this, r, i](const std::wstring& s) {
            FieldEdit(ID_NAME);
            Ring(r)[i].name = s;
        },
        L"Как сектор будет подписан в кольце");
    y += fh + ui.S(12);

    ui.Text(L"Путь", RectF(x, y, lw, fh), c.muted, 12.5f);
    float bw = ui.S(92);
    FieldOrText(
        ID_PATH, RectF(x + lw, y, w - lw - 2 * bw - ui.S(16), fh), view, it->path,
        [this, r, i](const std::wstring& s) {
            FieldEdit(ID_PATH);
            Ring(r)[i].path = s;
            Resolve(Ring(r)[i]);
        },
        L"Папка, файл, ярлык или ссылка");
    if (focusPath_) {
        ui.FocusField(ID_PATH);
        focusPath_ = false;
    }
    if (ui.Button(ID_PICK_FOLDER, RectF(x + w - 2 * bw - ui.S(8), y, bw, fh), L"Папка…"))
        Defer([this, r, i] {
            std::wstring p = PickPath(true);
            if (p.empty()) return;
            Change();
            Item& t = Ring(r)[i];
            bool rename = t.name.empty() || t.name == L"Новый сектор";
            t.path = p;
            if (rename) t.name = PathFindFileNameW(p.c_str());
            Resolve(t);
            Save();
        });
    if (ui.Button(ID_PICK_FILE, RectF(x + w - bw, y, bw, fh), L"Файл…"))
        Defer([this, r, i] {
            std::wstring p = PickPath(false);
            if (p.empty()) return;
            Change();
            Item& t = Ring(r)[i];
            bool rename = t.name.empty() || t.name == L"Новый сектор";
            t.path = p;
            if (rename) {
                std::wstring n = PathFindFileNameW(p.c_str());
                PathRemoveExtensionW(n.data());
                n.resize(wcslen(n.c_str()));
                t.name = n;
            }
            Resolve(t);
            Save();
        });
    y += fh + ui.S(12);

    for (const Problem& p : problems_) {
        if (p.ring != ring_ || p.i != sel_ || p.j != -1) continue;
        RectF b(x + lw, y, w - lw, ui.S(44));
        ui.Fill(b, c.warnBg, ui.S(10));
        ui.Stroke(b, c.warn, ui.S(10));
        ui.Text(L"Такого пути нет: папку переместили или удалили",
                RectF(b.X + ui.S(12), b.Y, b.Width - ui.S(130), b.Height), c.text, 12);
        if (ui.Button(ID_FIX_THIS, RectF(b.X + b.Width - ui.S(112), b.Y + ui.S(7), ui.S(102), ui.S(30)), L"Найти"))
            FixProblem(p);
        y += b.Height + ui.S(12);
        break;
    }

    if (isSector) {
        bool ex = it->expand, fl = it->files;
        if (ui.Toggle(ID_EXPAND, RectF(x + lw, y, ui.S(44), ui.S(30)), ex)) {
            Change();
            it->expand = ex;
            Save();
        }
        ui.Text(L"Второе кольцо из содержимого папки", RectF(x + lw + ui.S(54), y, w, ui.S(30)), c.text, 12.5f);
        y += ui.S(38);
        if (ex) {
            if (ui.Toggle(ID_FILES, RectF(x + lw, y, ui.S(44), ui.S(30)), fl)) {
                Change();
                it->files = fl;
                Save();
            }
            ui.Text(L"Показывать и обычные файлы, не только папки и ярлыки", RectF(x + lw + ui.S(54), y, w, ui.S(30)),
                    c.text, 12.5f);
            y += ui.S(38);
        }
    }
    y += ui.S(8);

    // карточки с цифрами
    FolderStats fs = Stats(*it);
    int count = (int)it->children.size() + (it->expand ? fs.total : 0);
    int per = std::max(2, cfg.maxChildren - 1);
    int pages = count == 0 ? 0 : (count > cfg.maxChildren ? (count + per - 1) / per : 1);
    struct {
        std::wstring v, l;
        Color col;
    } cards[] = {
        {std::to_wstring(count), L"во втором кольце", c.gold},
        {it->dir ? std::to_wstring(fs.dirs) : L"—", L"папок внутри", c.text},
        {pages ? std::to_wstring(pages) : L"—", pages > 1 ? L"страницы, листаются колесом" : L"страница", c.text}};
    float cw = (w - 2 * ui.S(12)) / 3;
    for (int k2 = 0; k2 < 3; ++k2) {
        RectF cr(x + k2 * (cw + ui.S(12)), y, cw, ui.S(78));
        Card(cr);
        ui.Text(cards[k2].v, RectF(cr.X + ui.S(16), cr.Y + ui.S(10), cr.Width, ui.S(34)), cards[k2].col, 22, true);
        ui.Text(cards[k2].l, RectF(cr.X + ui.S(16), cr.Y + ui.S(44), cr.Width - ui.S(20), ui.S(20)), c.muted, 11.5f);
    }
    y += ui.S(92);

    // что умеет сектор
    RectF tip(x, y, w, ui.S(150));
    Card(tip);
    ui.Text(L"Что умеет сектор", RectF(tip.X + ui.S(16), tip.Y + ui.S(12), tip.Width, ui.S(24)), c.text, 14, true);
    struct Row {
        std::vector<std::wstring> keys;
        const wchar_t* text;
    } rows[] = {{{L"Alt"}, L"отпустить над сектором: открыть"},
                {{L"Shift"}, L"открыть в VS Code"},
                {{L"Ctrl"}, L"терминал в этой папке"},
                {{L"ПКМ"}, L"зайти внутрь папки"}};
    float ry = tip.Y + ui.S(44), colW = (tip.Width - ui.S(32)) / 2;
    for (int k2 = 0; k2 < 4; ++k2) {
        float rx = tip.X + ui.S(16) + (k2 % 2) * colW, yy = ry + (k2 / 2) * ui.S(42);
        float kw = ui.KeycapWidth(rows[k2].keys[0]);
        ui.Keycap(RectF(rx, yy, kw, ui.S(32)), rows[k2].keys[0]);
        ui.Text(rows[k2].text, RectF(rx + kw + ui.S(10), yy, colW - kw - ui.S(14), ui.S(30)), c.muted, 12);
    }
    ui.Text(L"А если в меню бросить на сектор файл, он переедет в эту папку.",
            RectF(tip.X + ui.S(16), tip.Y + tip.Height - ui.S(30), tip.Width - ui.S(32), ui.S(22)), c.faint, 11.5f);
    y += tip.Height;
    bottom = y;
}

void Studio::TabRing(const RectF& view, float y, float& bottom) {
    const UiColors& c = ui.C();
    Item* it = Sel();
    float x = view.X + ui.S(4), w = view.Width - ui.S(8);
    int r = ring_, i = sel_;
    childArea_ = view;
    auto& ch = it->children;
    if (ch.empty() && !it->expand) {
        Mascot(RectF(x, y + ui.S(10), w, ui.S(230)), L"mascot-surprised.webp", L"Второго кольца пока нет",
               L"Добавь пункты вручную или включи «Второе кольцо из содержимого папки» на первой вкладке. "
               L"Можно и просто перетащить папки в окно.");
        if (ui.Button(ID_ADD_CHILD, RectF(x + (w - ui.S(160)) / 2, y + ui.S(250), ui.S(160), ui.S(38)), L"+ Пункт",
                      Kind::Primary)) {
            Change();
            Item n;
            n.name = L"Новый пункт";
            it->children.push_back(n);
            Save();
        }
        bottom = y + ui.S(300);
        return;
    }
    if (!ch.empty())
        ui.Text(L"Свои пункты идут первыми. Тяни за точки слева, чтобы поменять порядок.", RectF(x, y, w, ui.S(22)),
                c.muted, 12);
    y += ui.S(30);
    float rowH = ui.S(56);
    int dragFrom = -1;
    for (int j = 0; j < (int)ch.size(); ++j)
        if (ui.Dragging(ID_CHILD + j * 8)) dragFrom = j;
    float listY = y;
    for (int j = 0; j < (int)ch.size(); ++j) {
        RectF row(x, y, w, rowH - ui.S(8));
        Card(row, dragFrom == j);
        RectF handle(row.X + ui.S(6), row.Y, ui.S(24), row.Height);
        RectF hv = Intersect(handle, view);
        if (hv.Height > 1) ui.Hover(ID_CHILD + j * 8, hv);
        SolidBrush db(ui.Hot(ID_CHILD + j * 8) ? c.text : c.faint);
        for (int d = 0; d < 6; ++d)
            ui.G().FillEllipse(&db, handle.X + ui.S(7) + (d % 2) * ui.S(7),
                               row.Y + row.Height / 2 - ui.S(9) + (d / 2) * ui.S(7), ui.S(3.5f), ui.S(3.5f));
        ui.Icon(IconFile(ch[j]), RectF(row.X + ui.S(34), row.Y + (row.Height - ui.S(28)) / 2, ui.S(28), ui.S(28)),
                ch[j].full);
        float fx = row.X + ui.S(72), fh = ui.S(34), fy = row.Y + (row.Height - fh) / 2;
        float nameW = std::min(ui.S(190), w * 0.28f);
        FieldOrText(
            ID_CHILD + j * 8 + 1, RectF(fx, fy, nameW, fh), view, ch[j].name,
            [this, r, i, j](const std::wstring& s) {
                FieldEdit(ID_CHILD + j * 8 + 1);
                Ring(r)[i].children[j].name = s;
            },
            L"Подпись");
        float bw = ui.S(86), xw = ui.S(36);
        float pathW = row.X + row.Width - ui.S(10) - xw - ui.S(8) - bw - ui.S(8) - (fx + nameW + ui.S(8));
        FieldOrText(
            ID_CHILD + j * 8 + 2, RectF(fx + nameW + ui.S(8), fy, pathW, fh), view, ch[j].path,
            [this, r, i, j](const std::wstring& s) {
                FieldEdit(ID_CHILD + j * 8 + 2);
                Ring(r)[i].children[j].path = s;
                Resolve(Ring(r)[i].children[j]);
            },
            L"Путь");
        RectF pb(row.X + row.Width - ui.S(10) - xw - ui.S(8) - bw, fy, bw, fh);
        RectF pbv = Intersect(pb, view);
        if (pbv.Height > fh - 1 && ui.Button(ID_CHILD + j * 8 + 3, pb, L"Папка…"))
            Defer([this, r, i, j] {
                std::wstring p = PickPath(true);
                if (p.empty()) return;
                Change();
                Item& t = Ring(r)[i].children[j];
                if (t.name.empty() || t.name == L"Новый пункт") t.name = PathFindFileNameW(p.c_str());
                t.path = p;
                Resolve(t);
                Save();
            });
        RectF xb(row.X + row.Width - ui.S(10) - xw, fy, xw, fh);
        if (Intersect(xb, view).Height > fh - 1 && ui.Button(ID_CHILD + j * 8 + 4, xb, L"×", Kind::Ghost)) {
            Change();
            std::wstring n = ch[j].name;
            ch.erase(ch.begin() + j);
            Toast(L"Пункт «" + n + L"» удалён");
            Save();
            break;
        }
        y += rowH;
    }
    childDrag_ = dragFrom;
    POINT mm = ui.Mouse();
    bool toList = listView_.Contains((REAL)mm.x, (REAL)mm.y);
    if (dragFrom >= 0 && !toList) {
        int at = std::clamp((int)std::floor((ui.Mouse().y - listY + rowH / 2) / rowH), 0, (int)ch.size());
        ui.Fill(RectF(x + ui.S(6), listY + at * rowH - ui.S(6), w - ui.S(12), ui.S(3)), c.gold, ui.S(2));
    }
    for (int j = 0; j < (int)ch.size(); ++j) {
        if (!ui.Released(ID_CHILD + j * 8)) continue;
        if (toList && std::abs(mm.y - ui.PressPt().y) + std::abs(mm.x - ui.PressPt().x) > ui.S(6)) {
            // пункт брошен в список секторов: становится сектором
            int to =
                std::clamp((int)std::floor((mm.y - listView_.Y + listOff_) / itemH_ + 0.5f), 0, (int)Ring(r).size());
            Change();
            Item moving = ch[j];
            ch.erase(ch.begin() + j);
            Ring(r).insert(Ring(r).begin() + to, moving);
            if (to <= sel_) ++sel_;
            Toast(L"«" + moving.name + L"» теперь отдельный сектор");
            Save();
            break;
        }
        int at = std::clamp((int)std::floor((ui.Mouse().y - listY + rowH / 2) / rowH), 0, (int)ch.size());
        if (at != j && at != j + 1) {
            Change();
            Item moving = ch[j];
            ch.erase(ch.begin() + j);
            if (at > j) --at;
            ch.insert(ch.begin() + at, moving);
            Save();
        }
    }
    if (ui.Button(ID_ADD_CHILD, RectF(x, y + ui.S(4), ui.S(150), ui.S(36)), L"+ Пункт")) {
        Change();
        Item n;
        n.name = L"Новый пункт";
        ch.push_back(n);
        Save();
    }
    y += ui.S(54);
    if (it->expand) {
        const auto& list = Listing(*it);
        RectF card(x, y, w, ui.S(40));
        ui.Text(L"Из папки добавятся сами (" + std::to_wstring(list.size()) + L"):", RectF(x, y, w, ui.S(24)), c.muted,
                12);
        y += ui.S(30);
        float cx = x, cy = y;
        int shown = 0;
        for (const Item& li : list) {
            float cw = ui.Measure(li.name, 11.5f) + ui.S(40);
            if (cx + cw > x + w) {
                cx = x;
                cy += ui.S(36);
                if (cy > y + ui.S(36) * 3) break;
            }
            RectF chip(cx, cy, cw, ui.S(30));
            ui.Fill(chip, c.card, ui.S(8));
            ui.Stroke(chip, c.border, ui.S(8));
            ui.Icon(IconFile(li), RectF(chip.X + ui.S(6), chip.Y + ui.S(5), ui.S(20), ui.S(20)), li.full);
            ui.Text(li.name, RectF(chip.X + ui.S(30), chip.Y, cw - ui.S(34), chip.Height), c.text, 11.5f);
            cx += cw + ui.S(8);
            ++shown;
        }
        if (shown < (int)list.size())
            ui.Text(L"и ещё " + std::to_wstring(list.size() - shown), RectF(x, cy + ui.S(36), w, ui.S(22)), c.faint,
                    11.5f);
        y = cy + ui.S(64);
        (void)card;
    }
    bottom = y;
}

void Studio::TabIcon(const RectF& view, float y, float& bottom) {
    const UiColors& c = ui.C();
    Item* it = Sel();
    float x = view.X + ui.S(4), w = view.Width - ui.S(8);
    int r = ring_, i = sel_;
    float t = ui.S(140);
    iconTile_ = RectF(x, y, t, t);
    ui.Fill(iconTile_, c.card, ui.S(18));
    ui.Stroke(iconTile_, c.accent, ui.S(18), ui.S(1.5f), true);
    ui.Icon(IconFile(*it), Inset(iconTile_, ui.S(26), ui.S(26)), it->full);
    float bx = x + t + ui.S(22);
    ui.Text(L"Значок сектора", RectF(bx, y + ui.S(2), w, ui.S(26)), c.text, 15, true);
    ui.Text(L"Перетащи картинку из Проводника на квадрат или выбери ниже.", RectF(bx, y + ui.S(30), w - t, ui.S(22)),
            c.muted, 12);
    ui.Text(it->icon.empty() ? L"сейчас: системный значок" : L"сейчас: " + it->icon,
            RectF(bx, y + ui.S(54), w - t, ui.S(22)), c.faint, 11.5f);
    float by = y + ui.S(90);
    if (ui.Button(ID_ICON_FILE, RectF(bx, by, ui.S(110), ui.S(36)), L"Файл…"))
        Defer([this, r, i] {
            IFileOpenDialog* d = nullptr;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
            COMDLG_FILTERSPEC spec[] = {{L"Картинки", L"*.png;*.webp;*.jpg;*.jpeg;*.ico;*.bmp"}};
            d->SetFileTypes(1, spec);
            if (SUCCEEDED(d->Show(wnd))) {
                IShellItem* si = nullptr;
                if (SUCCEEDED(d->GetResult(&si))) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                        Change();
                        Ring(r)[i].icon = ShortPath(p);
                        Resolve(Ring(r)[i]);
                        Save();
                    }
                    CoTaskMemFree(p);
                    si->Release();
                }
            }
            d->Release();
        });
    if (ui.Button(ID_ICON_LIB, RectF(bx + ui.S(118), by, ui.S(150), ui.S(36)), L"Из библиотеки…")) {
        ov_ = OV_LIBRARY;
        if (libDirs_.empty()) {
            libDirs_.push_back(ExeDir() + L"\\icons");
            std::wstring lib = ExpandPath(cfg.library);
            if (lib.empty() || !IsFolder(lib)) {
                wchar_t buf[MAX_PATH];
                if (PathCanonicalizeW(buf, (ExeDir() + L"\\..\\image\\04 Иконки").c_str())) lib = buf;
            }
            std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& d, int depth) {
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
                if (h == INVALID_HANDLE_VALUE) return;
                bool has = false;
                std::vector<std::wstring> subs;
                do {
                    std::wstring n = fd.cFileName;
                    if (n == L"." || n == L".." || n[0] == L'.' || n[0] == L'_') continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        subs.push_back(d + L"\\" + n);
                    else if (IsImageFile(n))
                        has = true;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
                if (has && d != ExeDir() + L"\\icons") libDirs_.push_back(d);
                std::sort(subs.begin(), subs.end());
                if (depth < 5)
                    for (auto& s : subs) walk(s, depth + 1);
            };
            if (IsFolder(lib)) walk(lib, 0);
        }
        libCat_ = -1;
    }
    if (ui.Button(ID_ICON_SYS, RectF(bx + ui.S(276), by, ui.S(150), ui.S(36)), L"Системный", Kind::Ghost)) {
        Change();
        it->icon.clear();
        Resolve(*it);
        Save();
    }
    y += t + ui.S(28);

    ui.Text(L"Значки программы", RectF(x, y, w, ui.S(24)), c.text, 14, true);
    y += ui.S(34);
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
    for (int k2 = 0; k2 < (int)files.size(); ++k2) {
        RectF tr(x + (k2 % cols) * (tile + gap), y + (k2 / cols) * (tile + gap), tile, tile);
        RectF vis = Intersect(tr, view);
        int id = ID_GRID_ICON + k2;
        std::wstring rel = L"icons/" + files[k2];
        bool on = _wcsicmp(it->icon.c_str(), rel.c_str()) == 0 ||
                  _wcsicmp(it->icon.c_str(), (L"icons\\" + files[k2]).c_str()) == 0;
        if (vis.Height > 1 && ui.Click(id, vis)) {
            Change();
            it->icon = rel;
            Resolve(*it);
            Save();
        }
        ui.Fill(tr, on ? c.cardHi : ui.Hot(id) ? c.cardHi : c.card, ui.S(12));
        if (on) ui.Stroke(tr, c.accent, ui.S(12), ui.S(2));
        ui.Icon(ExeDir() + L"\\icons\\" + files[k2], Inset(tr, ui.S(10), ui.S(10)));
    }
    y += ((int)files.size() + cols - 1) / cols * (tile + gap);
    bottom = y;
}

}  // namespace studio
