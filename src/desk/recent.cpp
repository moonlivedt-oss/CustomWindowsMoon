#include "desk/recent.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <set>

#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/text.h"
#include "menu/menu.h"

namespace recent {

namespace {

const Config* g_cfg = nullptr;

std::wstring RecentDir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\Microsoft\\Windows\\Recent", p, MAX_PATH);
    return p;
}

// цель ярлыка без Resolve: только прочитать путь, сеть и диски не трогаем
std::wstring Target(IShellLinkW* link, const std::wstring& lnk) {
    IPersistFile* pf = nullptr;
    std::wstring out;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) {
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
            wchar_t buf[MAX_PATH * 2] = {};
            if (SUCCEEDED(link->GetPath(buf, MAX_PATH * 2, nullptr, SLGP_RAWPATH))) {
                wchar_t full[MAX_PATH * 2];
                DWORD n = ExpandEnvironmentStringsW(buf, full, MAX_PATH * 2);
                out = n && n < MAX_PATH * 2 ? full : buf;
            }
        }
        pf->Release();
    }
    return out;
}

std::wstring Leaf(const std::wstring& p) {
    std::wstring s = p;
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    size_t at = s.find_last_of(L'\\');
    return at == std::wstring::npos ? s : s.substr(at + 1);
}

std::wstring Ago(unsigned long long ft) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    unsigned long long n = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    long long min = n > ft ? (long long)((n - ft) / 600000000ull) : 0;
    if (min < 1) return L"только что";
    if (min < 60) return std::to_wstring(min) + L" мин назад";
    if (min < 24 * 60) return std::to_wstring(min / 60) + L" ч назад";
    return Plural((int)(min / (24 * 60)), L"день", L"дня", L"дней") + L" назад";
}

// Прочитать заново (до секунды при сотнях ярлыков) - только в фоне.
std::vector<Entry> Read(size_t max) {
    struct Lnk {
        std::wstring file;
        unsigned long long when;
    };
    std::vector<Lnk> lnks;
    std::wstring dir = RecentDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            lnks.push_back({dir + L"\\" + fd.cFileName, ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                                                            fd.ftLastWriteTime.dwLowDateTime});
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(lnks.begin(), lnks.end(), [](const Lnk& a, const Lnk& b) { return a.when > b.when; });
    std::vector<Entry> out;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        std::set<std::wstring> seen;
        for (size_t i = 0; i < lnks.size() && i < max * 3 && out.size() < max; ++i) {
            std::wstring t = Target(link, lnks[i].file);
            if (t.size() < 3 || t[1] != L':' || !seen.insert(Lower(t)).second) continue;  // только пути на дисках
            DWORD a = GetFileAttributesW(t.c_str());
            if (a == INVALID_FILE_ATTRIBUTES) continue;
            out.push_back({Leaf(t), t, (a & FILE_ATTRIBUTE_DIRECTORY) != 0, lnks[i].when});
        }
        link->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Entry> g_cache;  // под g_lock
std::atomic<ULONGLONG> g_at{0};
std::atomic<bool> g_busy{false};

void Refresh() {
    if (g_busy.exchange(true)) return;
    if (!bg::Run(
            [] {
                std::vector<Entry> v = Read(40);
                AcquireSRWLockExclusive(&g_lock);
                g_cache = std::move(v);
                ReleaseSRWLockExclusive(&g_lock);
                g_at = GetTickCount64();
                g_busy = false;
            },
            THREAD_PRIORITY_BELOW_NORMAL))
        g_busy = false;
}

}  // namespace

std::vector<Entry> List(size_t max) {
    // из кэша сразу; старше 30 с - освежить в фоне (кольцо откроется с тем, что было, следующее - со свежим)
    if (GetTickCount64() - g_at > 30000) Refresh();
    AcquireSRWLockShared(&g_lock);
    std::vector<Entry> v = g_cache;
    ReleaseSRWLockShared(&g_lock);
    if (v.size() > max) v.resize(max);
    return v;
}

void Open(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

Item ItemOf(const Entry& e, const std::wstring& key) {
    Item it;
    it.special = 3;
    it.name = e.name;
    it.path = key;
    it.full = e.path;  // значок - из оболочки, как у файла в Проводнике
    it.dir = e.dir;
    it.note = Ago(e.when) + L". " + e.path;
    return it;
}

std::vector<Item> Items(const std::wstring& tag) {
    std::vector<Item> out;
    for (const Entry& e : List()) {
        Item it = ItemOf(e, tag + e.path);
        it.name = L"Недавнее: " + it.name;
        out.push_back(std::move(it));
    }
    return out;
}

void ShowRing(RadialMenu& menu) {
    if (menu.IsOpen()) return;
    std::vector<Entry> all = List();
    std::vector<Item> files, dirs;
    for (const Entry& e : all) (e.dir ? dirs : files).push_back(ItemOf(e, e.path));
    std::vector<Item> ring;
    // первое кольцо: до 9 свежих файлов, «Папки», «Ещё»
    for (size_t i = 0; i < files.size() && i < 9; ++i) ring.push_back(files[i]);
    if (!dirs.empty()) {
        Item d;
        d.special = 3;
        d.name = L"Папки";
        d.note = L"Недавно открытые папки";
        d.iconFull = ExeDir() + L"\\icons\\folder.png";
        d.children = dirs;
        ring.push_back(std::move(d));
    }
    if (files.size() > 9) {
        Item more;
        more.special = 3;
        more.name = L"Ещё " + std::to_wstring(files.size() - 9);
        more.note = L"Файлы постарше";
        more.iconFull = ExeDir() + L"\\icons\\more.png";
        more.children.assign(files.begin() + 9, files.end());
        ring.push_back(std::move(more));
    }
    if (ring.empty()) {
        Item none;
        none.special = 3;
        none.name = L"Пусто";
        none.note = L"Windows не запоминает недавние файлы (Параметры - Персонализация - Пуск)";
        none.iconFull = ExeDir() + L"\\icons\\files.png";
        ring.push_back(none);
    }
    menu.onNet = [](const std::wstring& key) {
        if (!key.empty()) Open(key);
    };
    menu.netStatus = [] { return std::wstring(L"Недавнее - из списка Windows"); };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForSystem();
    static Config fallback;
    menu.ShowNet(g_cfg ? g_cfg : &fallback, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

void Init(const Config* cfg) {
    g_cfg = cfg;
    Refresh();  // к первому открытию кольца список уже готов
}

}  // namespace recent
