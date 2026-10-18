#include "desk/layout.h"

#include <dwmapi.h>

#include <algorithm>

#include "core/config.h"
#include "core/files.h"
#include "core/winlist.h"
#include "desk/zones.h"
#include "menu/menu.h"

namespace layout {

namespace {

const Config* g_cfg = nullptr;
HWND g_target = nullptr;

bool Ours(HWND w) {
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    return !wcsncmp(cls, L"MoonRadial", 10) || !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
           !wcscmp(cls, L"Shell_TrayWnd");
}

HWND Front() {
    HWND w = GetForegroundWindow();
    w = w ? GetAncestor(w, GA_ROOT) : nullptr;
    return w && !Ours(w) ? w : nullptr;
}

RECT WorkOf(HWND w) {
    MONITORINFO mi{sizeof(mi)};
    HMONITOR m = w ? MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!m) {
        POINT pt;
        GetCursorPos(&pt);
        m = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }
    GetMonitorInfoW(m, &mi);
    return mi.rcWork;
}

int Gap(HWND w) {
    UINT dpi = w ? GetDpiForWindow(w) : 96;
    return 8 * (int)(dpi ? dpi : 96) / 96;
}

Item Act(const std::wstring& name, const std::wstring& key, const std::wstring& note, const wchar_t* icon) {
    Item it;
    it.name = name;
    it.special = 3;
    it.path = key;
    it.note = note;
    it.iconFull = ExeDir() + L"\\icons\\" + icon;
    if (!Exists(it.iconFull)) it.iconFull = ExeDir() + L"\\icons\\desktop.png";
    return it;
}

}  // namespace

void Init(const Config* cfg) { g_cfg = cfg; }

HWND Target() { return g_target && IsWindow(g_target) ? g_target : Front(); }

bool Place(HWND w, const RECT& r) {
    if (!w || !IsWindow(w)) return false;
    if (IsZoomed(w) || IsIconic(w)) ShowWindow(w, SW_RESTORE);
    RECT wr{}, fr{};
    GetWindowRect(w, &wr);
    if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)))) fr = wr;
    // невидимые поля окна (тень Windows 10/11 - около 7 px слева, справа и снизу)
    LONG dl = fr.left - wr.left, dt = fr.top - wr.top, dr = wr.right - fr.right, db = wr.bottom - fr.bottom;
    return SetWindowPos(w, nullptr, r.left - dl, r.top - dt, (r.right - r.left) + dl + dr, (r.bottom - r.top) + dt + db,
                        SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

void Run(const std::wstring& key) {
    HWND w = Target();
    g_target = nullptr;
    RECT work = WorkOf(w);
    int gap = Gap(w);
    RECT r;
    if (zones::Rect(key, work, gap, r)) {
        Place(w, r);
        return;
    }
    std::vector<RECT> rects = zones::Layout(key, work, gap);
    if (rects.empty()) return;
    // последние окна этого монитора, начиная с того, что было впереди
    HMONITOR mon = MonitorFromWindow(w ? w : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    std::vector<HWND> wins;
    if (w) wins.push_back(w);
    for (const Item& app : winlist::Build(24)) {
        auto take = [&](HWND h) {
            if (!h || Ours(h) || std::find(wins.begin(), wins.end(), h) != wins.end()) return;
            if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != mon) return;
            wins.push_back(h);
        };
        if (app.children.empty())
            take(app.win);
        else
            for (const Item& c : app.children) take(c.win);
    }
    for (size_t i = 0; i < rects.size() && i < wins.size(); ++i) Place(wins[i], rects[i]);
    if (!wins.empty()) {
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(wins[0]);
    }
}

std::vector<Item> Items(const std::wstring& tag) {
    std::vector<Item> out;
    for (const zones::Zone& z : zones::OneWindow())
        out.push_back(Act(std::wstring(L"Окно: ") + z.name, tag + z.key, z.note, L"tools-layout.png"));
    for (const zones::Zone& z : zones::Layouts())
        out.push_back(Act(std::wstring(L"Раскладка: ") + z.name, tag + z.key, z.note, L"tools-layout.png"));
    return out;
}

void ShowRing(RadialMenu& menu) {
    if (menu.IsOpen()) return;
    g_target = Front();
    std::vector<Item> ring;
    const auto& one = zones::OneWindow();
    // первое кольцо - самое частое; треть и четверти - группами во втором
    for (int i = 0; i < 3; ++i) ring.push_back(Act(one[i].name, one[i].key, one[i].note, L"tools-layout.png"));
    Item thirds = Act(L"Трети", L"", L"Треть слева, в центре, справа, две трети", L"tools-layout.png");
    for (int i = 3; i < 7; ++i)
        thirds.children.push_back(Act(one[i].name, one[i].key, one[i].note, L"tools-layout.png"));
    ring.push_back(thirds);
    Item quarters = Act(L"Четверти", L"", L"Четверти и половины сверху и снизу", L"tools-layout.png");
    for (size_t i = 7; i < one.size(); ++i)
        quarters.children.push_back(Act(one[i].name, one[i].key, one[i].note, L"tools-layout.png"));
    ring.push_back(quarters);
    Item lays =
        Act(L"Несколько окон", L"", L"Последние окна монитора: рядом, колонками, четвертями", L"tools-layout.png");
    for (const zones::Zone& z : zones::Layouts())
        lays.children.push_back(Act(z.name, z.key, z.note, L"tools-layout.png"));
    ring.push_back(lays);
    menu.onNet = [](const std::wstring& key) { Run(key); };
    wchar_t title[120] = {};
    if (g_target) GetWindowTextW(g_target, title, 120);
    std::wstring status = g_target ? L"Окно: " + std::wstring(title) : L"Нет окна впереди - только раскладки";
    menu.netStatus = [status] { return status; };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForSystem();
    menu.ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

}  // namespace layout
