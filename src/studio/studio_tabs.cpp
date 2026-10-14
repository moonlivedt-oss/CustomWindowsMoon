// Раздел «Кольца»: вкладки выбранного сектора - «Основное», «Второе кольцо», «Значок».
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
