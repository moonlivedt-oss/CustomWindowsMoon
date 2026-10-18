#include "app/worksets.h"

#include <shellapi.h>

#include <algorithm>
#include <map>
#include <set>

#include "app/autos.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/launch.h"
#include "core/text.h"
#include "desk/layout.h"
#include "desk/zones.h"
#include "menu/menu.h"
#include "tools/tools.h"

namespace worksets {

namespace {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
std::map<std::wstring, std::vector<HWND>> g_opened;  // имя набора -> окна, что он открыл (под g_lock)

std::set<HWND> TopWindows() {
    std::set<HWND> out;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            if (IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && GetWindowTextLengthW(w) > 0)
                ((std::set<HWND>*)lp)->insert(w);
            return TRUE;
        },
        (LPARAM)&out);
    return out;
}

void Launch(const std::wstring& path) {
    std::wstring p = ExpandPath(path);
    if (p.rfind(L"code:", 0) == 0) {
        std::wstring dir = p.substr(5);
        ShellExecuteW(nullptr, nullptr, FindVsCode(g_cfg).c_str(), (L"\"" + dir + L"\"").c_str(), nullptr,
                      SW_SHOWNORMAL);
        return;
    }
    if (p.rfind(L"term:", 0) == 0) {
        tools::OpenEnvTerminal(tools::DetectEnv(p.substr(5)));
        return;
    }
    ShellExecuteW(nullptr, nullptr, p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring Icon() {
    std::wstring f = ExeDir() + L"\\icons\\tools-workset.png";
    return Exists(f) ? f : ExeDir() + L"\\icons\\projects.png";
}

}  // namespace

std::wstring KindOf(const std::wstring& path) {
    std::wstring p = Lower(path);
    if (p.rfind(L"code:", 0) == 0) return L"VS Code с папкой";
    if (p.rfind(L"term:", 0) == 0) return L"терминал с окружением";
    if (p.rfind(L"http://", 0) == 0 || p.rfind(L"https://", 0) == 0) return L"ссылка";
    if (EndsWithI(p, L".exe") || EndsWithI(p, L".lnk")) return L"программа";
    if (IsFolder(ExpandPath(path))) return L"папка";
    return p.empty() ? L"пусто" : L"файл";
}

void Init(HWND app, const Config* cfg) {
    g_app = app;
    g_cfg = cfg;
}

void Open(int n) {
    if (!g_cfg || n < 0 || n >= (int)g_cfg->sets.size()) return;
    const WorkSet& w = g_cfg->sets[n];
    std::wstring name = w.ring.name, lay = w.layout;
    std::set<HWND> before = TopWindows();
    AllowSetForegroundWindow(ASFW_ANY);
    for (const Item& it : w.ring.children) Launch(it.path);
    autos::Note(L"Набор «" + name + L"»: открыт (" +
                Plural((int)w.ring.children.size(), L"пункт", L"пункта", L"пунктов") + L")");
    size_t want = w.ring.children.size();
    // какие окна появились - в фоне: редактор и браузер открываются секундами
    bg::Run([name, lay, before, want] {
        std::vector<HWND> fresh;
        for (int t = 0; t < 40 && !bg::Quitting(); ++t) {
            Sleep(250);
            for (HWND h : TopWindows())
                if (!before.count(h) && std::find(fresh.begin(), fresh.end(), h) == fresh.end()) {
                    wchar_t cls[64] = {};
                    GetClassNameW(h, cls, 64);
                    if (!wcsncmp(cls, L"MoonRadial", 10)) continue;
                    fresh.push_back(h);
                }
            if (fresh.size() >= want && t >= 12) break;  // всё на месте, ещё немного подождали отстающих
        }
        AcquireSRWLockExclusive(&g_lock);
        g_opened[name] = fresh;
        ReleaseSRWLockExclusive(&g_lock);
        if (lay.empty() || fresh.empty()) return;
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(fresh[0], MONITOR_DEFAULTTONEAREST), &mi);
        UINT dpi = GetDpiForWindow(fresh[0]);
        std::vector<RECT> rects = zones::Layout(lay, mi.rcWork, 8 * (int)(dpi ? dpi : 96) / 96);
        for (size_t i = 0; i < rects.size() && i < fresh.size(); ++i) layout::Place(fresh[i], rects[i]);
    });
}

void Close(int n) {
    if (!g_cfg || n < 0 || n >= (int)g_cfg->sets.size()) return;
    std::wstring name = g_cfg->sets[n].ring.name;
    AcquireSRWLockExclusive(&g_lock);
    std::vector<HWND> wins = g_opened[name];
    g_opened.erase(name);
    ReleaseSRWLockExclusive(&g_lock);
    int closed = 0;
    for (HWND h : wins)
        if (IsWindow(h) && PostMessageW(h, WM_CLOSE, 0, 0)) ++closed;
    autos::Note(L"Набор «" + name + L"»: " +
                (closed ? L"закрыто окон: " + std::to_wstring(closed) : std::wstring(L"закрывать нечего")));
}

int OpenedCount(int n) {
    if (!g_cfg || n < 0 || n >= (int)g_cfg->sets.size()) return 0;
    AcquireSRWLockShared(&g_lock);
    int k = 0;
    auto it = g_opened.find(g_cfg->sets[n].ring.name);
    if (it != g_opened.end())
        for (HWND h : it->second) k += IsWindow(h) ? 1 : 0;
    ReleaseSRWLockShared(&g_lock);
    return k;
}

void Run(const std::wstring& key) {
    if (key.rfind(L"open:", 0) == 0) Open(_wtoi(key.c_str() + 5));
    if (key.rfind(L"close:", 0) == 0) Close(_wtoi(key.c_str() + 6));
}

std::vector<Item> Items(const std::wstring& tag) {
    std::vector<Item> out;
    if (!g_cfg) return out;
    for (size_t i = 0; i < g_cfg->sets.size(); ++i) {
        const WorkSet& w = g_cfg->sets[i];
        Item o;
        o.special = 3;
        o.name = L"Набор: " + w.ring.name;
        o.path = tag + L"open:" + std::to_wstring(i);
        o.note = L"Открыть: " + Plural((int)w.ring.children.size(), L"пункт", L"пункта", L"пунктов");
        o.iconFull = Icon();
        out.push_back(o);
        if (OpenedCount((int)i)) {
            Item c = o;
            c.name = L"Закрыть набор: " + w.ring.name;
            c.path = tag + L"close:" + std::to_wstring(i);
            c.note = L"Вежливо закрыть окна, которые он открыл: " + std::to_wstring(OpenedCount((int)i));
            out.push_back(c);
        }
    }
    return out;
}

void ShowRing(RadialMenu& menu) {
    if (menu.IsOpen()) return;
    std::vector<Item> ring;
    for (size_t i = 0; i < g_cfg->sets.size(); ++i) {
        const WorkSet& w = g_cfg->sets[i];
        Item g;
        g.special = 3;
        g.name = w.ring.name;
        g.iconFull = w.ring.iconFull.empty() ? Icon() : w.ring.iconFull;
        int open = OpenedCount((int)i);
        g.note = Plural((int)w.ring.children.size(), L"пункт", L"пункта", L"пунктов") +
                 (open ? L", открыто окон: " + std::to_wstring(open) : L"");
        Item o = g;
        o.name = L"Открыть";
        o.path = L"open:" + std::to_wstring(i);
        Item c = g;
        c.name = L"Закрыть";
        c.path = L"close:" + std::to_wstring(i);
        c.note = open ? L"Вежливо закрыть окна набора" : L"Набор не открывали в этот раз";
        g.children = {o, c};
        ring.push_back(std::move(g));
    }
    if (ring.empty()) {
        Item none;
        none.special = 3;
        none.name = L"Наборов нет";
        none.note = L"Наборы собираются в студии: Кольца - Программы";
        none.iconFull = Icon();
        ring.push_back(none);
    }
    menu.onNet = [](const std::wstring& key) { Run(key); };
    menu.netStatus = [] { return std::wstring(L"Рабочие наборы"); };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForWindow(g_app);
    menu.ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

}  // namespace worksets
