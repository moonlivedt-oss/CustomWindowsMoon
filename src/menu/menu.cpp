#include "menu/menu.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>

#include "core/mem.h"
#include "core/filedlg.h"
#include "core/files.h"
#include "core/launch.h"
#include "core/perf.h"
#include "core/project.h"
#include "core/text.h"
#include "core/themes.h"
#include "core/winlist.h"
#include "desk/share.h"
#include "gfx/color.h"
#include "gfx/draw.h"
#include "gfx/sound.h"
#include "menu/menu_internal.h"

using namespace Gdiplus;
using namespace menu_detail;

namespace {
RadialMenu* g_self = nullptr;

// exe окна, которое впереди (для кольца программы); пусто - стол или не узнать
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
}  // namespace

RadialMenu::Geo RadialMenu::MakeGeo(double k) {
    Geo g;
    g.k = k;
    g.rHub = 30 * k;
    g.r1 = 44 * k;
    g.r2 = 190 * k;
    g.r3 = 200 * k;
    g.r4 = 340 * k;
    g.iconRoot = (int)(42 * k + 0.5);
    g.iconSub = (int)(34 * k + 0.5);
    g.font = (float)(12 * k);
    g.w = (int)(g.r4 + 30 * k);
    g.h = (int)(2 * (g.r4 + 92 * k));  // сверху место под карточку пункта, снизу - под подсказки
    return g;
}

bool RadialMenu::Create(HINSTANCE inst) {
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"Radial Menu",
                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    drop_ = CreateMenuDrop(this);
    RegisterDragDrop(hwnd_, drop_);
    return true;
}

void RadialMenu::EnsureSurface() {
    if (dib_ && surfW_ == geo_.w && surfH_ == geo_.h) return;
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = geo_.w;
    bi.bmiHeader.biHeight = -geo_.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    memDC_ = CreateCompatibleDC(nullptr);
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    oldBmp_ = SelectObject(memDC_, dib_);
    surfW_ = geo_.w;
    surfH_ = geo_.h;
}

bool RadialMenu::Prewarm(const Config* cfg, UINT dpi, int budgetMs) {
    PERF_SCOPE("menu.Prewarm");
    if (warm_.empty()) {
        const Config* saved = cfg_;
        cfg_ = cfg;  // IconPng смотрит в настройки (общий значок папок)
        Geo g = MakeGeo(dpi / 96.0 * cfg->scale);
        for (const auto* ring : {&cfg->left, &cfg->right}) {
            for (const Item& it : *ring) {
                warm_.push_back({IconPng(it), it.full, g.iconRoot});
                for (const Item& c : it.children) warm_.push_back({IconPng(c), c.full, g.iconSub});
                if (it.expand) {
                    auto list = ListFolder(it.full, it.files);
                    for (size_t i = 0; i < list.size() && (int)i < cfg->maxChildren; ++i)
                        warm_.push_back({IconPng(list[i]), list[i].full, g.iconSub});
                }
            }
        }
        cfg_ = saved;
        warmPos_ = 0;
    }
    ULONGLONG start = GetTickCount64();
    while (warmPos_ < warm_.size() && GetTickCount64() - start < (ULONGLONG)budgetMs) {
        const WarmJob& j = warm_[warmPos_++];
        icons_.Get(j.png, j.shell, j.px);
    }
    if (warmPos_ >= warm_.size() && textW_.empty()) TextWidth(L"Проекты Ag");  // первый замер шрифта долгий
    return warmPos_ >= warm_.size();
}

void RadialMenu::Show(const Config* cfg, POINT cursor, const RECT& mon, bool leftEdge, UINT dpi, bool demo) {
    PERF_SCOPE("menu.Show");
    demo_ = demo;
    const std::vector<Item>* ring = &cfg->RingFor(leftEdge);
    // своё кольцо программы, что впереди, - первым сектором (из студии кольцо показывается без него)
    std::wstring exe = demo ? L"" : ForegroundExe();
    if (const AppRing* app = cfg->AppFor(exe)) {
        appRing_ = *ring;
        Item own = app->ring;
        own.special = 4;
        own.path.clear();
        if (own.iconFull.empty()) own.full = exe;  // значок самой программы
        appRing_.insert(appRing_.begin(), std::move(own));
        ring = &appRing_;
    }
    if (ring->empty()) return;
    cfg_ = cfg;
    ring_ = ring;
    geo_ = MakeGeo(dpi / 96.0 * cfg->scale);
    EnsureSurface();
    left_ = leftEdge;
    full_ = false;
    winMode_ = netMode_ = false;

    // центр держим так, чтобы второе кольцо и плашки помещались по высоте экрана
    int margin = (int)(geo_.r4 + 92 * geo_.k);
    int y = cursor.y;
    int top = mon.top + margin, bottom = mon.bottom - margin;
    y = top <= bottom ? std::clamp(y, top, bottom) : (mon.top + mon.bottom) / 2;
    origin_ = {left_ ? mon.left : mon.right - geo_.w, y - geo_.h / 2};
    center_ = {left_ ? 0 : geo_.w, geo_.h / 2};
    Start();
}

void RadialMenu::ShowAround(const Config* cfg, POINT cursor, const RECT& work, UINT dpi) {
    PERF_SCOPE("menu.ShowAround");
    fullRing_ = cfg->left;
    fullRing_.insert(fullRing_.end(), cfg->right.begin(), cfg->right.end());
    if (fullRing_.empty()) return;
    demo_ = false;
    cfg_ = cfg;
    ring_ = &fullRing_;
    geo_ = MakeGeo(dpi / 96.0 * cfg->scale);
    winMode_ = netMode_ = false;
    PlaceAround(cursor, work);
    Start();
}

void RadialMenu::ShowPalette(const Config* cfg, std::vector<Item> extra, POINT cursor, const RECT& work, UINT dpi) {
    PERF_SCOPE("menu.ShowPalette");
    fullRing_ = cfg->left;
    fullRing_.insert(fullRing_.end(), cfg->right.begin(), cfg->right.end());
    if (fullRing_.empty() && extra.empty()) return;
    if (fullRing_.empty()) fullRing_.push_back(extra.front());
    demo_ = false;
    cfg_ = cfg;
    ring_ = &fullRing_;
    geo_ = MakeGeo(dpi / 96.0 * cfg->scale);
    winMode_ = netMode_ = false;
    paletteExtra_ = std::move(extra);
    PlaceAround(cursor, work);
    Start();
    palette_ = true;
}

void RadialMenu::ShowWindows(const Config* cfg, POINT cursor, const RECT& work, UINT dpi) {
    PERF_SCOPE("menu.ShowWindows");
    winRing_ = winlist::Build(16);
    if (winRing_.empty()) return;
    demo_ = false;
    cfg_ = cfg;
    ring_ = &winRing_;
    geo_ = MakeGeo(dpi / 96.0 * cfg->scale);
    // середина отдана под миниатюру окна: ступица шире, кольца отодвинуты наружу
    double k = geo_.k;
    geo_.rHub = 112 * k;
    geo_.r1 = 124 * k;
    geo_.r2 = 232 * k;
    geo_.r3 = 242 * k;
    geo_.r4 = 372 * k;
    geo_.h = (int)(2 * (geo_.r4 + 72 * k));
    winMode_ = true;
    netMode_ = false;
    PlaceAround(cursor, work);
    Start();
}

void RadialMenu::ShowNet(const Config* cfg, std::vector<Item> items, POINT cursor, const RECT& work, UINT dpi) {
    PERF_SCOPE("menu.ShowNet");
    if (items.empty()) return;
    netRing_ = std::move(items);
    demo_ = false;
    cfg_ = cfg;
    ring_ = &netRing_;
    geo_ = MakeGeo(dpi / 96.0 * cfg->scale);
    winMode_ = false;
    netMode_ = true;
    PlaceAround(cursor, work);
    Start();
}

void RadialMenu::PlaceAround(POINT cursor, const RECT& work) {
    geo_.w = geo_.h;  // квадрат: кольцо целиком, сверху и снизу место под плашки
    EnsureSurface();
    left_ = true;
    full_ = true;
    // центр - у курсора, но так, чтобы кольцо со вторым ярусом не вылезло за экран
    int half = geo_.w / 2, reach = (int)(geo_.r4 + 20 * geo_.k);
    int cx =
        std::clamp((int)cursor.x, (int)work.left + reach, std::max((int)work.left + reach, (int)work.right - reach));
    int cy =
        std::clamp((int)cursor.y, (int)work.top + reach, std::max((int)work.top + reach, (int)work.bottom - reach));
    origin_ = {cx - half, cy - half};
    center_ = {half, half};
}

POINT RadialMenu::CursorPt() const {
    POINT p{};
    if (fakeCursor_)
        p = *fakeCursor_;
    else
        GetCursorPos(&p);
    return p;
}

int RadialMenu::Bench(const Config* cfg, UINT dpi, int ms) {
    Config quiet = *cfg;
    quiet.sounds = false;
    bool leftEdge = !quiet.left.empty();
    const auto& ring = quiet.RingFor(leftEdge);
    if (ring.empty()) return 0;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    demo_ = true;  // курсор не уводит меню
    cfg_ = &quiet;
    ring_ = &ring;
    geo_ = MakeGeo(dpi / 96.0 * quiet.scale);
    EnsureSurface();
    left_ = leftEdge;
    full_ = winMode_ = netMode_ = false;
    int cy = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
    origin_ = {left_ ? mi.rcMonitor.left : mi.rcMonitor.right - geo_.w, cy - geo_.h / 2};
    center_ = {left_ ? 0 : geo_.w, geo_.h / 2};
    POINT fake{origin_.x + center_.x, origin_.y + center_.y};
    fakeCursor_ = &fake;
    frames_ = 0;
    // значки оболочки догружаются в потоке: замеряем кадр, а не Проводник
    for (ULONGLONG t = GetTickCount64(); !icons_.Idle() && GetTickCount64() - t < 15000;) Sleep(20);
    Start(false);
    ULONGLONG t0 = GetTickCount64();
    for (ULONGLONG now = t0; now - t0 < (ULONGLONG)ms; now = GetTickCount64()) {
        // полсекунды курсор идёт дугой по первому кольцу, следующие полсекунды - по второму
        double ph = ((now - t0) % 1000) / 1000.0, back = ((now - t0) / 1000) % 2 ? -1 : 1;
        bool outer = ph >= 0.5;
        double t = (outer ? ph - 0.5 : ph) * 2;
        double r = outer ? (geo_.r3 + geo_.r4) / 2 : (geo_.r1 + geo_.r2) / 2;
        double rel = back * (-0.8 + 1.6 * t) * Span() * kPi / 180;
        double dir = left_ ? 1 : -1;
        fake = {origin_.x + center_.x + (LONG)(dir * r * std::cos(rel)),
                origin_.y + center_.y + (LONG)(r * std::sin(rel))};
        Tick();
        Sleep(10);
    }
    int n = frames_;
    {                                       // кадр из кэша колец должен совпасть с нарисованным заново
        hoverT_ = GetTickCount64() - 5000;  // анимация значка под курсором закончилась
        Render();
        Render();  // второй одинаковый кадр - кольца запомнены
        Render();  // этот - из кэша
        const size_t total = (size_t)geo_.w * geo_.h;
        std::vector<uint32_t> cached((uint32_t*)bits_, (uint32_t*)bits_ + total);
        bool used = ringCache_[0].ok || ringCache_[1].ok;
        for (RingCache& c : ringCache_) c = {};
        Render();
        int diff = 0;
        for (size_t i = 0; i < total; ++i) diff += cached[i] != ((uint32_t*)bits_)[i];
        benchDiff_ = used ? diff : -1;
    }
    SaveFrame(ExeDir() + L"\\bench_ring.png");  // глазами проверить, что кэш колец ничего не испортил
    Hide();
    fakeCursor_ = nullptr;
    cfg_ = cfg;
    return n;
}

void RadialMenu::SaveFrame(const std::wstring& path) {
    Bitmap frame(geo_.w, geo_.h, geo_.w * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png")) frame.Save(path.c_str(), &codecs[i].Clsid, nullptr);
}

// Кольцо-действие (утилиты, сеть) в картинку без мыши: курсор стоит на секторе root, кадры идут ms.
void RadialMenu::SnapNet(const Config* cfg, std::vector<Item> items, UINT dpi, int root, int ms,
                         const std::wstring& path) {
    Config quiet = *cfg;
    quiet.sounds = false;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    POINT mid{(mi.rcWork.left + mi.rcWork.right) / 2, (mi.rcWork.top + mi.rcWork.bottom) / 2};
    quiet.blur = false;  // стекло - снимок того, что сейчас на экране: эталон от него зависел бы
    syncGlass_ = true;
    ShowNet(&quiet, std::move(items), mid, mi.rcWork, dpi);
    syncGlass_ = false;
    demo_ = true;
    double step = 2 * Span() / std::max<size_t>(1, netRing_.size()), r = (geo_.r1 + geo_.r2) / 2;
    double a = Rad(AbsAngle(-Span() + step * (root + 0.5)));  // середина сектора, как его раскладывает кадр
    POINT fake{origin_.x + center_.x + (LONG)(std::cos(a) * r), origin_.y + center_.y + (LONG)(std::sin(a) * r)};
    fakeCursor_ = &fake;
    for (ULONGLONG t0 = GetTickCount64(); GetTickCount64() - t0 < (ULONGLONG)ms;) {
        Tick();
        Sleep(10);
    }
    Render();
    SaveFrame(path);
    Hide();
    fakeCursor_ = nullptr;
    cfg_ = cfg;
}

void RadialMenu::Film(const Config* cfg, UINT dpi, int root, const std::wstring& dir) {
    Config quiet = *cfg;
    quiet.sounds = false;
    bool leftEdge = !quiet.left.empty();
    const auto& ring = quiet.RingFor(leftEdge);
    if (ring.empty()) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    demo_ = true;
    cfg_ = &quiet;
    ring_ = &ring;
    geo_ = MakeGeo(dpi / 96.0 * quiet.scale);
    EnsureSurface();
    left_ = leftEdge;
    full_ = winMode_ = netMode_ = false;
    int cy = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
    origin_ = {left_ ? mi.rcMonitor.left : mi.rcMonitor.right - geo_.w, cy - geo_.h / 2};
    center_ = {left_ ? 0 : geo_.w, geo_.h / 2};
    POINT fake{origin_.x + center_.x + (left_ ? 1 : -1) * (LONG)(geo_.rHub * 0.5), origin_.y + center_.y};
    fakeCursor_ = &fake;
    for (ULONGLONG t = GetTickCount64(); !icons_.Idle() && GetTickCount64() - t < 15000;) Sleep(20);
    // кадры в отметки времени от начала сцены
    auto scene = [&](const wchar_t* name, std::initializer_list<int> marks, int total) {
        ULONGLONG t0 = GetTickCount64();
        int k = 0;
        auto next = marks.begin();
        for (ULONGLONG now = t0; now - t0 < (ULONGLONG)total && open_; now = GetTickCount64()) {
            Tick();
            if (next != marks.end() && now - t0 >= (ULONGLONG)*next) {
                Render();
                SaveFrame(dir + L"\\" + name + L"-" + std::to_wstring(++k) + L".png");
                ++next;
            }
            Sleep(5);
        }
    };
    Start(false);
    scene(L"open", {30, 110, 230}, 700);
    int n = (int)ring.size();
    double step = 2 * Span() / n, r = (geo_.r1 + geo_.r2) / 2;
    double rel = Rad(-Span() + step * (std::clamp(root, 0, n - 1) + 0.5));
    fake = {origin_.x + center_.x + (LONG)((left_ ? 1 : -1) * r * std::cos(rel)),
            origin_.y + center_.y + (LONG)(r * std::sin(rel))};
    scene(L"hover", {40, 160, 330}, 900);
    Launch();  // как после выбора, но ничего не открывая
    scene(L"launch", {40, 110, 200}, 400);
    Hide();
    fakeCursor_ = nullptr;
    cfg_ = cfg;
}

int RadialMenu::GlassProbe(const Config* cfg, UINT dpi, const std::wstring& dir) {
    static Config quiet;  // меню держит указатель на настройки до Hide
    quiet = *cfg;
    quiet.sounds = false;
    quiet.blur = true;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    POINT at{mi.rcMonitor.left, (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2};
    ULONGLONG t0 = GetTickCount64();
    Show(&quiet, at, mi.rcMonitor, true, dpi, true);  // demo: далёкий курсор кольцо не закрывает
    if (!open_) return -1;
    int glassMs = -1;
    for (ULONGLONG now = t0; now - t0 < 600; now = GetTickCount64()) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (glassMs < 0 && glassT_) glassMs = (int)(glassT_ - t0);
        Sleep(5);
    }
    if (!blurPx_.empty()) {
        std::vector<uint32_t> keep((uint32_t*)bits_, (uint32_t*)bits_ + (size_t)geo_.w * geo_.h);
        memcpy(bits_, blurPx_.data(), blurPx_.size() * 4);
        SaveFrame(dir + L"\\glass-backdrop.png");
        memcpy(bits_, keep.data(), keep.size() * 4);
    }
    SaveFrame(dir + L"\\glass-frame.png");
    Hide();
    cfg_ = cfg;
    return glassMs;
}

void RadialMenu::Start(bool live) {
    const auto& ring = *ring_;
    mode_ = Mode::None;
    hoverRoot_ = hoverSub_ = subParent_ = pendingRoot_ = -1;
    hlRoot_.assign(ring.size(), 0.0);
    hlSub_.clear();
    subItems_.clear();
    subOwned_.clear();
    navStack_.clear();
    rootRot_ = rootRotTo_ = subScroll_ = subScrollTo_ = 0;
    grab_ = -1;
    grabSpun_ = false;
    clickKey_.clear();
    hoverKey_.clear();
    project::ForgetInfo();
    query_.clear();
    searchPool_.clear();
    poolBuilt_ = false;
    kbSel_ = 0;
    lastHoverKey_.clear();
    dragging_ = false;
    std::fill(std::begin(swallowed_), std::end(swallowed_), false);
    shift_ = GetAsyncKeyState(VK_SHIFT) & 0x8000;
    ctrl_ = GetAsyncKeyState(VK_CONTROL) & 0x8000;
    // меню не забирает фокус, поэтому впереди всё ещё окно пользователя
    HWND fg = GetForegroundWindow();
    dialog_ = !demo_ && !winMode_ && filedlg::Is(fg) ? fg : nullptr;

    open_ = true;
    closing_ = false;
    altWasDown_ = GetAsyncKeyState(VK_MENU) & 0x8000;
    reached_ = false;
    if (altWasDown_ && live) {
        // отпускание Alt больше ничего не открывает и уйдёт окну под меню - а там включит строку меню;
        // пустое нажатие, пока Alt ещё зажат, это гасит
        INPUT in[2]{};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = 0xE8;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
    tOpen_ = tLast_ = GetTickCount64();
    subSlide_ = 0;
    flick_ = false;
    {
        POINT c = CursorPt();
        stroke_.Start(tOpen_, c.x, c.y);
    }

    // живое меню показывается сразу, стекло подтягивается снимком в фоне; без мыши (замеры, снимки) - как раньше,
    // снимком до показа окна, чтобы в него не попало само меню
    if (live && !syncGlass_)
        CaptureBackdropAsync();
    else
        CaptureBackdrop();
    EnsureHexGlass();
    Render();
    if (!live) return;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(hwnd_, kTimer, 10, nullptr);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    keyHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    if (cfg_->sounds) Sound::Play(Sound::Open);
}

void RadialMenu::Close() {
    if (!open_ || closing_) return;
    closing_ = true;
    tClose_ = GetTickCount64();
}

void RadialMenu::StopHooks() {
    if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
    if (keyHook_) UnhookWindowsHookEx(keyHook_);
    mouseHook_ = keyHook_ = nullptr;
}

double RadialMenu::CloseMs() const { return fx::Dur(launching_ ? kLaunchMs : kCloseMs); }

void RadialMenu::Launch() {
    if (fx::Game()) {
        Hide();
        return;
    }
    launching_ = true;
    launchRoot_ = hoverRoot_;
    launchSub_ = HotSub();
    closing_ = true;
    tClose_ = GetTickCount64();
    // пока гаснет, клики уходят окну под ним
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
}

void RadialMenu::Hide() {
    if (!open_) return;
    open_ = closing_ = false;
    if (launching_) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
    launching_ = false;
    KillTimer(hwnd_, kTimer);
    StopHooks();
    LeaveShellTarget();
    DropThumb();
    dragging_ = false;
    grab_ = -1;  // закрыли посреди вращения (Esc) - захват мыши больше не нужен
    grabSpun_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    ShowWindow(hwnd_, SW_HIDE);
    ++glassGen_;  // снимок, который ещё идёт, уже не нужен
    SetWindowDisplayAffinity(hwnd_, share::Affinity());  // в показе экрана - мимо захвата
    blurPx_ = {};
    maskPx_ = {};
    baseCache_ = {};
    baseCached_ = baseLast_ = baseNow_ = {};
    for (RingCache& c : ringCache_) c = {};
    searchPool_.clear();
    searchPool_.shrink_to_fit();
    palette_ = false;
    paletteExtra_ = {};
    // подписи и их раскладка остаются: первый кадр следующего открытия не собирает их заново (~70 картинок по
    // нескольку КБ); растут только при смене размера или темы - тогда разом в чистку
    if (labels_.size() > 600 || labelFit_.size() > 1500) {
        labels_.clear();
        textW_.clear();
        labelFit_.clear();
    }
    mem::TrimSoon();  // в покое память не держим
}

void RadialMenu::Guard() {
    if (!hwnd_) return;
    if (!open_) {
        if (IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    } else if (closing_ && GetTickCount64() - tClose_ >= CloseMs() + 1000) {
        Hide();
    }
}

void RadialMenu::Tick() {
    PERF_SCOPE("menu.Tick");
    ULONGLONG now = GetTickCount64();
    double dt = (now - tLast_) / 1000.0;
    tLast_ = now;
    if (closing_ && now - tClose_ >= CloseMs()) {
        Hide();
        return;
    }

    POINT pt = CursorPt();
    Hit h = HitTest(pt);
    if (!closing_) {
        // Перетаскивание без нажатых кнопок - значит, оно оборвалось, не дойдя до Drop или DragLeave.
        if (dragging_ && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000))
            OnDragLeave();
        // Во время перетаскивания наведение задаёт сама Windows (OnDragOver), иначе два источника спорят.
        if (!dragging_) UpdateHover(h, now);
        // курсор увели далеко - меню само гаснет; но кольцо из угла открывается в стороне от курсора,
        // поэтому считаем только после того, как курсор в нём побывал
        if (h.dist <= Extent()) reached_ = true;
        if (!demo_ && reached_ && h.dist > Extent() + 170 * geo_.k) Close();

        bool alt = GetAsyncKeyState(VK_MENU) & 0x8000;
        bool mouseDown = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
        // росчерк: Alt ещё держат и сразу махнули - выбор по направлению; отпустили Alt над пунктом - открыть.
        // Над сектором со вторым кольцом (группа, папка) - кольцо просто остаётся, как без росчерка
        if (cfg_->stroke && !winMode_ && altWasDown_ && alt && !flick_ && now - tOpen_ < 600) {
            stroke_.Feed(now, pt.x, pt.y);
            flick_ = stroke_.Flick(70 * geo_.k);
        }
        if (flick_ && altWasDown_ && !alt) {
            flick_ = false;
            const Item* it = HoveredItem();
            bool leaf = it && it->special != 4 && !it->Nested() &&
                        (hoverSub_ >= 0 || (hoverRoot_ >= 0 && !(*ring_)[hoverRoot_].HasRing()));
            if (leaf && !mouseDown && !dragging_ && mode_ != Mode::Search) {
                ActivateHover();
                if (!open_) return;
            }
        }
        // кольцо окон работает как Alt+Tab: отпустил Alt над окном - оно впереди;
        // папки и программы открываются только двойным кликом или Enter
        if (winMode_ && altWasDown_ && !alt && !dragging_ && !mouseDown && (hoverSub_ >= 0 || hoverRoot_ >= 0)) {
            ActivateHover();
            if (!open_) return;
        }
        altWasDown_ = alt;
        if (grab_ >= 0) {
            if (mouseDown)
                DragGrab(pt, now);
            else
                EndGrab();  // отпускание ушло мимо окна
            if (!open_ || closing_) return;
        }

        // перетаскивание: подержал файл над папкой - зашли в неё
        if (dragging_) {
            const Item* it = HoveredItem();
            bool canDrill = it && it->dir && !it->special &&
                            (hoverSub_ >= 0 || (hoverRoot_ >= 0 && !(*ring_)[hoverRoot_].HasRing()));
            std::wstring key = canDrill ? it->full : L"";
            if (key != dwellKey_) {
                dwellKey_ = key;
                dwellSince_ = now;
            } else if (!key.empty() && now - dwellSince_ > kDwellMs) {
                Item copy = *it;
                LeaveShellTarget();
                if (hoverSub_ < 0) mode_ = Mode::None;
                Drill(copy);
                dwellKey_.clear();
            }
        }
    }

    double rate = std::min(1.0, dt * 16), moved = 0;
    auto approach = [&](std::vector<double>& v, int hot) {
        for (size_t i = 0; i < v.size(); ++i) {
            double d = (((int)i == hot) - v[i]) * rate;
            v[i] += d;
            // подсветка доходит до конца ровно, а не бесконечно малыми шагами: неподвижный сектор рисуется
            // одинаково от кадра к кадру (кэш колец), а значок без подсветки - копией 1:1, без масштаба
            if (std::fabs(((int)i == hot) - v[i]) < 0.002) v[i] = ((int)i == hot);
            moved = std::max(moved, std::fabs(d));
        }
    };
    approach(hlRoot_, hoverRoot_);
    approach(hlSub_, HotSub());
    if (grab_ < 0) {  // докрутка к ближайшему сектору и возврат резинки
        double spin = std::min(1.0, dt * 13);
        double dr = (rootRotTo_ - rootRot_) * spin, ds = (subScrollTo_ - subScroll_) * spin;
        if (std::fabs(rootRotTo_ - rootRot_) < 0.002) dr = rootRotTo_ - rootRot_;
        if (std::fabs(subScrollTo_ - subScroll_) < 0.002) ds = subScrollTo_ - subScroll_;
        rootRot_ += dr;
        subScroll_ += ds;
        if (dr != 0) UpdateSubMid();
        moved = std::max(moved, std::max(std::fabs(dr), std::fabs(ds)));
    } else if (grabSpun_) {
        moved = 1;
    }
    bool anim = closing_ || now - hoverT_ < 1100 || (glassT_ && now - glassT_ < fx::Dur(kGlassMs) + 30) ||
                now - tOpen_ < fx::Dur(kOpenMs + kStaggerMs * hlRoot_.size() + 200) ||
                (mode_ != Mode::None && now - tSub_ < fx::Dur(kSubMs + kSubWaveMs * subItems_.size()) + 30);
    static std::wstring lastHint;
    double ha = 0;
    std::wstring hint = BottomHint(ha) + L"\x1" + TopHint();
    bool icons = icons_.Arrived();  // значок оболочки догрузился в потоке - показать
    if (icons)
        for (RingCache& c : ringCache_) c.ok = false;  // в запомненном кольце его ещё нет
    if (anim || moved > 0.001 || hint != lastHint || icons) Render();
    lastHint = hint;
}

LRESULT CALLBACK RadialMenu::MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_self && g_self->open_) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN) {
            PostMessageW(g_self->hwnd_, WM_OUTSIDE_CLICK, 0, MAKELPARAM(m->pt.x, m->pt.y));
        } else if (wp == WM_MOUSEWHEEL && g_self->HitTest(m->pt).dist <= g_self->Extent() + 10) {
            PostMessageW(g_self->hwnd_, WM_MENU_WHEEL, (WPARAM)(short)HIWORD(m->mouseData), 0);
            return 1;  // колесо над меню не прокручивает окно под ним
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK RadialMenu::KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_self && g_self->open_ && !g_self->closing_ && !g_self->dragging_) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (g_self->OnRawKey(k->vkCode, k->scanCode, down)) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK RadialMenu::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_self && g_self->hwnd_ == h) return g_self->Handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT RadialMenu::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimer && open_) Tick();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MENU_KEY:
            OnKey((UINT)wp);
            return 0;
        case WM_MENU_CHAR:
            if (open_ && !closing_) OnChar((wchar_t)wp);
            return 0;
        case WM_MENU_GLASS: {
            std::unique_ptr<std::vector<uint32_t>> px((std::vector<uint32_t>*)lp);
            if ((int)wp != glassGen_) return 0;  // меню уже закрыли или открыли заново
            SetWindowDisplayAffinity(hwnd_, share::Affinity());  // в показе экрана - мимо захвата
            if (open_ && px->size() == (size_t)geo_.w * geo_.h) {
                blurPx_ = std::move(*px);
                glassT_ = GetTickCount64();
                for (RingCache& c : ringCache_) c = {};
            }
            return 0;
        }
        case WM_MENU_WHEEL:
            if (open_ && !closing_) OnWheel((int)(short)wp);
            return 0;
        case WM_OUTSIDE_CLICK: {
            if (!open_ || closing_) return 0;
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            if (HitTest(p).dist > Extent() + 10) Close();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!open_ || closing_ || dragging_) return 0;
            POINT p;
            GetCursorPos(&p);
            BeginGrab(p);
            return 0;
        }
        case WM_LBUTTONUP:
            if (open_ && !closing_ && grab_ >= 0) EndGrab();
            return 0;
        case WM_RBUTTONUP:
            if (open_ && !closing_) OnRightClick();
            return 0;
        case WM_MBUTTONUP:
            if (open_ && !closing_ && winMode_) CloseHoveredWindow();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
