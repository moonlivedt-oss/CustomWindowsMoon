#include "desk/cliphist.h"

#include <shlwapi.h>
#include <wincrypt.h>
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
constexpr size_t kMaxPins = 20;
constexpr size_t kMaxPinDib = 8u << 20;  // закреплённая картинка на диск - не больше 8 МБ

struct Entry {
    std::wstring text;
    std::vector<BYTE> dib;  // картинка как была в буфере
    std::wstring thumb;     // превью PNG во временной папке (значок пункта)
    int w = 0, h = 0;
    SYSTEMTIME at{};
    bool pinned = false;  // закреплённое не вытесняется и хранится на диске (DPAPI), пока не открепят
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
    // то же самое уже есть - поднимаем наверх, а не дублируем (закрепление остаётся)
    for (auto it = g_list.begin(); it != g_list.end(); ++it)
        if ((!e.text.empty() && it->text == e.text) || (!e.dib.empty() && it->dib == e.dib)) {
            e.pinned = e.pinned || it->pinned;
            if (!it->thumb.empty() && it->thumb != e.thumb) {
                if (e.thumb.empty())
                    e.thumb = it->thumb;
                else
                    DeleteFileW(it->thumb.c_str());
            }
            g_list.erase(it);
            break;
        }
    g_list.push_front(std::move(e));
    // вытесняются только незакреплённые, самые старые
    size_t loose = 0;
    for (const Entry& x : g_list) loose += !x.pinned;
    for (auto it = g_list.end(); loose > kMax && it != g_list.begin();) {
        --it;
        if (it->pinned) continue;
        if (!it->thumb.empty()) DeleteFileW(it->thumb.c_str());
        it = g_list.erase(it);
        --loose;
    }
}

std::wstring PinFile() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", p, MAX_PATH);
    CreateDirectoryW(p, nullptr);
    return std::wstring(p) + L"\\clip-pins.bin";
}

// Закреплённое - на диск, зашифровано DPAPI (расшифровать может только этот пользователь на этом ПК).
// Запись: 1 байт вида (0 - текст UTF-16, 1 - DIB), 4 байта длины, данные, SYSTEMTIME.
void SavePins() {
    std::string raw;
    auto put = [&](const void* d, size_t n) { raw.append((const char*)d, n); };
    for (const Entry& e : g_list) {
        if (!e.pinned) continue;
        BYTE kind = e.dib.empty() ? 0 : 1;
        if (kind && e.dib.size() > kMaxPinDib) continue;
        DWORD n = kind ? (DWORD)e.dib.size() : (DWORD)(e.text.size() * sizeof(wchar_t));
        put(&kind, 1);
        put(&n, 4);
        put(kind ? (const void*)e.dib.data() : (const void*)e.text.data(), n);
        put(&e.at, sizeof(e.at));
    }
    std::wstring file = PinFile();
    if (raw.empty()) {
        DeleteFileW(file.c_str());
        return;
    }
    DATA_BLOB in{(DWORD)raw.size(), (BYTE*)raw.data()}, out{};
    if (!CryptProtectData(&in, L"MoonRadial clip pins", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return;
    std::wstring tmp = file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        BOOL ok = WriteFile(f, out.pbData, out.cbData, &wrote, nullptr) && wrote == out.cbData;
        CloseHandle(f);
        if (ok) MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    LocalFree(out.pbData);
}

void MakeThumb(Entry& e);

void LoadPins() {
    HANDLE f = CreateFileW(PinFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string raw;
    char buf[65536];
    DWORD got = 0;
    while (raw.size() < (kMaxPins + 1) * kMaxPinDib && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got)
        raw.append(buf, got);
    CloseHandle(f);
    DATA_BLOB in{(DWORD)raw.size(), (BYTE*)raw.data()}, out{};
    if (raw.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return;
    const BYTE* d = out.pbData;
    size_t at = 0, size = out.cbData;
    while (at + 5 <= size && g_list.size() < kMaxPins) {
        BYTE kind = d[at];
        DWORD n = 0;
        memcpy(&n, d + at + 1, 4);
        at += 5;
        if (n > size - at || sizeof(SYSTEMTIME) > size - at - n) break;
        Entry e;
        e.pinned = true;
        if (kind == 1)
            e.dib.assign(d + at, d + at + n);
        else
            e.text.assign((const wchar_t*)(d + at), n / sizeof(wchar_t));
        at += n;
        memcpy(&e.at, d + at, sizeof(SYSTEMTIME));
        at += sizeof(SYSTEMTIME);
        if (!e.dib.empty()) MakeThumb(e);
        g_list.push_back(std::move(e));
    }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
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
    MakeThumb(e);
    Remember(std::move(e));
}

void MakeThumb(Entry& e) {
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
    if (g_list.empty()) LoadPins();
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
    // закреплённое остаётся: его снимают по одному («Открепить»)
    for (auto it = g_list.begin(); it != g_list.end();) {
        if (it->pinned) {
            ++it;
            continue;
        }
        if (!it->thumb.empty()) DeleteFileW(it->thumb.c_str());
        it = g_list.erase(it);
    }
}

void TogglePin(int n) {
    if (n < 0 || n >= (int)g_list.size()) return;
    Entry& e = g_list[n];
    if (!e.pinned) {
        size_t pins = 0;
        for (const Entry& x : g_list) pins += x.pinned;
        if (pins >= kMaxPins) return;
    }
    e.pinned = !e.pinned;
    SavePins();
}

std::vector<Item> Items(const std::wstring& tag) {
    std::vector<Item> out;
    // закреплённое - первым, номер в ключе - место в общем списке
    std::vector<size_t> order;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < g_list.size(); ++i)
            if (g_list[i].pinned == (pass == 0)) order.push_back(i);
    for (size_t i : order) {
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
        if (e.pinned) it.note = L"Закреплено. " + it.note;
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
    // первое кольцо - 12 секторов вместе с «Закрепить»: 10 свежих и «Ещё» со всем, что постарше
    if (ring.size() > 11) {
        Item more;
        more.special = 3;
        more.name = L"Ещё " + std::to_wstring(ring.size() - 10);
        more.note = L"Записи постарше";
        more.iconFull = ExeDir() + L"\\icons\\more.png";
        more.children.assign(ring.begin() + 10, ring.end());
        ring.resize(10);
        ring.push_back(std::move(more));
    }
    // сектор «Закрепить»: записи с ключом pin:номер - закрепить или открепить
    if (!g_list.empty()) {
        Item pin;
        pin.special = 3;
        pin.name = L"Закрепить";
        pin.note = L"Закреплённое не вытесняется новым и переживает перезапуск (на диске - зашифровано)";
        pin.iconFull = ExeDir() + L"\\icons\\tools-pin.png";
        if (!Exists(pin.iconFull)) pin.iconFull = ExeDir() + L"\\icons\\more.png";
        for (Item it : Items(L"pin:")) {
            int n = _wtoi(it.path.c_str() + 4);
            bool on = n >= 0 && n < (int)g_list.size() && g_list[n].pinned;
            it.name = (on ? L"Открепить: " : L"") + it.name;
            it.note = on ? L"2 клика - открепить" : L"2 клика - закрепить";
            pin.children.push_back(std::move(it));
        }
        ring.push_back(std::move(pin));
    }
    menu.onNet = [](const std::wstring& key) {
        if (key.rfind(L"pin:", 0) == 0)
            TogglePin(_wtoi(key.c_str() + 4));
        else if (!key.empty())
            Use(_wtoi(key.c_str()));
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
