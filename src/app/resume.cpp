// «Продолжить, где остановился» (Л4): снимок того, что открыто, и карточка после перерыва.
#include <windows.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <ctime>
#include <set>

#include "app/autos.h"
#include "app/resume.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/launch.h"
#include "core/procsnap.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "tools/tools.h"

namespace resume {

namespace {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
BoostCard g_card;
ULONGLONG g_saveAt = 0;     // следующий плановый снимок
ULONGLONG g_holdUntil = 0;  // после перерыва: снимок ждёт, пока не ответят на карточку (или 10 минут)
ULONGLONG g_askAt = 0;      // когда спросить (через 20 с после конца перерыва: автозапуск успевает подняться)
ULONGLONG g_lockedAt = 0;   // когда заблокировали или усыпили
std::atomic<bool> g_saving{false};

LONGLONG Unix() { return (LONGLONG)time(nullptr); }

std::wstring File() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", p, MAX_PATH);
    CreateDirectoryW(p, nullptr);
    return std::wstring(p) + L"\\lastsession.tsv";
}

std::wstring ReadText(const std::wstring& path) {
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::string s((size_t)std::min<LONGLONG>(sz.QuadPart, 8 << 20), '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return Widen(s);
}

void WriteText(const std::wstring& path, const std::wstring& text) {
    std::string s = Narrow(text);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &put, nullptr);
    CloseHandle(f);
}

std::wstring LeafOf(const std::wstring& p) {
    std::wstring s = p;
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s.substr(s.find_last_of(L'\\') + 1);
}

// окна верхнего уровня: заголовки и процессы с окнами
void Windows(std::vector<std::wstring>& titles, std::set<DWORD>& pids) {
    struct Box {
        std::vector<std::wstring>* t;
        std::set<DWORD>* p;
    } box{&titles, &pids};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            wchar_t t[512];
            int n = GetWindowTextW(w, t, 512);
            if (n <= 0) return TRUE;
            auto* b = (Box*)lp;
            b->t->push_back(std::wstring(t, n));
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            b->p->insert(pid);
            return TRUE;
        },
        (LPARAM)&box);
}

// папки, открытые в Проводнике (IShellWindows); поток - STA
std::vector<std::wstring> ExplorerFolders() {
    std::vector<std::wstring> out;
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_IShellWindows, (void**)&sw))) return out;
    long n = 0;
    sw->get_Count(&n);
    for (long i = 0; i < n && i < 40; ++i) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        IDispatch* d = nullptr;
        if (FAILED(sw->Item(v, &d)) || !d) continue;
        IWebBrowser2* wb = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_IWebBrowser2, (void**)&wb))) {
            BSTR url = nullptr;
            if (SUCCEEDED(wb->get_LocationURL(&url)) && url) {
                std::wstring p = FromFileUri(url);
                if (!p.empty() && IsFolder(p)) out.push_back(p);
                SysFreeString(url);
            }
            wb->Release();
        }
        d->Release();
    }
    sw->Release();
    return out;
}

// программа с окном, которую стоит вернуть: не система, не оболочка, не то, что уже учтено отдельно
bool Worth(const std::wstring& path) {
    std::wstring l = Lower(path);
    if (l.find(L"\\windows\\") != std::wstring::npos || l.find(L"\\windowsapps\\") != std::wstring::npos) return false;
    std::wstring n = l.substr(l.find_last_of(L'\\') + 1);
    static const wchar_t* kSkip[] = {L"explorer.exe",        L"code.exe",           L"unrealeditor.exe",
                                     L"radialmenu.exe",      L"radialtools.exe",    L"applicationframehost.exe",
                                     L"textinputhost.exe",   L"wallpaper64.exe",    L"wallpaper32.exe",
                                     L"start11_64.exe",      L"steamwebhelper.exe", L"msedgewebview2.exe",
                                     L"windowsterminal.exe", L"conhost.exe",        L"openconsole.exe",
                                     L"taskmgr.exe",         L"claude.exe",         L"nvcontainer.exe"};
    for (const wchar_t* s : kSkip)
        if (n == s) return false;
    return true;
}

std::wstring AppLabel(const std::wstring& path) {
    std::wstring n = LeafOf(path);
    if (EndsWithI(n, L".exe")) n.resize(n.size() - 4);
    return n;
}

void Persist(const std::vector<Thing>& things) {
    if (things.empty()) return;  // пустой снимок не затирает прошлый (только вошли, ничего не открыто)
    Snapshot s;
    s.when = Unix();
    s.things = things;
    WriteText(File(), Save(s));
}

void SaveAsync() {
    if (g_saving.exchange(true)) return;
    if (!bg::Run(
            [] {
                HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                Persist(Now());
                if (SUCCEEDED(co)) CoUninitialize();
                g_saving = false;
            },
            THREAD_PRIORITY_BELOW_NORMAL))
        g_saving = false;
}

ThemeInfo Theme() { return CurrentTheme(*g_cfg); }

void Ask() {
    Snapshot was;
    if (!Parse(ReadText(File()), was) || was.things.empty()) return;
    if (Unix() - was.when > 14 * 24 * 3600) return;  // двухнедельной давности - уже не «где остановился»
    bg::Run([was] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<Thing> now = Now();
        if (SUCCEEDED(co)) CoUninitialize();
        auto* miss = new std::vector<Thing>(Missing(was, now));
        auto* when = new std::wstring(When(was.when, Unix()));
        if (miss->empty() || !PostMessageW(g_app, WM_RESUME, (WPARAM)when, (LPARAM)miss)) {
            delete miss;
            delete when;
        }
    });
}

}  // namespace

std::vector<Thing> Now() {
    std::vector<Thing> out;
    std::vector<std::wstring> titles;
    std::set<DWORD> pids;
    Windows(titles, pids);
    // VS Code: папки окон, что открыты сейчас
    bool code = false;
    std::shared_ptr<const procsnap::List> ps = procsnap::Get(1000);
    for (const PROCESSENTRY32W& p : *ps) code = code || !_wcsicmp(p.szExeFile, L"Code.exe");
    if (code) {
        std::wstring json = ReadText(ExpandPath(L"%APPDATA%\\Code\\User\\globalStorage\\storage.json"));
        for (const std::wstring& f : CodeFolders(json, titles)) out.push_back({Thing::CODE, f, LeafOf(f)});
    }
    for (const std::wstring& f : ExplorerFolders()) {
        bool dup = false;
        for (const Thing& t : out) dup = dup || (t.kind == Thing::FOLDER && !_wcsicmp(t.path.c_str(), f.c_str()));
        if (!dup) out.push_back({Thing::FOLDER, f, LeafOf(f)});
    }
    // Unreal: проект из командной строки редактора; программы с окнами
    int apps = 0;
    std::set<std::wstring> seen;
    for (const PROCESSENTRY32W& p : *ps) {
        if (!pids.count(p.th32ProcessID)) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, p.th32ProcessID);
        if (!h) continue;
        wchar_t img[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        std::wstring exe = QueryFullProcessImageNameW(h, 0, img, &n) ? img : L"";
        if (!_wcsicmp(p.szExeFile, L"UnrealEditor.exe")) {
            for (const std::wstring& t : tools::CmdTokens(tools::ProcCommandLine(h)))
                if (EndsWithI(t, L".uproject") && seen.insert(Lower(t)).second) {
                    std::wstring label = LeafOf(t);
                    out.push_back({Thing::UNREAL, t, label.substr(0, label.size() - 9)});
                }
        } else if (!exe.empty() && Worth(exe) && apps < 8 && seen.insert(Lower(exe)).second) {
            out.push_back({Thing::APP, exe, AppLabel(exe)});
            ++apps;
        }
        CloseHandle(h);
    }
    return out;
}

void OpenAll(const std::vector<Thing>& things) {
    for (const Thing& t : things) {
        switch (t.kind) {
            case Thing::CODE:
                ShellExecuteW(nullptr, nullptr, FindVsCode(g_cfg).c_str(), (L"\"" + t.path + L"\"").c_str(), nullptr,
                              SW_SHOWNORMAL);
                break;
            case Thing::FOLDER:
                ShellExecuteW(nullptr, L"open", t.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            default:
                ShellExecuteW(nullptr, nullptr, t.path.c_str(), nullptr, ParentDir(t.path).c_str(), SW_SHOWNORMAL);
                break;
        }
    }
    autos::Note(L"Продолжить, где остановился: открыто " + std::to_wstring(things.size()) + L" - " + Describe(things));
}

void Init(HINSTANCE inst, HWND app, const Config* cfg) {
    g_app = app;
    g_cfg = cfg;
    g_card.Create(inst);
    ULONGLONG now = GetTickCount64();
    g_saveAt = now + 10 * 60 * 1000;
    // запуск программы - тоже конец перерыва, если прошлый снимок старше часа (включили компьютер)
    Snapshot was;
    if (Parse(ReadText(File()), was) && Unix() - was.when > 3600) {
        g_askAt = now + 20000;
        g_holdUntil = now + 10 * 60 * 1000;
    }
}

void Tick() {
    if (!g_cfg) return;
    g_card.Tick();
    ULONGLONG now = GetTickCount64();
    if (g_askAt && now >= g_askAt) {
        g_askAt = 0;
        Ask();
    }
    if (now >= g_saveAt && now >= g_holdUntil) {
        g_saveAt = now + 10 * 60 * 1000;
        LASTINPUTINFO li{sizeof(li)};
        if (GetLastInputInfo(&li) && GetTickCount() - li.dwTime < 5 * 60 * 1000) SaveAsync();  // вы за компьютером
    }
}

void OnLock() {
    g_lockedAt = GetTickCount64();
    if (GetTickCount64() >= g_holdUntil) SaveAsync();
}

void OnUnlock() {
    ULONGLONG now = GetTickCount64();
    if (g_lockedAt && now - g_lockedAt >= 60 * 60 * 1000) {  // перерыв от часа
        g_askAt = now + 20000;
        g_holdUntil = now + 10 * 60 * 1000;
    }
    g_lockedAt = 0;
}

void SaveNow() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Persist(Now());
    if (SUCCEEDED(co)) CoUninitialize();
}

void OnMessage(WPARAM wp, LPARAM lp) {
    std::unique_ptr<std::wstring> when((std::wstring*)wp);
    std::unique_ptr<std::vector<Thing>> miss((std::vector<Thing>*)lp);
    std::vector<Thing> things = *miss;
    g_card.yesLabel = L"Открыть всё";
    g_card.noLabel = L"Не надо";
    g_card.onYes = [things] {
        g_card.yesLabel.clear(), g_card.noLabel.clear();
        g_holdUntil = 0;
        OpenAll(things);
    };
    g_card.onNo = [] {
        g_card.yesLabel.clear(), g_card.noLabel.clear();
        g_holdUntil = 0;
    };
    g_card.Show(BoostCard::OFFER, L"Продолжить, где остановились?",
                L"Были открыты " + *when + L": " + Describe(things, 200) + L".", Theme(), 60000, Toast::G_RESTORE);
}

void Shutdown() { g_card.Destroy(); }

}  // namespace resume
