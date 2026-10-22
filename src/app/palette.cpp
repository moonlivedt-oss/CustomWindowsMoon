#include "app/palette.h"

#include <vector>

#include "app/netui.h"
#include "app/toolsui.h"
#include "core/config.h"
#include "core/files.h"
#include "core/macro.h"
#include "core/bg.h"
#include "core/text.h"
#include "core/winlist.h"
#include "tools/tools.h"
#include "menu/menu.h"
#include "studio/studio.h"

namespace palette {

namespace {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
RadialMenu* g_menu = nullptr;
std::function<void(int)> g_openPage;
std::vector<std::function<void(std::vector<Item>&)>> g_sources;
std::vector<std::pair<std::wstring, std::function<void(const std::wstring&)>>> g_runners;
std::wstring g_status;  // пусто - палитра в этом процессе не запускалась (снимок студии)

// листья кольца (у них есть ключ действия) - в палитру с приставкой, по которой Run поймёт, чьё это
void Leaves(const std::vector<Item>& ring, const std::wstring& tag, const std::wstring& where, std::vector<Item>& out) {
    for (const Item& it : ring) {
        std::wstring here = where.empty() ? it.name : where + L" / " + it.name;
        if (!it.children.empty()) Leaves(it.children, tag, here, out);
        if (it.path.empty() || it.special != 3) continue;
        Item c = it;
        c.children.clear();
        c.path = tag + it.path;
        c.note = where + (it.note.empty() ? L"" : L". " + it.note);
        out.push_back(std::move(c));
    }
}

std::wstring ForegroundExe() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring out = QueryFullProcessImageNameW(h, 0, buf, &n) ? std::wstring(buf, n) : L"";
    CloseHandle(h);
    return out;
}

void Run(const std::wstring& key) {
    if (key.rfind(L"t|", 0) == 0) return toolsui::Run(key.substr(2));
    if (key.rfind(L"n|", 0) == 0) return netui::Run(key.substr(2));
    if (key.rfind(L"p|", 0) == 0 && g_openPage) return g_openPage(_wtoi(key.c_str() + 2));
    for (auto& [prefix, run] : g_runners)
        if (key.rfind(prefix, 0) == 0) return run(key.substr(prefix.size()));
}

}  // namespace

void Init(HWND app, const Config* cfg, RadialMenu* menu, std::function<void(int)> openPage) {
    g_app = app;
    g_cfg = cfg;
    g_menu = menu;
    g_openPage = std::move(openPage);
}

void AddSource(std::function<void(std::vector<Item>&)> source) { g_sources.push_back(std::move(source)); }

void AddRunner(const std::wstring& prefix, std::function<void(const std::wstring&)> run) {
    g_runners.push_back({prefix, std::move(run)});
}

bool Apply() {
    UnregisterHotKey(g_app, kHotId);
    if (!g_cfg->palette) {
        g_status = L"выключена";
        return true;
    }
    std::vector<macro::Step> st = macro::Parse(L"keys:" + g_cfg->paletteKey);
    if (st.empty() || st.size() != 1) {
        g_status = L"не понял сочетание «" + g_cfg->paletteKey + L"»";
        return false;
    }
    UINT mods = MOD_NOREPEAT, vk = 0;
    for (WORD v : st[0].vk) {
        if (v == VK_CONTROL)
            mods |= MOD_CONTROL;
        else if (v == VK_SHIFT)
            mods |= MOD_SHIFT;
        else if (v == VK_MENU)
            mods |= MOD_ALT;
        else if (v == VK_LWIN)
            mods |= MOD_WIN;
        else
            vk = v;
    }
    if (!vk) {
        g_status = L"нужна клавиша, кроме Ctrl, Alt, Shift и Win";
        return false;
    }
    if (!RegisterHotKey(g_app, kHotId, mods, vk)) {
        g_status = macro::Describe(L"keys:" + g_cfg->paletteKey) + L" занято другой программой";
        return false;
    }
    g_status = macro::Describe(L"keys:" + g_cfg->paletteKey);
    return true;
}

std::wstring Status() { return g_status; }

void Show() {
    if (!g_menu || g_menu->IsOpen()) return;
    std::vector<Item> extra;
    // команды программы, что впереди (кольцо программы): «VS Code: Палитра команд»
    if (const AppRing* app = g_cfg->AppFor(ForegroundExe()))
        for (const Item& c : app->ring.children) {
            Item it = c;
            it.name = app->ring.name + L": " + c.name;
            if (it.iconFull.empty()) it.iconFull = ExeDir() + L"\\icons\\apps.png";
            extra.push_back(std::move(it));
        }
    std::wstring status;
    std::vector<Item> tools = toolsui::RingItems(status);
    // «Важное» - копии пунктов из групп: в палитре они были бы вторыми строками тех же действий
    if (!tools.empty() && tools[0].name == L"Важное") tools.erase(tools.begin());
    Leaves(tools, L"t|", L"Утилиты", extra);
    Leaves(netui::RingItems(), L"n|", L"Сеть", extra);
    for (auto& [page, name] : EditorPages()) {
        Item it;
        it.name = L"Студия: " + name;
        it.special = 3;
        it.path = L"p|" + std::to_wstring(page);
        it.note = L"Раздел студии";
        it.iconFull = ExeDir() + L"\\icons\\tools-studio.png";
        if (!Exists(it.iconFull)) it.iconFull = ExeDir() + L"\\icons\\app.png";
        extra.push_back(std::move(it));
    }
    for (Item& w : winlist::Build(40)) {
        if (w.children.empty()) {
            w.name = L"Окно: " + w.name;
            extra.push_back(std::move(w));
            continue;
        }
        for (Item& c : w.children) {
            c.name = w.name + L": " + c.name;
            extra.push_back(std::move(c));
        }
    }
    for (auto& src : g_sources) src(extra);
    g_menu->onNet = Run;
    // папки из таблиц MFT - по мере набора; индекс грузится заранее, пока кольцо раскрывается
    bg::Run([] {
        std::vector<tools::FoundDir> none;
        tools::MftFindDirs(L"", 1, none);
    });
    g_menu->paletteQuery = [](const std::wstring& q) {
        std::vector<Item> out;
        std::vector<tools::FoundDir> found;
        tools::MftFindDirs(q, 12, found);
        std::wstring icon = ExeDir() + L"\\icons\\tools-folder-find.png";
        if (!Exists(icon)) icon = ExeDir() + L"\\icons\\folder.png";
        for (const tools::FoundDir& f : found) {
            if (!IsFolder(f.path)) continue;  // таблица старая, папку с тех пор убрали
            Item it;
            it.name = L"Папка: " + f.path.substr(f.path.find_last_of(L'\\') + 1);
            it.path = it.full = f.path;
            it.dir = true;
            it.iconFull = icon;
            out.push_back(std::move(it));
        }
        return out;
    };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForWindow(g_app);
    g_menu->ShowPalette(g_cfg, std::move(extra), pt, mi.rcWork, dpi ? dpi : 96);
}

}  // namespace palette
