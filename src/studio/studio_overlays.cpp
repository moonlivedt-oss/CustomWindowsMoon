// Окна поверх: тур, журнал, библиотека значков, меню действий, уведомление.
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

// Уведомление внизу с кнопкой «Отменить»: держится 5 секунд.
void Studio::Toast(const std::wstring& s) {
    toast_ = s;
    toastUntil_ = GetTickCount64() + 5000;
    SetTimer(wnd, kToastTimer, 5050, nullptr);
    Log(s);
}

void Studio::DrawToast(float w, float h) {
    if (!toastUntil_ || GetTickCount64() >= toastUntil_) return;
    const UiColors& c = ui.C();
    float tw = std::min(w - ui.S(200), ui.Measure(toast_, 12.5f) + ui.S(160));
    RectF r((w + ui.S(78) - tw) / 2, h - ui.S(48) - ui.S(62), tw, ui.S(48));
    ui.Fill(r, MixColor(c.cardHi, c.accent, 0.08f), ui.S(14));
    ui.Stroke(r, c.accent, ui.S(14), ui.S(1.5f));
    ui.Text(toast_, RectF(r.X + ui.S(18), r.Y, r.Width - ui.S(150), r.Height), c.text, 12.5f);
    if (ui.Button(ID_TOAST_UNDO, RectF(r.X + r.Width - ui.S(128), r.Y + ui.S(8), ui.S(116), ui.S(32)), L"Отменить",
                  Kind::Primary))
        Undo();
}

void Studio::TourOverlay(float w, float h) {
    const UiColors& c = ui.C();
    ui.Fill(RectF(0, 0, w, h), Color(170, 0, 0, 0));
    ui.Hover(ID_OV_BG, RectF(0, 0, w, h));
    struct Step {
        const wchar_t* mascot;
        const wchar_t* title;
        const wchar_t* text;
    } steps[] = {
        {L"mascot-wave.webp", L"Привет! Это Radial Studio",
         L"Здесь настраиваются кольца, которые открываются, если зажать Alt и довести курсор до края экрана."},
        {L"mascot-point.webp", L"Кольца",
         L"Слева секторы выбранного края. Тяни их, чтобы поменять порядок, а папки и файлы бросай прямо в окно."},
        {L"mascot-surprised.webp", L"Темы",
         L"Тема красит и кольца, и саму студию. Двенадцать штук, включая цвет из обоев и свою палитру."},
        {L"mascot-read.webp", L"Сохранять не нужно",
         L"Всё сохраняется само. Ошибся - «Отменить» внизу. «Показать меню» сразу открывает кольцо для проверки."},
    };
    const Step& s = steps[tourStep_];
    RectF card((w - ui.S(600)) / 2, (h - ui.S(300)) / 2, ui.S(600), ui.S(300));
    ui.Fill(card, c.card, ui.S(18));
    ui.Stroke(card, c.accent, ui.S(18), ui.S(1.5f));
    ui.Hover(ID_OV_BG + 1, card);
    ui.Icon(ExeDir() + L"\\icons\\" + s.mascot, RectF(card.X + ui.S(24), card.Y + ui.S(50), ui.S(170), ui.S(170)));
    float tx = card.X + ui.S(214), tw = card.Width - ui.S(240);
    ui.Text(L"Шаг " + std::to_wstring(tourStep_ + 1) + L" из 4", RectF(tx, card.Y + ui.S(34), tw, ui.S(20)), c.accent,
            11.5f, true);
    ui.Text(s.title, RectF(tx, card.Y + ui.S(56), tw, ui.S(32)), c.text, 18, true);
    ui.Text(s.text, RectF(tx, card.Y + ui.S(96), tw, ui.S(110)), c.muted, 13, false, ui::Left, true);
    float by = card.Y + card.Height - ui.S(62);
    bool last = tourStep_ == 3;
    if (ui.Button(ID_TOUR_NEXT, RectF(card.X + card.Width - ui.S(146), by, ui.S(122), ui.S(38)),
                  last ? L"Готово" : L"Дальше", Kind::Primary)) {
        if (last) {
            ov_ = OV_NONE;
            if (!cfg.tourDone) {
                cfg.tourDone = true;
                Save();
            }
        } else {
            ++tourStep_;
        }
    }
    if (tourStep_ > 0 &&
        ui.Button(ID_TOUR_BACK, RectF(card.X + card.Width - ui.S(270), by, ui.S(116), ui.S(38)), L"Назад"))
        --tourStep_;
    if (!last && ui.Button(ID_TOUR_SKIP, RectF(tx - ui.S(8), by, ui.S(120), ui.S(38)), L"Пропустить", Kind::Ghost)) {
        ov_ = OV_NONE;
        cfg.tourDone = true;
        Save();
    }
    for (int i = 0; i < 4; ++i) {
        SolidBrush b(i == tourStep_ ? c.accent : c.border);
        ui.G().FillEllipse(&b, card.X + ui.S(84) + i * ui.S(14), card.Y + card.Height - ui.S(36), ui.S(8), ui.S(8));
    }
}

void Studio::JournalOverlay(float w, float h) {
    const UiColors& c = ui.C();
    float statusH = ui.S(48);
    if (ui.Click(ID_OV_BG, RectF(0, 0, w, h - statusH))) {
        ov_ = OV_NONE;
        return;
    }
    RectF p(w - ui.S(490), h - statusH - ui.S(500), ui.S(476), ui.S(494));
    ui.Fill(p, c.card, ui.S(14));
    ui.Stroke(p, c.border, ui.S(14));
    ui.Hover(ID_OV_BG + 1, p);
    ui.Text(L"Журнал", RectF(p.X + ui.S(16), p.Y + ui.S(10), p.Width, ui.S(28)), c.text, 14, true);
    RectF view(p.X + ui.S(8), p.Y + ui.S(44), p.Width - ui.S(16), ui.S(220));
    float lh = ui.S(28);
    float off = ui.Scroll(ID_JOURNAL_SCROLL, view, log_.size() * lh);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(view);
    if (log_.empty()) ui.Text(L"Пока пусто", Inset(view, ui.S(8), 0), c.faint, 12);
    float y = view.Y - off;
    for (auto it = log_.rbegin(); it != log_.rend(); ++it, y += lh) {
        ui.Text(it->first, RectF(view.X + ui.S(8), y, ui.S(50), lh), c.faint, 11.5f);
        ui.Text(it->second, RectF(view.X + ui.S(60), y, view.Width - ui.S(64), lh), c.text, 12);
    }
    ui.G().Restore(st);

    // резервные копии radial.cfg
    float by = view.Y + view.Height + ui.S(14);
    ui.Fill(RectF(p.X + ui.S(16), by - ui.S(7), p.Width - ui.S(32), 1), c.border);
    ui.Text(L"Резервные копии", RectF(p.X + ui.S(16), by, p.Width, ui.S(26)), c.text, 14, true);
    ui.Text(L"до 10 штук, не чаще раза в 10 минут", RectF(p.X + ui.S(170), by, p.Width - ui.S(186), ui.S(26)), c.faint,
            11, false, ui::Right);
    RectF bv(p.X + ui.S(8), by + ui.S(32), p.Width - ui.S(16), p.Y + p.Height - ui.S(10) - (by + ui.S(32)));
    auto list = Backups();
    float bh = ui.S(38);
    float boff = ui.Scroll(ID_BACKUP - 1, bv, list.size() * bh);
    st = ui.G().Save();
    ui.G().SetClip(bv);
    if (list.empty()) ui.Text(L"Появятся после первого изменения", Inset(RectF(bv.X, bv.Y, bv.Width, bh), ui.S(8), 0), c.faint, 12);
    for (int i = 0; i < (int)list.size(); ++i) {
        RectF ir(bv.X, bv.Y + i * bh - boff, bv.Width, bh - ui.S(4));
        // radial-2026-10-14_11-09-26.cfg -> 2026-10-14 11:09
        std::wstring d = list[i].size() >= 23 ? list[i].substr(7, 10) + L"  " + list[i].substr(18, 2) + L":" + list[i].substr(21, 2) : list[i];
        ui.Text(d, RectF(ir.X + ui.S(8), ir.Y, ir.Width, ir.Height), c.text, 12);
        RectF btn(ir.X + ir.Width - ui.S(110), ir.Y + ui.S(3), ui.S(104), ir.Height - ui.S(6));
        if (Intersect(btn, bv).Height > btn.Height - 1 && ui.Button(ID_BACKUP + i, btn, L"Вернуть")) {
            std::wstring f = list[i];
            ov_ = OV_NONE;
            Restore(f);
            break;
        }
    }
    ui.G().Restore(st);
}

void Studio::LibraryOverlay(float w, float h) {
    const UiColors& c = ui.C();
    ui.Fill(RectF(0, 0, w, h), Color(170, 0, 0, 0));
    if (ui.Click(ID_OV_BG, RectF(0, 0, w, h))) {
        ov_ = OV_NONE;
        return;
    }
    RectF p(ui.S(70), ui.S(50), w - ui.S(140), h - ui.S(100));
    ui.Fill(p, c.card, ui.S(18));
    ui.Stroke(p, c.border, ui.S(18));
    ui.Hover(ID_OV_BG + 1, p);
    ui.Text(L"Значок из библиотеки", RectF(p.X + ui.S(20), p.Y + ui.S(14), p.Width, ui.S(30)), c.text, 16, true);
    if (ui.Button(ID_LIB_CLOSE, RectF(p.X + p.Width - ui.S(130), p.Y + ui.S(12), ui.S(112), ui.S(34)), L"Закрыть"))
        ov_ = OV_NONE;
    // категории
    RectF cats(p.X + ui.S(12), p.Y + ui.S(58), ui.S(260), p.Height - ui.S(70));
    float lh = ui.S(34);
    float coff = ui.Scroll(ID_LIB_CATS_SCROLL, cats, libDirs_.size() * lh);
    GraphicsState st = ui.G().Save();
    ui.G().SetClip(cats);
    std::wstring libRoot;
    if (libDirs_.size() > 1) {
        libRoot = libDirs_[1];
        for (size_t i = 2; i < libDirs_.size(); ++i)
            while (!libRoot.empty() && libDirs_[i].rfind(libRoot, 0) != 0)
                libRoot = libRoot.substr(0, libRoot.find_last_of(L'\\'));
    }
    for (int i = 0; i < (int)libDirs_.size(); ++i) {
        RectF ir(cats.X, cats.Y + i * lh - coff, cats.Width, lh - ui.S(4));
        RectF vis = Intersect(ir, cats);
        int id = ID_LIB_CAT + i;
        if (vis.Height > 1 && ui.Click(id, vis)) libCat_ = i;
        if (libCat_ == i)
            ui.Fill(ir, c.cardHi, ui.S(8));
        else if (ui.Hot(id))
            ui.Fill(ir, c.field, ui.S(8));
        std::wstring label =
            i == 0 ? L"Значки программы"
                   : (libDirs_[i].size() > libRoot.size() && !libRoot.empty() ? libDirs_[i].substr(libRoot.size() + 1)
                                                                              : libDirs_[i]);
        ui.Text(label, Inset(ir, ui.S(10), 0), libCat_ == i ? c.text : c.muted, 12);
    }
    ui.G().Restore(st);
    if (libCat_ < 0 && !libDirs_.empty()) libCat_ = 0;
    // файлы категории
    static int loadedCat = -2;
    if (loadedCat != libCat_ && libCat_ >= 0) {
        loadedCat = libCat_;
        libFiles_.clear();
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW((libDirs_[libCat_] + L"\\*").c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImageFile(fd.cFileName))
                    libFiles_.push_back(libDirs_[libCat_] + L"\\" + fd.cFileName);
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        std::sort(libFiles_.begin(), libFiles_.end());
        ui.ResetScroll(ID_LIB_GRID_SCROLL);
    }
    RectF grid(cats.X + cats.Width + ui.S(14), cats.Y, p.X + p.Width - ui.S(12) - (cats.X + cats.Width + ui.S(14)),
               cats.Height);
    float tile = ui.S(84), gap = ui.S(10);
    int cols = std::max(1, (int)((grid.Width + gap) / (tile + gap)));
    float goff = ui.Scroll(ID_LIB_GRID_SCROLL, grid, ((int)libFiles_.size() + cols - 1) / cols * (tile + gap));
    st = ui.G().Save();
    ui.G().SetClip(grid);
    int budget = 10;
    for (int i = 0; i < (int)libFiles_.size(); ++i) {
        RectF tr(grid.X + (i % cols) * (tile + gap), grid.Y + (i / cols) * (tile + gap) - goff, tile, tile);
        RectF vis = Intersect(tr, grid);
        if (vis.Height < 1) continue;
        int id = ID_LIB_ICON + i;
        if (ui.Click(id, vis)) {
            Item* it = Sel();
            if (it) {
                Change();
                it->icon = ShortPath(libFiles_[i]);
                Resolve(*it);
                Log(L"Значок: " + std::wstring(PathFindFileNameW(libFiles_[i].c_str())));
                Save();
            }
            ov_ = OV_NONE;
        }
        ui.Fill(tr, ui.Hot(id) ? c.cardHi : c.field, ui.S(12));
        // миниатюры догружаются по несколько за кадр, чтобы окно не замирало
        if (thumbsDone_.count(libFiles_[i]) || budget-- > 0) {
            thumbsDone_.insert(libFiles_[i]);
            ui.Icon(libFiles_[i], Inset(tr, ui.S(12), ui.S(12)));
        } else {
            ui.Invalidate();
        }
    }
    ui.G().Restore(st);
}

void Studio::MoreOverlay() {
    const UiColors& c = ui.C();
    if (ui.Click(ID_OV_BG, RectF(0, 0, 100000, 100000))) {
        ov_ = OV_NONE;
        return;
    }
    struct Entry {
        int op;
        const wchar_t* name;
        const wchar_t* key;
    } items[] = {{OP_RENAME, L"Переименовать", L"F2"},         {OP_UP, L"Выше", L"Alt+\u2191"},
                 {OP_DOWN, L"Ниже", L"Alt+\u2193"},            {OP_OTHER_SIDE, L"На другой край", L""},
                 {OP_DUPLICATE, L"Дублировать", L"Ctrl+D"},    {OP_DELETE, L"Удалить", L"Delete"}};
    const int n = 6;
    float iw = ui.S(240), ih = ui.S(38);
    RectF p(moreAnchor_.X + moreAnchor_.Width - iw, moreAnchor_.Y + moreAnchor_.Height + ui.S(6), iw, ih * n + ui.S(12));
    if (moreAtMouse_) p = RectF(moreAnchor_.X, moreAnchor_.Y, iw, ih * n + ui.S(12));
    RECT rc;
    GetClientRect(wnd, &rc);
    p.X = std::clamp(p.X, ui.S(4), (float)rc.right - p.Width - ui.S(4));
    p.Y = std::clamp(p.Y, ui.S(4), (float)rc.bottom - p.Height - ui.S(4));
    ui.Fill(p, c.cardHi, ui.S(12));
    ui.Stroke(p, c.border, ui.S(12));
    ui.Hover(ID_OV_BG + 1, p);
    for (int i = 0; i < n; ++i) {
        RectF ir(p.X + ui.S(6), p.Y + ui.S(6) + i * ih, p.Width - ui.S(12), ih);
        int id = ID_MORE_ITEM + i;
        bool clicked = ui.Click(id, ir);
        if (ui.Hot(id)) ui.Fill(ir, c.card, ui.S(8));
        ui.Text(items[i].name, Inset(ir, ui.S(12), 0), items[i].op == OP_DELETE ? c.warn : c.text, 12.5f);
        ui.Text(items[i].key, Inset(ir, ui.S(12), 0), c.faint, 11, false, ui::Right);
        if (clicked) {
            ov_ = OV_NONE;
            moreAtMouse_ = false;
            SectorOp(items[i].op);
            return;
        }
    }
}

}  // namespace studio
