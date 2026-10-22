// Студия, «Утилиты» - вкладка PATH: системный и свой PATH по порядку поиска, мёртвые папки, затенения,
// перетаскивание, переменные окружения.
// Раздел «Утилиты»: PATH и переменные, диски (место, здоровье, что занимает, кэши), автозагрузка, порты и блокировки.
// Долгое (сканирование, очистка, задача с правами) - в своём потоке, страница только читает результат.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "app/toolsui.h"
#include "core/bg.h"
#include "core/elevate.h"
#include "core/files.h"
#include "core/fx.h"
#include "core/text.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/hex.h"
#include "net/net.h"
#include "studio/studio_internal.h"
#include "studio/studio_layout.h"

using namespace Gdiplus;
using ui::Kind;

namespace studio {

void Studio::ToolsPathTab(ToolsKit& tk) {
    const UiColors& c = ui.C();
    int ndead = 0, ndup = 0;
    for (const tools::PathEntry& e : toolsPath_) {
        ndead += e.dead;
        ndup += e.dup && !e.dead;
    }
    size_t nsys = 0, sysBad = 0;
    for (const tools::PathEntry& e : toolsPath_)
        if (e.system) {
            ++nsys;
            sysBad += e.dead || e.dup;
        }
    size_t visible = toolsPath_.size() - (toolsShowSys_ ? 0 : nsys) + 1;
    tk.Wide(L"PATH", tk.RowsFor(visible) + 3.2f, [&](const RectF& b) {
        std::vector<std::wstring> user;
        for (const tools::PathEntry& e : toolsPath_)
            if (!e.system) user.push_back(e.raw);
        float y = b.Y;
        int ui_ = 0;
        // сначала свой PATH (его и правят), системный свёрнут: он длинный, а трогать его почти не нужно
        std::vector<size_t> order;
        for (size_t i = 0; i < toolsPath_.size(); ++i)
            if (!toolsPath_[i].system) order.push_back(i);
        order.push_back(SIZE_MAX);  // строка «системный PATH»
        if (toolsShowSys_)
            for (size_t i = 0; i < toolsPath_.size(); ++i)
                if (toolsPath_[i].system) order.push_back(i);
        // кто кого затеняет: команда есть в двух папках - побеждает та, что выше
        struct Link {
            size_t win, lose;
            std::wstring cmd;
        };
        std::vector<Link> links;
        {
            auto norm = [](std::wstring s) {
                while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
                CharLowerBuffW(s.data(), (DWORD)s.size());
                return s;
            };
            auto dirOf = [&](const std::wstring& file) {
                size_t cut = file.find_last_of(L'\\');
                return norm(cut == std::wstring::npos ? file : file.substr(0, cut));
            };
            std::vector<std::wstring> dirs;
            for (const tools::PathEntry& e : toolsPath_) dirs.push_back(norm(e.full));
            auto find = [&](const std::wstring& d) {
                for (size_t j = 0; j < dirs.size(); ++j)
                    if (dirs[j] == d) return j;
                return SIZE_MAX;
            };
            for (const tools::Which& w : toolsWhich_) {
                size_t wi = find(dirOf(w.path));
                if (wi == SIZE_MAX) continue;
                for (const std::wstring& o : w.other) {
                    size_t li = find(dirOf(o));
                    if (li != SIZE_MAX && li != wi) links.push_back({wi, li, w.cmd});
                }
            }
        }
        // свои строки можно перетаскивать за левый край; остальные расступаются
        size_t nUser = user.size();
        float userTop = y;
        POINT m = ui.Mouse();
        if (pathDrag_ >= (int)nUser) pathDrag_ = -1;
        if (pathDrag_ >= 0) {
            float at = m.y - pathGrab_ - userTop;
            pathDragTo_ = std::clamp((int)std::lround(at / tk.lineH), 0, (int)nUser - 1);
        }
        std::vector<float> rowY(toolsPath_.size(), -1);
        size_t hoverRow = SIZE_MAX;
        int dragRow = -1;
        for (size_t i : order) {
            if (i == SIZE_MAX) {
                RectF rr(b.X - ui.S(6), y, b.Width + ui.S(12), tk.lineH);
                if (tk.RowHover(ID_TOOLS_SYS_TOGGLE, rr)) toolsShowSys_ = !toolsShowSys_;
                ui.Text(std::wstring(toolsShowSys_ ? L"Скрыть" : L"Показать") + L" системный PATH (" +
                            std::to_wstring(nsys) + L")" +
                            (sysBad ? L", лишних " + std::to_wstring(sysBad) : std::wstring()) +
                            L" - Windows ищет в нём раньше, чем в своём",
                        RectF(b.X, y, b.Width, tk.lineH), sysBad ? c.gold : c.accent, 11.5f, true);
                y += tk.lineH;
                continue;
            }
            const tools::PathEntry& e = toolsPath_[i];
            int kk = e.system ? -1 : ui_++;
            float ry = y;
            if (kk >= 0) {
                // место строки с учётом перетаскиваемой, доезжает пружиной
                int slot = kk;
                if (pathDrag_ >= 0 && kk != pathDrag_) {
                    if (pathDrag_ < pathDragTo_ && kk > pathDrag_ && kk <= pathDragTo_) slot = kk - 1;
                    if (pathDragTo_ < pathDrag_ && kk >= pathDragTo_ && kk < pathDrag_) slot = kk + 1;
                }
                ry = userTop + ui.Glide(-2000 - kk, RectF(0, slot * tk.lineH, 1, 1)).Y;
                if (kk == pathDrag_) {
                    dragRow = (int)i;
                    rowY[i] = m.y - pathGrab_;
                    y += tk.lineH;
                    continue;  // рисуется последней, поверх остальных
                }
            }
            rowY[i] = ry;
            RectF rowR(b.X - ui.S(6), ry, b.Width + ui.S(12), tk.lineH);
            bool hot = pathDrag_ < 0 && tk.RowHot(rowR);
            if (hot) hoverRow = i;
            y += tk.lineH;
        }
        // дуги затенения для строки под курсором: от победителя к проигравшему, по ним бегут штрихи
        auto rowCenter = [&](size_t i) { return rowY[i] + tk.lineH / 2; };
        bool anyLink = false;
        for (const Link& l : links) {
            bool on = hoverRow != SIZE_MAX && (l.win == hoverRow || l.lose == hoverRow);
            if (!on || rowY[l.win] < 0 || rowY[l.lose] < 0) continue;
            anyLink = true;
            float y1 = rowCenter(l.win), y2 = rowCenter(l.lose);
            float bend = ui.S(10) + std::min(ui.S(10), std::fabs(y2 - y1) * 0.04f);
            Pen arc(c.gold, ui.S(1.8f));
            arc.SetDashStyle(DashStyleDash);
            arc.SetDashOffset(-(float)std::fmod(GetTickCount64() / std::max(1.0, fx::Dur(60)), 1000.0));
            arc.SetEndCap(LineCapArrowAnchor);
            ui.G().DrawBezier(&arc, PointF(b.X - ui.S(2), y1), PointF(b.X - bend - ui.S(4), y1),
                              PointF(b.X - bend - ui.S(4), y2), PointF(b.X - ui.S(2), y2));
        }
        if (anyLink && !fx::Game()) ui.Tick();
        auto drawRow = [&](size_t i, bool lifted) {
            const tools::PathEntry& e = toolsPath_[i];
            float ry = rowY[i];
            RectF rowR(b.X - ui.S(6), ry, b.Width + ui.S(12), tk.lineH);
            if (lifted) {
                ui.Fill(RectF(rowR.X - ui.S(2), rowR.Y + ui.S(2), rowR.Width + ui.S(4), rowR.Height + ui.S(3)),
                        WithAlpha(Color(0, 0, 0), 90), ui.S(8));
                ui.Fill(rowR, c.cardHi, ui.S(7));
                ui.Stroke(rowR, WithAlpha(c.accent, 200), ui.S(7), ui.S(1.4f));
            } else if (i == hoverRow && !e.system) {
                ui.Fill(rowR, WithAlpha(c.cardHi, 170), ui.S(6));
            }
            // подсветка участников затенения
            bool winner = false, loser = false;
            std::wstring loseTo;
            for (const Link& l : links) {
                if (hoverRow == SIZE_MAX || (l.win != hoverRow && l.lose != hoverRow)) continue;
                if (l.win == i) winner = true;
                if (l.lose == i) {
                    loser = true;
                    loseTo += (loseTo.empty() ? L"" : L", ") + l.cmd;
                }
            }
            if ((winner || loser) && i != hoverRow) ui.Fill(rowR, WithAlpha(c.gold, 22), ui.S(6));
            Color col = e.dead ? c.warn : e.dup ? c.gold : c.text;
            // левый край своей строки - ручка: под курсором вместо «свой» точки для перетаскивания
            int gid = ID_TOOLS_ROW + (int)i * 4 + 3;
            RectF grip(b.X - ui.S(6), ry, ui.S(62), tk.lineH);
            if (!e.system && !tk.busy) ui.Hover(gid, grip);
            if (!e.system && (ui.Hot(gid) || lifted)) {
                SolidBrush dot(lifted ? c.accent : c.muted);
                for (int r = 0; r < 3; ++r)
                    for (int q = 0; q < 2; ++q)
                        ui.G().FillEllipse(&dot, b.X + q * ui.S(6), ry + tk.lineH / 2 - ui.S(7) + r * ui.S(6), ui.S(3),
                                           ui.S(3));
                ui.Text(L"тяни", RectF(b.X + ui.S(16), ry, ui.S(48), tk.lineH), c.faint, 10.5f);
                ui.Tip(gid, L"Перетащи, чтобы поменять порядок: кто выше, того Windows находит первым");
            } else {
                ui.Text(e.system ? L"система" : L"свой", RectF(b.X, ry, ui.S(64), tk.lineH), c.faint, 10.5f);
            }
            float bw = e.system ? 0 : ui.S(210);
            std::wstring txt = e.raw + (e.dead ? L"   (нет папки)" : e.dup ? L"   (дубль)" : L"");
            ui.Text(txt, RectF(b.X + ui.S(66), ry, b.Width - ui.S(66) - bw - ui.S(150), tk.lineH), col, 11.5f);
            if (loser)
                ui.Text(L"уступает: " + loseTo, RectF(b.X + b.Width - bw - ui.S(148), ry, ui.S(144), tk.lineH), c.gold,
                        10.5f, true, ui::Right);
            else if (!e.provides.empty())
                ui.Text(e.provides, RectF(b.X + b.Width - bw - ui.S(148), ry, ui.S(144), tk.lineH),
                        winner ? c.gold : c.muted, 10.5f, winner, ui::Right);
            if (i == hoverRow && !e.system && !lifted) {
                float bx = b.X + b.Width - bw;
                int k = 0;
                for (size_t j = 0; j < i; ++j) k += !toolsPath_[j].system;
                auto move = [&](int to) {
                    std::vector<std::wstring> v = user;
                    std::swap(v[k], v[to]);
                    ToolsRun(L"PATH", [v](const tools::Log& l, ToolsJob&) { return tools::WriteUserPath(v, l); });
                    tk.Refresh();
                };
                if (ui.Button(ID_TOOLS_ROW + (int)i * 4, RectF(bx, ry + ui.S(2), ui.S(64), ui.S(26)), L"Выше") &&
                    k > 0 && !tk.busy)
                    move(k - 1);
                if (ui.Button(ID_TOOLS_ROW + (int)i * 4 + 1, RectF(bx + ui.S(70), ry + ui.S(2), ui.S(64), ui.S(26)),
                              L"Ниже") &&
                    k + 1 < (int)user.size() && !tk.busy)
                    move(k + 1);
                if (ui.Button(ID_TOOLS_ROW + (int)i * 4 + 2, RectF(bx + ui.S(140), ry + ui.S(2), ui.S(70), ui.S(26)),
                              L"Убрать") &&
                    !tk.busy) {
                    std::vector<std::wstring> v = user;
                    v.erase(v.begin() + k);
                    ToolsRun(L"PATH", [v](const tools::Log& l, ToolsJob&) { return tools::WriteUserPath(v, l); });
                    tk.Refresh();
                }
            }
            // перетаскивание: началось - запоминаем, отпустили - пишем новый порядок
            if (!e.system) {
                int k = 0;
                for (size_t j = 0; j < i; ++j) k += !toolsPath_[j].system;
                if (pathDrag_ < 0 && ui.Dragging(gid)) {
                    pathDrag_ = k;
                    pathDragTo_ = k;
                    pathGrab_ = ui.PressPt().y - ry;
                }
                if (ui.Released(gid) && pathDrag_ == k) {
                    int to = pathDragTo_;
                    pathDrag_ = -1;
                    ui.Invalidate();
                    if (to != k && to >= 0 && !tk.busy) {
                        std::vector<std::wstring> v = user;
                        std::wstring moved = v[k];
                        v.erase(v.begin() + k);
                        v.insert(v.begin() + to, moved);
                        ToolsRun(L"PATH", [v](const tools::Log& l, ToolsJob&) { return tools::WriteUserPath(v, l); });
                        tk.Refresh();
                    }
                }
            }
        };
        for (size_t i : order)
            if (i != SIZE_MAX && (int)i != dragRow && rowY[i] >= 0) drawRow(i, false);
        if (dragRow >= 0) {
            ui.Invalidate();
            drawRow((size_t)dragRow, true);
        }
        if (pathDrag_ >= 0 && !ui.Down()) pathDrag_ = -1;  // кнопку отпустили где-то вне окна
        y += ui.S(8);
        RectF fr(b.X, y, b.Width - ui.S(140), ui.S(36));
        FieldOrText(
            ID_TOOLS_ADD, fr, tk.view, toolsAddPath_, [this](const std::wstring& s) { toolsAddPath_ = s; },
            L"Папка для PATH пользователя");
        if (ui.Button(ID_TOOLS_ADD_BTN, RectF(b.X + b.Width - ui.S(130), y, ui.S(130), ui.S(36)), L"Добавить") &&
            !tk.busy) {
            std::wstring p = toolsAddPath_;
            ToolsRun(L"PATH", [p](const tools::Log& l, ToolsJob&) { return tools::AddUserPath(p, l); });
            toolsAddPath_.clear();
            tk.Refresh();
        }
        y += ui.S(46);
        float bw = (b.Width - ui.S(20)) / 3;
        if (ui.Button(ID_TOOLS_CLEAN, RectF(b.X, y, bw, ui.S(36)),
                      L"Почистить (" + std::to_wstring(ndead + ndup) + L")", Kind::Primary) &&
            !tk.busy) {
            ToolsRun(L"PATH", [](const tools::Log& l, ToolsJob&) { return tools::CleanUserPath(l) >= 0; });
            tk.Refresh();
        }
        if (ui.Button(ID_TOOLS_SYSCLEAN, RectF(b.X + bw + ui.S(10), y, bw, ui.S(36)), L"Системный (права)") &&
            !tk.busy) {
            ToolsRun(L"Системный PATH",
                     [](const tools::Log& l, ToolsJob&) { return net::RunAdmin(net::JOB_SYSPATH_CLEAN, l); });
            tk.Refresh();
        }
        if (ui.Button(ID_TOOLS_RESTORE, RectF(b.X + 2 * (bw + ui.S(10)), y, bw, ui.S(36)), L"Вернуть прошлый") &&
            !tk.busy) {
            std::vector<std::wstring> bk = tools::PathBackups();
            if (bk.empty()) {
                Toast(L"Бэкапов PATH пока нет");
            } else if (MessageBoxW(wnd, (L"Вернуть PATH пользователя из\n" + bk[0] + L"?").c_str(), L"PATH",
                                   MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::wstring f = bk[0];
                ToolsRun(L"PATH", [f](const tools::Log& l, ToolsJob&) { return tools::RestoreUserPath(f, l); });
                tk.Refresh();
            }
        }
        y += ui.S(46);
        tk.Note(b, (y - b.Y) / tk.rowH,
                (toolsJob_ ? tk.Log() + L"\n" : L"") +
                    L"Windows ищет программу сначала в системном PATH, потом в своём, и берёт первую найденную. "
                    L"Каждая правка сохраняет прежний PATH в бэкап.",
                c.muted);
    });
    tk.Wide(L"Что найдётся первым", tk.RowsFor(toolsWhich_.size()), [&](const RectF& b) {
        float y = b.Y;
        for (const tools::Which& w : toolsWhich_) {
            ui.Text(w.cmd, RectF(b.X, y, ui.S(70), tk.lineH), c.text, 12, true);
            ui.Text(w.path, RectF(b.X + ui.S(74), y, b.Width - ui.S(200), tk.lineH), c.muted, 11);
            if (!w.other.empty())
                ui.Text(L"затенено ещё " + std::to_wstring(w.other.size()),
                        RectF(b.X + b.Width - ui.S(122), y, ui.S(122), tk.lineH), c.gold, 10.5f, false, ui::Right);
            y += tk.lineH;
        }
    });
    tk.Wide(L"Переменные окружения", tk.RowsFor(toolsEnv_.size()) + 1.2f, [&](const RectF& b) {
        float y = b.Y;
        for (const tools::EnvVar& v : toolsEnv_) {
            ui.Text(v.system ? L"система" : L"свой", RectF(b.X, y, ui.S(64), tk.lineH), c.faint, 10.5f);
            ui.Text(v.name, RectF(b.X + ui.S(66), y, ui.S(200), tk.lineH), c.text, 11.5f, true);
            ui.Text(v.value + (v.deadPath ? L"   (нет такого пути)" : L""),
                    RectF(b.X + ui.S(270), y, b.Width - ui.S(270), tk.lineH), v.deadPath ? c.warn : c.muted, 11);
            y += tk.lineH;
        }
        y += ui.S(8);
        float w3 = (b.Width - ui.S(150)) / 2;
        FieldOrText(
            ID_TOOLS_ENV_NAME, RectF(b.X, y, w3 - ui.S(6), ui.S(36)), tk.view, toolsEnvName_,
            [this](const std::wstring& s) { toolsEnvName_ = s; }, L"Имя");
        FieldOrText(
            ID_TOOLS_ENV_VALUE, RectF(b.X + w3, y, w3 - ui.S(6), ui.S(36)), tk.view, toolsEnvValue_,
            [this](const std::wstring& s) { toolsEnvValue_ = s; }, L"Значение (пусто - удалить)");
        if (ui.Button(ID_TOOLS_ENV_SET, RectF(b.X + b.Width - ui.S(140), y, ui.S(140), ui.S(36)), L"Записать") &&
            !tk.busy) {
            std::wstring n = Trim(toolsEnvName_), v = toolsEnvValue_;
            if (!n.empty()) {
                ToolsRun(L"Переменная", [n, v](const tools::Log& l, ToolsJob&) { return tools::SetUserVar(n, v, l); });
                tk.Refresh();
            }
        }
    });
}

}  // namespace studio
