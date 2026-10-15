// Окно Radial Studio: кадр, колонка разделов, строка статуса, сообщения Windows.
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

#include "core/mem.h"
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

namespace {
Studio* g_st = nullptr;

// Группы боковой панели: в каждой - разделы, которые показываются вкладками над страницей.
struct GroupPage {
    Page page;
    const wchar_t* name;
};
struct Group {
    const wchar_t* name;
    const wchar_t* own;  // свой значок группы (icons\rail-*.png), пока его нет - общий
    const wchar_t* icon;
    GroupPage pages[3];
    int count;
};
const Group kGroups[] = {
    {L"Обзор", L"rail-home.png", L"hub.png", {{PAGE_HOME, L"Обзор"}}, 1},
    {L"Кольца", L"rail-rings.png", L"apps.png", {{PAGE_RINGS, L"Кольца"}, {PAGE_THEMES, L"Темы"}}, 2},
    {L"Стол",
     L"rail-desk.png",
     L"desktop.png",
     {{PAGE_DESK, L"Углы и панель"}, {PAGE_STYLE, L"Красота"}, {PAGE_SYSTEM, L"Система"}},
     3},
    {L"Сервис", L"rail-tools.png", L"tools-box.png", {{PAGE_NET, L"Сеть"}, {PAGE_TOOLS, L"Утилиты"}}, 2},
    {L"Настройки", L"rail-behavior.png", L"editor.png", {{PAGE_BEHAVIOR, L"Поведение"}, {PAGE_KEYS, L"Клавиши"}}, 2},
};
constexpr int kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);

int GroupOf(Page p) {
    for (int g = 0; g < kGroupCount; ++g)
        for (int k = 0; k < kGroups[g].count; ++k)
            if (kGroups[g].pages[k].page == p) return g;
    return 0;
}
}  // namespace

void Studio::Paint() {
    PERF_SCOPE("studio.Paint");
    // по одному имени на каждый раздел из enum Page, в том же порядке
    static const char* kPageScope[] = {
        "studio.page.rings", "studio.page.themes", "studio.page.behavior", "studio.page.desk", "studio.page.system",
        "studio.page.style", "studio.page.net",    "studio.page.tools",    "studio.page.home", "studio.page.keys"};
    static_assert(sizeof(kPageScope) / sizeof(kPageScope[0]) == PAGE_KEYS + 1, "имя замера на каждый раздел");
    perf::Scope pageScope(kPageScope[page_]);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    RECT rc;
    GetClientRect(wnd, &rc);
    int w = std::max(1L, rc.right), h = std::max(1L, rc.bottom);
    if (!memDC_ || w != bw_ || h != bh_) {
        if (memDC_) {
            SelectObject(memDC_, oldBmp_);
            DeleteObject(dib_);
            DeleteDC(memDC_);
        }
        memDC_ = CreateCompatibleDC(dc);
        dib_ = CreateCompatibleBitmap(dc, w, h);
        oldBmp_ = SelectObject(memDC_, dib_);
        bw_ = w;
        bh_ = h;
    }
    {
        Graphics g(memDC_);
        Config view = cfg;
        if (!hoverTheme_.empty() && page_ == PAGE_THEMES && ov_ == OV_NONE) view.theme = hoverTheme_;
        UiColors target = UiPalette(view);
        ULONGLONG now = GetTickCount64();
        float dt = lastPaint_ ? std::min(0.1f, (now - lastPaint_) / 1000.0f) : 1.0f;
        lastPaint_ = now;
        if (!curUiReady_) {
            curUi_ = target;
            curUiReady_ = true;
        }
        curUi_ = MixUi(curUi_, target, std::min(1.0f, dt * 11));
        bool settling = !SameUi(curUi_, target);
        if (!settling) curUi_ = target;
        hoverThemeNext_.clear();
        ui.Begin(g, curUi_, k);
        Frame((float)w, (float)h);
        ui.End();
        if (settling) ui.Invalidate();
        if (hoverThemeNext_ != hoverTheme_) {
            hoverTheme_ = hoverThemeNext_;
            ui.Invalidate();
        }
    }
    BitBlt(dc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    EndPaint(wnd, &ps);
    if (ui.TakeRepaint()) InvalidateRect(wnd, nullptr, FALSE);
}

void Studio::Frame(float w, float h) {
    const UiColors& c = ui.C();
    ui.Fill(RectF(0, 0, w, h), c.win);
    float rail = ui.S(78), status = ui.S(48);
    ui.interactive = ov_ == OV_NONE;
    Rail(RectF(0, 0, rail, h));
    RectF body(rail, 0, w - rail, h - status);
    float tabsH = SubTabs(body);  // вкладки группы (Кольца | Темы...) над страницей
    body.Y += tabsH;
    body.Height -= tabsH;
    childDragShown_ = childDrag_;
    childDrag_ = -1;
    switch (page_) {
        case PAGE_RINGS:
            RingsPage(body);
            break;
        case PAGE_THEMES:
            ThemesPage(body);
            break;
        case PAGE_BEHAVIOR:
            BehaviorPage(body);
            break;
        case PAGE_DESK:
            DeskPage(body);
            break;
        case PAGE_SYSTEM:
            SystemPage(body);
            break;
        case PAGE_STYLE:
            StylePage(body);
            break;
        case PAGE_NET:
            NetPage(body);
            break;
        case PAGE_TOOLS:
            ToolsPage(body);
            break;
        case PAGE_HOME:
            HomePage(body);
            break;
        case PAGE_KEYS:
            KeysPage(body);
            break;
    }
    // смена раздела: страница проступает за 180 мс
    float fade = 1 - std::min(1.0f, (GetTickCount64() - pageAt_) / 180.0f);
    if (fade > 0) {
        ui.Fill(body, Color((BYTE)(255 * fade * 0.85f), c.win.GetR(), c.win.GetG(), c.win.GetB()));
        ui.Invalidate();
    }
    ui.interactive = ov_ == OV_NONE || ov_ == OV_JOURNAL;  // «Журнал» внизу закрывает сам себя
    StatusBar(RectF(rail, h - status, w - rail, status));
    ui.interactive = ov_ == OV_NONE;
    DrawToast(w, h);  // под всплывающими окнами, чтобы не налезать на журнал
    ui.interactive = true;
    if (ov_ == OV_TOUR) TourOverlay(w, h);
    if (ov_ == OV_JOURNAL) JournalOverlay(w, h);
    if (ov_ == OV_LIBRARY) LibraryOverlay(w, h);
    if (ov_ == OV_MORE) MoreOverlay();
    ui.DrawTip(w, h);
}

void Studio::Card(const RectF& r, bool hi) {
    const UiColors& c = ui.C();
    ui.Fill(r, hi ? c.cardHi : c.card, ui.S(12));
    ui.Stroke(r, c.border, ui.S(12));
}

void Studio::Header(const RectF& r, const std::wstring& title, const std::wstring& sub) {
    ui.Text(title, RectF(r.X, r.Y, r.Width, ui.S(32)), ui.C().text, 21, true);
    ui.Text(sub, RectF(r.X, r.Y + ui.S(32), r.Width, ui.S(20)), ui.C().muted, 12);
}

RectF Studio::Column(const RectF& in, float maxW) const {
    float w = std::min(in.Width, maxW);
    return RectF(in.X + (in.Width - w) / 2, in.Y, w, in.Height);
}

void Studio::Mascot(const RectF& r, const std::wstring& file, const std::wstring& title, const std::wstring& text) {
    float m = std::min(ui.S(120), r.Height * 0.45f);
    ui.Icon(ExeDir() + L"\\icons\\" + file, RectF(r.X + (r.Width - m) / 2, r.Y, m, m));
    ui.Text(title, RectF(r.X, r.Y + m + ui.S(8), r.Width, ui.S(26)), ui.C().text, 15, true, ui::Center);
    ui.Text(text, RectF(r.X + ui.S(10), r.Y + m + ui.S(36), r.Width - ui.S(20), ui.S(60)), ui.C().muted, 12, false,
            ui::Center, true);
}

bool Studio::FieldOrText(int id, const RectF& r, const RectF& view, const std::wstring& value,
                         std::function<void(const std::wstring&)> onChange, const std::wstring& ph) {
    if (Inside(r, view) && ui.interactive) {
        ui.Field(id, r, value, std::move(onChange), ph);
        return true;
    }
    if (r.Y > view.Y + view.Height || r.Y + r.Height < view.Y) return false;
    ui.Fill(r, ui.C().field, ui.S(9));
    ui.Stroke(r, ui.C().border, ui.S(9));
    ui.Text(value.empty() ? ph : value, Inset(r, ui.S(11), 0), value.empty() ? ui.C().faint : ui.C().text, 13);
    return false;
}

void Studio::Rail(const RectF& r) {
    const UiColors& c = ui.C();
    ui.Fill(r, c.rail);
    ui.Icon(ExeDir() + L"\\icons\\app.png", RectF(r.X + (r.Width - ui.S(40)) / 2, ui.S(14), ui.S(40), ui.S(40)));
    // свой значок раздела (icons\rail-*.png из набора 2), пока его нет - прежний общий
    auto railIcon = [](const wchar_t* own, const wchar_t* fallback) {
        std::wstring p = ExeDir() + L"\\icons\\" + own;
        return Exists(p) ? p : ExeDir() + L"\\icons\\" + fallback;
    };
    // пять групп вместо десяти разделов; внутри группы - вкладки над страницей (SubTabs)
    float y = ui.S(72);
    for (int gi = 0; gi < kGroupCount; ++gi) {
        const Group& e = kGroups[gi];
        RectF b(r.X + ui.S(7), y, r.Width - ui.S(14), ui.S(66));
        bool on = GroupOf(page_) == gi;
        if (ui.Click(ID_RAIL + gi, b) && !on) {
            pageAt_ = GetTickCount64();
            renaming_ = -1;
            page_ = groupLast_[gi];
            ui.ResetScroll(ID_PAGE_SCROLL);
        }
        // выбранная группа: фон и полоска акцента у левого края; она вырастает на новой группе и
        // гаснет на прежней, при наведении на другую видна короткая бледная полоска
        float sel = ui.Anim(ID_RAIL_BAR + gi, on, 16);
        float hov = ui.Anim(ID_RAIL_HOVER + gi, !on && ui.Hot(ID_RAIL + gi), 18);
        if (sel > 0.01f) ui.Fill(b, WithAlpha(c.cardHi, (BYTE)(255 * sel)), ui.S(12));
        if (hov > 0.01f) ui.Fill(b, WithAlpha(c.card, (BYTE)(255 * hov * (1 - sel))), ui.S(12));
        float barH = ui.S(34) * sel + ui.S(14) * hov * (1 - sel);
        if (barH > ui.S(1)) {
            Color bar = sel > hov ? WithAlpha(c.accent, (BYTE)(255 * std::min(1.0f, sel * 1.4f)))
                                  : WithAlpha(c.muted, (BYTE)(160 * hov));
            ui.Fill(RectF(r.X + ui.S(1), b.Y + (b.Height - barH) / 2, ui.S(4), barH), bar, ui.S(2));
        }
        ui.Icon(railIcon(e.own, e.icon), RectF(b.X + (b.Width - ui.S(30)) / 2, b.Y + ui.S(8), ui.S(30), ui.S(30)));
        ui.Text(e.name, RectF(b.X - ui.S(4), b.Y + ui.S(42), b.Width + ui.S(8), ui.S(18)), on ? c.text : c.muted, 10.5f,
                on, ui::Center);
        std::wstring tip;
        for (int k = 0; k < e.count; ++k) tip += (k ? L", " : L"") + std::wstring(e.pages[k].name);
        if (e.count > 1) ui.Tip(ID_RAIL + gi, tip);
        y += ui.S(72);
    }
    RectF t(r.X + ui.S(7), r.Height - ui.S(122), r.Width - ui.S(14), ui.S(66));
    if (ui.Click(ID_TOUR_BTN, t)) {
        ov_ = OV_TOUR;
        tourStep_ = 0;
    }
    if (ui.Hot(ID_TOUR_BTN)) ui.Fill(t, c.card, ui.S(12));
    ui.Icon(railIcon(L"rail-tour.png", L"study.png"),
            RectF(t.X + (t.Width - ui.S(30)) / 2, t.Y + ui.S(8), ui.S(30), ui.S(30)));
    ui.Text(L"Тур", RectF(t.X, t.Y + ui.S(42), t.Width, ui.S(18)), c.muted, 10.5f, false, ui::Center);
}

float Studio::SubTabs(const RectF& body) {
    int gi = GroupOf(page_);
    groupLast_[gi] = page_;
    const Group& g = kGroups[gi];
    if (g.count < 2) return 0;
    const UiColors& c = ui.C();
    float x = body.X + ui.S(28), y = ui.S(14), h = ui.S(34);
    for (int k = 0; k < g.count; ++k) {
        const wchar_t* name = g.pages[k].name;
        float w = ui.Measure(name, 12.5f, true) + ui.S(30);
        RectF tr(x, y, w, h);
        bool on = page_ == g.pages[k].page;
        int id = ID_SUBTAB + g.pages[k].page;
        if (ui.Click(id, tr) && !on) {
            Page p = g.pages[k].page;
            Defer([this, p] { GoPage(p); });
        }
        float hov = ui.Anim(id + 40, !on && ui.Hot(id), 18);
        if (on)
            ui.Fill(tr, c.accent, h / 2);
        else
            ui.Fill(tr, WithAlpha(c.cardHi, (BYTE)(90 + 140 * hov)), h / 2);
        ui.Text(name, tr, on ? c.onAccent : c.muted, 12.5f, on, ui::Center);
        x += w + ui.S(8);
    }
    return ui.S(44);
}

void Studio::StatusBar(const RectF& r) {
    const UiColors& c = ui.C();
    RectF bar = Inset(r, ui.S(12), ui.S(6));
    ui.Fill(bar, c.card, ui.S(10));
    ui.Stroke(bar, c.border, ui.S(10));
    SolidBrush dot(statusFresh_ ? c.accent : c.faint);
    float d = ui.S(9);
    ui.G().FillEllipse(&dot, bar.X + ui.S(14), bar.Y + (bar.Height - d) / 2, d, d);
    ui.Text(status_, RectF(bar.X + ui.S(32), bar.Y, bar.Width - ui.S(300), bar.Height), c.muted, 12);
    RectF j(bar.X + bar.Width - ui.S(104), bar.Y + ui.S(4), ui.S(98), bar.Height - ui.S(8));
    if (ui.Button(ID_JOURNAL, j, L"Журнал", Kind::Secondary)) ov_ = ov_ == OV_JOURNAL ? OV_NONE : OV_JOURNAL;
    ui.Tip(ID_JOURNAL, L"История изменений и резервные копии radial.cfg");
    RectF u(j.X - ui.S(118), j.Y, ui.S(110), j.Height);
    if (ui.Button(ID_UNDO, u, L"Отменить", undo_.empty() ? Kind::Ghost : Kind::Secondary)) Undo();
    ui.Tip(ID_UNDO,
           undo_.empty() ? L"Пока нечего отменять" : L"Отменить последнее изменение (Ctrl+Z). Вернуть - Ctrl+Y");
}

void Studio::GoPage(Page p) {
    if (page_ == p) return;
    pageAt_ = GetTickCount64();
    renaming_ = -1;
    page_ = p;
    ui.ResetScroll(ID_PAGE_SCROLL);
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

int Studio::Snapshot(int page, int tab, const std::wstring& out, int ms) {
    OleInitialize(nullptr);  // как в программе: значки (WIC) и планировщик (автозапуск) идут через COM
    LoadConfig(cfgPath, cfg);
    ApplyAccent();
    Recheck();
    k = 1.0;
    const int w = 1240, h = 800;
    wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"Radial Studio snapshot", WS_POPUP, -5000, -5000, w, h, nullptr,
                          nullptr, inst, nullptr);  // не показывается
    ui.Attach(wnd);
    ov_ = OV_NONE;
    if (page == PAGE_TOOLS)
        GoTools(tab, L"");
    else
        GoPage((Page)page);
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    ULONGLONG end = GetTickCount64() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        pageAt_ = 0;  // без проступания страницы
        Graphics g(&bmp);
        ui.Begin(g, UiPalette(cfg), k);
        Frame((float)w, (float)h);
        ui.End();
        Sleep(120);
    } while (GetTickCount64() < end);
    CLSID png{};
    UINT n = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&n, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png")) png = codecs[i].Clsid;
    return bmp.Save(out.c_str(), &png, nullptr) == Gdiplus::Ok ? 0 : 1;
}

void Studio::Open() {
    LoadConfig(cfgPath, cfg);
    ApplyAccent();
    Recheck();
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    k = dx / 96.0;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int w = std::min((int)(1240 * k), (int)(mi.rcWork.right - mi.rcWork.left) - 40);
    int h = std::min((int)(800 * k), (int)(mi.rcWork.bottom - mi.rcWork.top) - 40);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    wnd = CreateWindowExW(0, kClass, L"Radial Studio", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr,
                          nullptr, inst, nullptr);
    ui.Attach(wnd);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(wnd, 20, &dark, sizeof(dark));  // тёмный заголовок окна
    UiColors c = UiPalette(cfg);
    COLORREF cap = RGB(c.rail.GetR(), c.rail.GetG(), c.rail.GetB());
    DwmSetWindowAttribute(wnd, 35, &cap, sizeof(cap));  // цвет заголовка (Windows 11)
    DragAcceptFiles(wnd, TRUE);
    if (!cfg.tourDone) ov_ = OV_TOUR;
    ShowWindow(wnd, SW_SHOWNORMAL);
    SetForegroundWindow(wnd);
}

LRESULT Studio::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    auto after = [&] {
        if (ui.TakeRepaint()) InvalidateRect(wnd, nullptr, FALSE);
    };
    switch (msg) {
        case WM_PAINT: {
            Paint();
            // заголовок окна следует за темой
            UiColors c = UiPalette(cfg);
            COLORREF cap = RGB(c.rail.GetR(), c.rail.GetG(), c.rail.GetB());
            DwmSetWindowAttribute(wnd, 35, &cap, sizeof(cap));
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
            TrackMouseEvent(&tme);
            ui.OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp));
            if (ui.HotChanged()) SetTimer(wnd, kTipTimer, 650, nullptr);  // подсказка появится, если задержаться
            after();
            return 0;
        }
        case WM_MOUSELEAVE:
            ui.OnLeave();
            after();
            return 0;
        case WM_LBUTTONDOWN:
            ui.OnDown((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_LBUTTONUP:
            ui.OnUp((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_LBUTTONDBLCLK:
            ui.OnDown((short)LOWORD(lp), (short)HIWORD(lp));
            ui.OnDouble((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_RBUTTONDOWN:
            ui.OnRightDown((short)LOWORD(lp), (short)HIWORD(lp));
            return 0;
        case WM_RBUTTONUP:
            ui.OnRightUp((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(wnd, &p);
            ui.OnWheel(p.x, p.y, (short)HIWORD(wp));
            after();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (HWND)wp == wnd && ui.OnSetCursor()) return TRUE;
            break;
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE && ui.OnEditChange((HWND)lp)) after();
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, ui.FieldText());
            SetBkColor(dc, ui.FieldBack());
            return (LRESULT)ui.FieldBrush();
        }
        case WM_TIMER:
            if (wp == kSaveTimer) Save();
            if (wp == kTipTimer || wp == kToastTimer) {
                KillTimer(wnd, wp);
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;
        case WM_RUN: {
            auto todo = std::move(deferred_);
            deferred_.clear();
            for (auto& f : todo) f();
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            POINT pt;
            DragQueryPoint(drop, &pt);
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> paths;
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH];
                if (DragQueryFileW(drop, i, buf, MAX_PATH)) paths.push_back(buf);
            }
            DragFinish(drop);
            if (paths.empty()) return 0;
            SetForegroundWindow(wnd);
            Item* it = Sel();
            if (page_ == PAGE_RINGS && tab_ == 2 && it && IsImageFile(paths[0]) &&
                iconTile_.Contains((REAL)pt.x, (REAL)pt.y)) {
                Change();
                it->icon = ShortPath(paths[0]);
                Resolve(*it);
                Log(L"Значок заменён");
                Save();
            } else {
                bool asChildren = page_ == PAGE_RINGS && tab_ == 1 && it;
                page_ = PAGE_RINGS;
                AddPaths(paths, asChildren);
            }
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {(LONG)(1000 * k), (LONG)(700 * k)};
            return 0;
        }
        case WM_DPICHANGED: {
            k = HIWORD(wp) / 96.0;
            RECT* r = (RECT*)lp;
            SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        case WM_SYSKEYDOWN:
            if (CaptureKey(wp)) return 0;
            // Alt+стрелки двигают сектор
            if (page_ == PAGE_RINGS && ov_ == OV_NONE && (wp == VK_UP || wp == VK_DOWN)) {
                SectorOp(wp == VK_UP ? OP_UP : OP_DOWN);
                InvalidateRect(wnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_KEYDOWN: {
            if (CaptureKey(wp)) return 0;
            bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
            if (wp == 'Z' && ctrl) {
                Undo();
                InvalidateRect(wnd, nullptr, FALSE);
                return 0;
            }
            if (wp == 'Y' && ctrl) {
                Redo();
                InvalidateRect(wnd, nullptr, FALSE);
                return 0;
            }
            if (page_ == PAGE_RINGS && ov_ == OV_NONE) {
                int n = (int)Ring(ring_).size();
                bool handled = true;
                if (wp == VK_UP && sel_ > 0)
                    --sel_;
                else if (wp == VK_DOWN && sel_ + 1 < n)
                    ++sel_;
                else if (wp == VK_DELETE)
                    SectorOp(OP_DELETE);
                else if (wp == VK_F2)
                    SectorOp(OP_RENAME);
                else if (wp == 'D' && ctrl)
                    SectorOp(OP_DUPLICATE);
                else if (wp == VK_TAB && ctrl)
                    ring_ = 1 - ring_, sel_ = 0;
                else
                    handled = false;
                if (handled) {
                    InvalidateRect(wnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (wp == VK_ESCAPE && ov_ != OV_NONE) {
                if (ov_ == OV_TOUR && !cfg.tourDone) {
                    cfg.tourDone = true;
                    Save();
                }
                ov_ = OV_NONE;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_CLOSE:
            if (savePending_) Save();
            DestroyWindow(wnd);
            return 0;
        case WM_DESTROY:
            if (memDC_) {
                SelectObject(memDC_, oldBmp_);
                DeleteObject(dib_);
                DeleteDC(memDC_);
            }
            g_st = nullptr;
            delete this;
            mem::TrimSoon(1500);  // студия закрыта: её страницы и картинки больше не нужны
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

LRESULT CALLBACK StudioProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_st && g_st->wnd == h) return g_st->Proc(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace studio

using namespace studio;

int EditorSnapshot(HINSTANCE inst, const std::wstring& cfgPath, int page, int tab, const std::wstring& out, int ms) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = StudioProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_st = new Studio;
    g_st->inst = inst;
    g_st->cfgPath = cfgPath;
    return g_st->Snapshot(page, tab, out, ms);
}

void OpenEditor(HINSTANCE inst, const std::wstring& cfgPath, std::function<void()> onSaved,
                std::function<void(bool, HWND)> showMenu) {
    if (g_st) {
        ShowWindow(g_st->wnd, SW_RESTORE);
        SetForegroundWindow(g_st->wnd);
        return;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = StudioProc;
        wc.style = CS_DBLCLKS;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        reg = true;
    }
    g_st = new Studio;
    g_st->inst = inst;
    g_st->cfgPath = cfgPath;
    g_st->onSaved = std::move(onSaved);
    g_st->showMenu = std::move(showMenu);
    g_st->Open();
}

namespace studio {
std::function<void(int)> g_preview;
void Preview(int what) {
    if (g_preview) g_preview(what);
}
}  // namespace studio

void EditorGoNet() {
    if (g_st) g_st->GoPage(PAGE_NET);
}

void EditorGoTools(int tab, const std::wstring& scan) {
    if (g_st) g_st->GoTools(tab, scan);
}

void SetEditorPreview(std::function<void(int)> f) { studio::g_preview = std::move(f); }

namespace studio {
std::function<std::wstring()> g_gameStatus;
std::wstring GameStatus() { return g_gameStatus ? g_gameStatus() : L""; }
std::function<std::wstring()> g_devStatus;
std::wstring DevStatus() { return g_devStatus ? g_devStatus() : L""; }
}  // namespace studio

void SetEditorGameStatus(std::function<std::wstring()> f) { studio::g_gameStatus = std::move(f); }
void SetEditorDevStatus(std::function<std::wstring()> f) { studio::g_devStatus = std::move(f); }

bool EditorPreTranslate(MSG* m) {
    if (!g_st || !g_st->wnd) return false;
    if (m->message != WM_KEYDOWN) return false;
    HWND root = GetAncestor(m->hwnd, GA_ROOT);
    if (root != g_st->wnd) return false;
    bool inEdit = m->hwnd != g_st->wnd;
    if (inEdit && (m->wParam == VK_RETURN || m->wParam == VK_ESCAPE)) {
        SetFocus(g_st->wnd);  // Enter или Esc в поле - закончить ввод
        return true;
    }
    return false;
}
