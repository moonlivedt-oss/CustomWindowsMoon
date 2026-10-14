#include "core/winlist.h"

#include <dwmapi.h>

#include <map>
#include <string>

#include "core/text.h"

namespace winlist {
namespace {

bool Eligible(HWND h) {
    if (!IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    bool app = ex & WS_EX_APPWINDOW;
    if (!app && ((ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_NOACTIVATE) || GetWindow(h, GW_OWNER))) return false;
    DWORD cloaked = 0;  // окно с другого рабочего стола или спрятанное приложение из магазина
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid != GetCurrentProcessId();
}

// Процесс, чьё это окно на самом деле: у приложений из магазина рамку рисует ApplicationFrameHost.
DWORD OwnerPid(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"ApplicationFrameWindow")) {
        for (HWND c = FindWindowExW(h, nullptr, nullptr, nullptr); c; c = FindWindowExW(h, c, nullptr, nullptr)) {
            DWORD cp = 0;
            GetWindowThreadProcessId(c, &cp);
            if (cp != pid) return cp;
        }
    }
    return pid;
}

std::wstring ExePath(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out = QueryFullProcessImageNameW(p, 0, buf, &n) ? std::wstring(buf, n) : L"";
    CloseHandle(p);
    return out;
}

// «Описание файла» из ресурса версии (Microsoft Edge, Telegram Desktop), иначе имя exe без расширения.
std::wstring AppName(const std::wstring& exe) {
    static std::map<std::wstring, std::wstring> cache;
    auto it = cache.find(exe);
    if (it != cache.end()) return it->second;
    std::wstring name;
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (size) {
        std::wstring data(size / 2 + 1, L'\0');
        if (GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) {
            struct Lang {
                WORD lang, cp;
            }* tr = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(Lang)) {
                wchar_t key[64];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
                wchar_t* v = nullptr;
                if (VerQueryValueW(data.data(), key, (void**)&v, &len) && v && len > 1) name = v;
            }
        }
    }
    while (!name.empty() && name.back() == L' ') name.pop_back();
    if (name.empty()) {
        size_t s = exe.find_last_of(L"\\/");
        name = s == std::wstring::npos ? exe : exe.substr(s + 1);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    }
    cache[exe] = name;
    return name;
}

}  // namespace

std::wstring Title(HWND w) {
    wchar_t buf[512] = {};
    GetWindowTextW(w, buf, 512);
    return buf;
}

std::vector<Item> Build(int maxRoots) {
    std::vector<HWND> wins;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            if (Eligible(h)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
            return TRUE;
        },
        (LPARAM)&wins);

    std::vector<Item> roots;
    std::map<std::wstring, size_t> byExe;
    for (HWND h : wins) {
        std::wstring exe = ExePath(OwnerPid(h));
        std::wstring key = exe.empty() ? L"#" + std::to_wstring((uintptr_t)h) : Lower(exe);
        Item w;
        w.name = Title(h);
        w.full = exe;
        w.special = 2;
        w.win = h;
        auto found = byExe.find(key);
        if (found == byExe.end()) {
            if ((int)roots.size() >= maxRoots) continue;
            byExe[key] = roots.size();
            Item r = w;
            r.name = exe.empty() ? w.name : AppName(exe);
            roots.push_back(std::move(r));
        } else {
            Item& r = roots[found->second];
            if (r.children.empty()) {  // второе окно: первое тоже уходит во второе кольцо
                Item first = r;
                first.name = Title(r.win);
                r.children.push_back(std::move(first));
            }
            r.children.push_back(std::move(w));
        }
    }
    return roots;
}

void Activate(HWND w) {
    if (!IsWindow(w)) return;
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    if (!SetForegroundWindow(w)) SwitchToThisWindow(w, TRUE);
}

void Close(HWND w) {
    if (IsWindow(w)) PostMessageW(w, WM_SYSCOMMAND, SC_CLOSE, 0);
}

}  // namespace winlist
