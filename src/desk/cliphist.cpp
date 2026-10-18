#include "desk/cliphist.h"

#include <shlwapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <deque>
#include <memory>

#include "app/autos.h"
#include "core/config.h"
#include "core/files.h"
#include "core/macro.h"
#include "core/secrets.h"
#include "core/text.h"
#include "desk/snip.h"
#include "menu/menu.h"

using namespace Gdiplus;

namespace cliphist {

namespace {

constexpr wchar_t kClass[] = L"MoonRadialClip";
constexpr UINT_PTR kWipeTimer = 1;
constexpr size_t kMax = 30;
constexpr size_t kMaxDib = 48u << 20;  // картинка больше 48 МБ (скриншот 8K) в историю не идёт

struct Entry {
    std::wstring text;
    std::vector<BYTE> dib;  // картинка как была в буфере
    std::wstring thumb;     // превью PNG во временной папке (значок пункта)
    int w = 0, h = 0;
    SYSTEMTIME at{};
};

HWND g_wnd = nullptr;
const Config* g_cfg = nullptr;
std::deque<Entry> g_list;
DWORD g_wipeSeq = 0;  // номер изменения буфера, когда туда попал секрет
DWORD g_ownSeq = 0;   // буфер положили мы сами (выбор из истории) - не запоминать заново
int g_wiped = 0, g_skipped = 0, g_thumbN = 0;

std::wstring ThumbDir() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"radial-clip";
}

void ClearThumbs() {
    std::wstring dir = ThumbDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

bool Excluded() {
    static UINT ex = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static UINT hist = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(ex)) return true;
    if (IsClipboardFormatAvailable(hist) && OpenClipboard(g_wnd)) {
        bool no = false;
        if (HANDLE h = GetClipboardData(hist))
            if (auto* p = (const DWORD*)GlobalLock(h)) {
                no = GlobalSize(h) >= sizeof(DWORD) && *p == 0;
                GlobalUnlock(h);
            }
        CloseClipboard();
        return no;
    }
    return false;
}

bool OpenRetry() {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(g_wnd)) return true;
        Sleep(15);
    }
    return false;
}

void Remember(Entry e) {
    // то же самое уже есть - поднимаем наверх, а не дублируем
    for (auto it = g_list.begin(); it != g_list.end(); ++it)
        if ((!e.text.empty() && it->text == e.text) || (!e.dib.empty() && it->dib == e.dib)) {
            if (!it->thumb.empty() && it->thumb != e.thumb) DeleteFileW(it->thumb.c_str());
            g_list.erase(it);
            break;
        }
    g_list.push_front(std::move(e));
    while (g_list.size() > kMax) {
        if (!g_list.back().thumb.empty()) DeleteFileW(g_list.back().thumb.c_str());
        g_list.pop_back();
    }
}

void OnUpdate() {
    if (!g_cfg || (!g_cfg->clipHistory && !g_cfg->clipSecrets)) return;
    DWORD seq = GetClipboardSequenceNumber();
    if (seq == g_ownSeq) return;
    if (Excluded()) return;
    bool text = IsClipboardFormatAvailable(CF_UNICODETEXT), img = IsClipboardFormatAvailable(CF_DIB);
    if (!text && !img) return;
    if (!OpenRetry()) return;
    Entry e;
    GetLocalTime(&e.at);
    if (text) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
            if (auto* p = (const wchar_t*)GlobalLock(h)) {
                size_t n = wcsnlen(p, GlobalSize(h) / sizeof(wchar_t));
                e.text.assign(p, std::min<size_t>(n, 100000));
                GlobalUnlock(h);
            }
    } else if (HANDLE h = GetClipboardData(CF_DIB)) {
        SIZE_T sz = GlobalSize(h);
        if (sz <= kMaxDib)
            if (auto* p = (const BYTE*)GlobalLock(h)) {
                e.dib.assign(p, p + sz);
                GlobalUnlock(h);
            }
    }
    CloseClipboard();

    if (!e.text.empty()) {
        std::wstring kind = g_cfg->clipSecrets ? secrets::Kind(e.text) : L"";
        if (!kind.empty()) {
            // секрет: не запоминаем, через минуту стираем, если буфер с тех пор не меняли
            ++g_skipped;
            g_wipeSeq = seq;
            SetTimer(g_wnd, kWipeTimer, (UINT)g_cfg->clipSecretSec * 1000, nullptr);
            return;
        }
        if (Trim(e.text).empty() || !g_cfg->clipHistory) return;
        Remember(std::move(e));
        return;
    }
    if (e.dib.empty() || !g_cfg->clipHistory) return;
    if (auto bmp = snipimg::FromDib(e.dib.data(), e.dib.size())) {
        e.w = (int)bmp->GetWidth();
        e.h = (int)bmp->GetHeight();
        int side = 96;
        double k = std::min(1.0, (double)side / std::max(e.w, e.h));
        Bitmap small(std::max(1, (int)(e.w * k)), std::max(1, (int)(e.h * k)), PixelFormat32bppARGB);
        {
            Graphics g(&small);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.DrawImage(bmp.get(), 0, 0, (INT)small.GetWidth(), (INT)small.GetHeight());
        }
        std::vector<uint8_t> png = snipimg::ToPng(&small);
        CreateDirectoryW(ThumbDir().c_str(), nullptr);
        e.thumb = ThumbDir() + L"\\" + std::to_wstring(++g_thumbN) + L".png";
        HANDLE f = CreateFileW(e.thumb.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(f, png.data(), (DWORD)png.size(), &put, nullptr);
            CloseHandle(f);
        } else {
            e.thumb.clear();
        }
    }
    Remember(std::move(e));
}

void Wipe() {
    KillTimer(g_wnd, kWipeTimer);
    if (!g_wipeSeq || GetClipboardSequenceNumber() != g_wipeSeq) return;  // уже скопировали другое
    g_wipeSeq = 0;
    if (!OpenRetry()) return;
    EmptyClipboard();
    CloseClipboard();
    g_ownSeq = GetClipboardSequenceNumber();
    ++g_wiped;
    autos::Note(L"Буфер: секрет стёрт через " + std::to_wstring(g_cfg->clipSecretSec) + L" с (что это было - не пишу)");
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLIPBOARDUPDATE) {
        OnUpdate();
        return 0;
    }
    if (m == WM_TIMER && wp == kWipeTimer) {
        Wipe();
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

std::wstring Hhmm(const SYSTEMTIME& t) {
    wchar_t b[16];
    swprintf(b, 16, L"%02d:%02d", t.wHour, t.wMinute);
    return b;
}

}  // namespace

void Start(HINSTANCE inst, const Config* cfg) {
    g_cfg = cfg;
    if (g_wnd) return;
    ClearThumbs();  // прошлый запуск мог упасть и не убрать за собой
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (g_wnd) AddClipboardFormatListener(g_wnd);
}

void Stop() {
    if (g_wnd) {
        RemoveClipboardFormatListener(g_wnd);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
    g_list.clear();
    ClearThumbs();
    RemoveDirectoryW(ThumbDir().c_str());
}

void Clear() {
    g_list.clear();
    ClearThumbs();
}

std::vector<Item> Items(const std::wstring& tag) {
    std::vector<Item> out;
    for (size_t i = 0; i < g_list.size(); ++i) {
        const Entry& e = g_list[i];
        Item it;
        it.special = 3;
        it.path = tag + std::to_wstring(i);
        if (!e.dib.empty()) {
            it.name = L"Картинка " + std::to_wstring(e.w) + L"x" + std::to_wstring(e.h);
            it.iconFull = e.thumb;
            it.note = L"Скопирована в " + Hhmm(e.at) + L". 2 клика - снова в буфер и вставить";
        } else {
            std::wstring one = e.text.substr(0, e.text.find_first_of(L"\r\n"));
            one = Trim(one);
            if (one.size() > 48) one = one.substr(0, 46) + L"...";
            int lines = 1 + (int)std::count(e.text.begin(), e.text.end(), L'\n');
            it.name = one;
            it.iconFull = ExeDir() + L"\\icons\\tools-clip.png";
            if (!Exists(it.iconFull)) it.iconFull = ExeDir() + L"\\icons\\files.png";
            it.note = Hhmm(e.at) + (lines > 1 ? L", " + Plural(lines, L"строка", L"строки", L"строк") : L"") +
                      L". 2 клика - снова в буфер и вставить";
        }
        out.push_back(std::move(it));
    }
    return out;
}

void Use(int n) {
    if (n < 0 || n >= (int)g_list.size() || !OpenRetry()) return;
    Entry e = g_list[n];
    EmptyClipboard();
    bool ok = false;
    if (!e.dib.empty()) {
        if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, e.dib.size())) {
            memcpy(GlobalLock(h), e.dib.data(), e.dib.size());
            GlobalUnlock(h);
            ok = SetClipboardData(CF_DIB, h) != nullptr;
            if (!ok) GlobalFree(h);
        }
    } else if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (e.text.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(h), e.text.c_str(), (e.text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    g_ownSeq = GetClipboardSequenceNumber();
    if (!ok) return;
    Remember(std::move(e));  // выбранное - снова первым
    macro::Run(L"keys:ctrl+v");
}

void ShowRing(RadialMenu& menu) {
    if (menu.IsOpen()) return;
    std::vector<Item> ring = Items(L"");
    if (ring.empty()) {
        Item none;
        none.special = 3;
        none.name = g_cfg && g_cfg->clipHistory ? L"Пусто" : L"История выключена";
        none.note = L"Скопируйте текст или картинку - они появятся здесь (секреты не запоминаются)";
        none.iconFull = ExeDir() + L"\\icons\\files.png";
        ring.push_back(none);
    }
    if (ring.size() > 12) {  // первое кольцо - 12 свежих, остальное - во втором кольце сектора «Ещё»
        Item more;
        more.special = 3;
        more.name = L"Ещё " + std::to_wstring(ring.size() - 11);
        more.note = L"Записи постарше";
        more.iconFull = ExeDir() + L"\\icons\\more.png";
        more.children.assign(ring.begin() + 11, ring.end());
        ring.resize(11);
        ring.push_back(std::move(more));
    }
    menu.onNet = [](const std::wstring& key) {
        if (!key.empty()) Use(_wtoi(key.c_str()));
    };
    menu.netStatus = [] { return L"История буфера   |   " + Status(); };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForSystem();
    menu.ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

std::wstring Status() {
    std::wstring s = L"запомнено " + std::to_wstring(g_list.size());
    if (g_skipped) s += L", секретов не запомнено " + std::to_wstring(g_skipped);
    if (g_wiped) s += L", стёрто из буфера " + std::to_wstring(g_wiped);
    return s;
}

}  // namespace cliphist
