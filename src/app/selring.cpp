// Кольцо выделенного: копирование выделенного с возвратом буфера, кольцо действий над текстом.
#include "app/selring.h"

#include <shellapi.h>

#include <memory>
#include <vector>

#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/launch.h"
#include "core/text.h"
#include "desk/cliphist.h"
#include "gfx/qr.h"
#include "menu/menu.h"
#include "tools/tools.h"

namespace selring {

namespace {

constexpr wchar_t kClass[] = L"MoonRadialSelRing";
constexpr UINT WM_SEL_READY = WM_APP + 1;  // lParam - Got*

const Config* g_cfg = nullptr;
RadialMenu* g_menu = nullptr;
HWND g_wnd = nullptr;
bool g_busy = false;

struct Got {
    std::wstring text;
    bool fromClipboard = false;  // выделения не было - текст из буфера (заменять в программе нечего)
};
std::wstring g_text;  // текст открытого кольца
bool g_fromClip = false;

// ---- буфер обмена целиком: сохранить и вернуть ----
struct Saved {
    UINT format;
    std::vector<BYTE> data;
};

bool OpenRetry() {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(15);
    }
    return false;
}

// форматы с данными в памяти (текст, картинка DIB, файлы, HTML...); ручки GDI не копируются
bool Copyable(UINT f) {
    return f != CF_BITMAP && f != CF_METAFILEPICT && f != CF_PALETTE && f != CF_ENHMETAFILE && f != CF_OWNERDISPLAY &&
           f != CF_DSPBITMAP && f != CF_DSPMETAFILEPICT && f != CF_DSPENHMETAFILE;
}

std::vector<Saved> SaveClipboard() {
    std::vector<Saved> out;
    if (!OpenRetry()) return out;
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) {
        if (!Copyable(f)) continue;
        HANDLE h = GetClipboardData(f);
        SIZE_T n = h ? GlobalSize(h) : 0;
        const void* p = n ? GlobalLock(h) : nullptr;
        if (!p) continue;
        Saved s{f, std::vector<BYTE>((const BYTE*)p, (const BYTE*)p + n)};
        GlobalUnlock(h);
        out.push_back(std::move(s));
    }
    CloseClipboard();
    return out;
}

void PutAll(const std::vector<Saved>& all) {
    if (!OpenRetry()) return;
    EmptyClipboard();
    for (const Saved& s : all) {
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, s.data.size());
        if (!g) continue;
        memcpy(GlobalLock(g), s.data.data(), s.data.size());
        GlobalUnlock(g);
        if (!SetClipboardData(s.format, g)) GlobalFree(g);
    }
    CloseClipboard();
}

void PutText(const std::wstring& t) {
    std::vector<BYTE> b((const BYTE*)t.c_str(), (const BYTE*)(t.c_str() + t.size() + 1));
    PutAll({{CF_UNICODETEXT, b}});
}

std::wstring ReadText() {
    std::wstring t;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenRetry()) return t;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            t.assign(p, wcsnlen(p, GlobalSize(h) / sizeof(wchar_t)));
            GlobalUnlock(h);
        }
    CloseClipboard();
    return t;
}

std::wstring TextOf(const std::vector<Saved>& all) {
    for (const Saved& s : all)
        if (s.format == CF_UNICODETEXT && s.data.size() >= 2)
            return std::wstring((const wchar_t*)s.data.data(),
                                wcsnlen((const wchar_t*)s.data.data(), s.data.size() / 2));
    return L"";
}

void Keys(WORD key) {  // Ctrl + key
    INPUT in[4]{};
    for (INPUT& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = key;
    in[2].ki.wVk = key;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// пока держат сочетание (Ctrl+Alt+Пробел), Ctrl+C превратился бы в Ctrl+Alt+C
void WaitKeysUp() {
    for (int i = 0; i < 100; ++i) {
        bool down = false;
        for (int vk : {VK_MENU, VK_CONTROL, VK_SHIFT, VK_LWIN, VK_RWIN, VK_SPACE})
            down = down || (GetAsyncKeyState(vk) & 0x8000);
        if (!down) return;
        Sleep(15);
    }
}

// В терминале Ctrl+C - не «копировать», а «прервать» запущенную команду: там берём только буфер
bool IsTerminal(HWND w) {
    wchar_t cls[64] = {};
    if (!w || !GetClassNameW(w, cls, 64)) return false;
    return !wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") ||
           !wcscmp(cls, L"mintty") || !wcscmp(cls, L"PuTTY") || !wcscmp(cls, L"VirtualConsoleClass");
}

// в фоне: Ctrl+C, дождаться нового содержимого, вернуть буфер как был
void Capture(bool copy) {
    WaitKeysUp();
    cliphist::Pause(true);
    std::vector<Saved> was = SaveClipboard();
    DWORD seq = GetClipboardSequenceNumber();
    if (copy) Keys('C');
    auto* got = new Got;
    for (int i = 0; copy && i < 40 && GetClipboardSequenceNumber() == seq; ++i) Sleep(15);
    if (copy && GetClipboardSequenceNumber() != seq) {
        Sleep(30);  // программа могла положить несколько форматов по очереди
        got->text = ReadText();
        PutAll(was);
    }
    cliphist::Pause(false);
    if (got->text.empty()) {
        got->text = TextOf(was);
        got->fromClipboard = true;
    }
    if (!PostMessageW(g_wnd, WM_SEL_READY, 0, (LPARAM)got)) delete got;
}

// заменить выделенное в программе: текст в буфер, Ctrl+V, буфер как был
void Replace(const std::wstring& t) {
    bg::Run([t] {
        Sleep(160);  // кольцо гаснет, фокус у программы
        cliphist::Pause(true);
        std::vector<Saved> was = SaveClipboard();
        PutText(t);
        Keys('V');
        Sleep(350);  // вставка читает буфер не сразу
        PutAll(was);
        cliphist::Pause(false);
    });
}

void Open(const std::wstring& what, const std::wstring& args = L"") {
    ShellExecuteW(nullptr, L"open", what.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
}

std::wstring Short(const std::wstring& s, size_t n) {
    std::wstring t = Squeeze(s);
    for (wchar_t& c : t)
        if (c == L'\n') c = L' ';
    return t.size() > n ? t.substr(0, n - 1) + L"…" : t;
}

// путь к существующему файлу или папке (в кавычках или без)
std::wstring PathOf(const std::wstring& s) {
    std::wstring t = Trim(s);
    if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"') t = t.substr(1, t.size() - 2);
    if (t.size() < 3 || t.size() > MAX_PATH || t.find(L'\n') != std::wstring::npos) return L"";
    std::wstring x = ExpandPath(t);
    if (!(x.size() > 2 && x[1] == L':') && x.rfind(L"\\\\", 0) != 0) return L"";
    return GetFileAttributesW(x.c_str()) != INVALID_FILE_ATTRIBUTES ? x : L"";
}

void QrOpen(const std::wstring& text) {
    qr::Code c = qr::Encode(Narrow(text));
    if (!c.size) return;
    using namespace Gdiplus;
    const int px = 12, quiet = 4, side = (c.size + 2 * quiet) * px;
    Bitmap bmp(side, side, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.Clear(Color(255, 255, 255, 255));
    SolidBrush ink(Color(255, 0, 0, 0));
    for (int y = 0; y < c.size; ++y)
        for (int x = 0; x < c.size; ++x)
            if (c.At(x, y)) g.FillRectangle(&ink, (x + quiet) * px, (y + quiet) * px, px, px);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring file = std::wstring(tmp) + L"RadialMenu-qr.png";
    CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    if (bmp.Save(file.c_str(), &png, nullptr) == Ok) Open(file);
}

Item Act(const std::wstring& name, const wchar_t* icon, const std::wstring& key, const std::wstring& note) {
    Item it;
    it.special = 3;
    it.name = name;
    it.path = key;
    it.note = note;
    std::wstring f = ExeDir() + L"\\icons\\" + icon;
    it.iconFull = GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES ? f : ExeDir() + L"\\icons\\more.png";
    return it;
}

void Run(const std::wstring& key) {
    const std::wstring& t = g_text;
    bool ru = HasCyrillic(t);
    if (key == L"search") {
        Open(L"https://www.google.com/search?q=" + UrlEncode(Short(t, 300)));
    } else if (key == L"translate") {
        Open(L"https://translate.google.com/?sl=auto&tl=" + std::wstring(ru ? L"en" : L"ru") + L"&op=translate&text=" +
             UrlEncode(t.substr(0, 4000)));
    } else if (key == L"url") {
        std::wstring u = tools::SafeUrl(Trim(t));
        if (!u.empty()) Open(u);
    } else if (key == L"path-open") {
        Open(PathOf(t));
    } else if (key == L"path-show") {
        Open(L"explorer.exe", L"/select,\"" + PathOf(t) + L"\"");
    } else if (key == L"path-term") {
        std::wstring p = PathOf(t);
        DWORD a = GetFileAttributesW(p.c_str());
        OpenTerminal(g_cfg, (a & FILE_ATTRIBUTE_DIRECTORY) ? p : p.substr(0, p.find_last_of(L'\\')));
    } else if (key == L"copy") {
        bg::Run([t] { PutText(t); });
    } else if (key == L"qr") {
        QrOpen(t);
    } else if (key.rfind(L"put:", 0) == 0) {  // заменить выделенное (или положить в буфер, если выделения не было)
        std::wstring r;
        double v = 0;
        if (key == L"put:upper") {
            r = t;
            CharUpperBuffW(r.data(), (DWORD)r.size());
        } else if (key == L"put:lower") {
            r = t;
            CharLowerBuffW(r.data(), (DWORD)r.size());
        } else if (key == L"put:sentence") {
            r = Sentence(t);
        } else if (key == L"put:layout") {
            r = FixLayout(t);
        } else if (key == L"put:squeeze") {
            r = Squeeze(t);
        } else if (key == L"put:calc" && Calc(t, v)) {
            r = FormatNumber(v, t.find(L',') != std::wstring::npos);
        }
        if (r.empty()) return;
        if (g_fromClip)
            bg::Run([r] { PutText(r); });
        else
            Replace(r);
    }
}

std::vector<Item> RingFor(const std::wstring& t) {
    std::vector<Item> ring;
    std::wstring path = PathOf(t), url = tools::SafeUrl(Trim(t));
    std::wstring put = g_fromClip ? L"В буфер: " : L"";
    if (!url.empty()) {
        ring.push_back(Act(L"Открыть ссылку", L"web.png", L"url", url));
    } else if (!path.empty()) {
        ring.push_back(Act(L"Открыть", L"folder.png", L"path-open", path));
        ring.push_back(Act(L"Показать в Проводнике", L"files.png", L"path-show", path));
        ring.push_back(Act(L"Терминал здесь", L"terminal.png", L"path-term", path));
    }
    double v = 0;
    if (Calc(t, v)) {
        std::wstring res = FormatNumber(v, t.find(L',') != std::wstring::npos);
        ring.push_back(Act(L"= " + res, L"table.png", L"put:calc",
                           g_fromClip ? L"Результат - в буфер" : L"Заменить выражение результатом"));
    }
    ring.push_back(Act(L"Найти", L"search.png", L"search", L"Поиск в интернете: «" + Short(t, 60) + L"»"));
    ring.push_back(Act(L"Перевести", L"web.png", L"translate",
                       HasCyrillic(t) ? L"Переводчик: с русского на английский" : L"Переводчик: на русский"));
    Item text = Act(L"Текст", L"editor.png", L"", L"Раскладка, регистр, пробелы");
    std::wstring fixed = FixLayout(t);
    text.children.push_back(Act(put + L"Раскладка: " + Short(fixed, 18), L"keys.png", L"put:layout",
                                L"Набрано не в той раскладке: «" + Short(t, 40) + L"» -> «" + Short(fixed, 40) + L"»"));
    text.children.push_back(Act(put + L"ВЕРХНИЙ", L"editor.png", L"put:upper", L"Все буквы заглавные"));
    text.children.push_back(Act(put + L"нижний", L"editor.png", L"put:lower", L"Все буквы строчные"));
    text.children.push_back(
        Act(put + L"Как в предложении", L"editor.png", L"put:sentence", L"Заглавная - в начале предложений"));
    if (Squeeze(t) != t)
        text.children.push_back(Act(put + L"Без лишних пробелов", L"editor.png", L"put:squeeze",
                                    L"Двойные пробелы, табы и пустые строки прочь"));
    ring.push_back(std::move(text));
    if (Narrow(t).size() <= 200)
        ring.push_back(Act(L"QR-код", L"qr.png", L"qr", L"Навести камеру телефона - текст или ссылка на телефоне"));
    if (!g_fromClip) ring.push_back(Act(L"Копировать", L"files.png", L"copy", L"Выделенное - в буфер обмена"));
    return ring;
}

void Show(const Got& got) {
    if (!g_menu || g_menu->IsOpen()) return;
    if (Trim(got.text).empty()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    g_text = got.text.substr(0, 20000);
    g_fromClip = got.fromClipboard;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dy = 96;
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (auto f = (Fn)(void*)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")) f(mon, 0, &dpi, &dy);
    g_menu->onNet = [](const std::wstring& key) { Run(key); };
    std::wstring head = (g_fromClip ? L"Из буфера: " : L"Выделено: ") + std::to_wstring(g_text.size()) + L" зн., " +
                        std::to_wstring(Words(g_text)) + L" сл.";
    g_menu->netStatus = [head] { return head; };
    g_menu->ShowNet(g_cfg, RingFor(g_text), pt, mi.rcWork, dpi);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SEL_READY) {
        std::unique_ptr<Got> got((Got*)lp);
        g_busy = false;
        Show(*got);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Start(HINSTANCE inst, const Config* cfg, RadialMenu* menu) {
    g_cfg = cfg;
    g_menu = menu;
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
}

void Trigger() {
    if (!g_wnd || g_busy || (g_menu && g_menu->IsOpen())) return;
    g_busy = true;
    bool copy = !IsTerminal(GetForegroundWindow());
    if (!bg::Run([copy] { Capture(copy); })) g_busy = false;
}

std::vector<Item> RingItems(const std::wstring& text, bool fromClipboard) {
    g_fromClip = fromClipboard;
    return RingFor(text);
}

void Stop() {
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
}

}  // namespace selring
