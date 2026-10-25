// Заряд Bluetooth-наушников: раз в минуту в фоне - свойство заряда у подключённых устройств, подсказка и карточка.
#include <windows.h>
#include <devpropdef.h>
#include <memory>
#include <setupapi.h>

#include <algorithm>
#include <atomic>

#include "app/autos.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "life/btbattery.h"

namespace btbattery {

namespace {

const DEVPROPKEY kBattery = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
const DEVPROPKEY kName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
constexpr UINT WM_BT = WM_APP + 1;

const Config* g_cfg = nullptr;
void (*g_changed)() = nullptr;
HWND g_wnd = nullptr;
BoostCard g_card;
LowWatch g_low;
ULONGLONG g_at = 0;
std::atomic<bool> g_busy{false};
std::vector<std::pair<std::wstring, int>> g_levels;  // только основной поток
std::wstring g_line;

std::vector<std::pair<std::wstring, int>> Read() {
    std::vector<std::pair<std::wstring, int>> out;
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &d); ++i) {
        wchar_t id[256];
        if (!SetupDiGetDeviceInstanceIdW(h, &d, id, 256, nullptr) || (wcsncmp(id, L"BTH", 3) != 0)) continue;
        DEVPROPTYPE type = 0;
        BYTE level = 0;
        if (!SetupDiGetDevicePropertyW(h, &d, &kBattery, &type, &level, sizeof(level), nullptr, 0) ||
            type != DEVPROP_TYPE_BYTE || level > 100)
            continue;
        wchar_t name[256] = {};
        DEVPROPTYPE nt = 0;
        if (!SetupDiGetDevicePropertyW(h, &d, &kName, &nt, (BYTE*)name, sizeof(name) - 2, nullptr, 0)) continue;
        std::wstring base = BaseName(name);
        auto it = std::find_if(out.begin(), out.end(), [&](const auto& p) { return p.first == base; });
        if (it == out.end()) out.push_back({base, level});
    }
    SetupDiDestroyDeviceInfoList(h);
    return out;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_BT) {
        std::unique_ptr<std::vector<std::pair<std::wstring, int>>> v((std::vector<std::pair<std::wstring, int>>*)lp);
        g_busy = false;
        g_levels = *v;
        std::wstring line = Line(g_levels);
        if (line != g_line) {
            g_line = line;
            if (g_changed) g_changed();
        }
        for (const auto& [name, pct] : g_levels)
            if (g_low.Feed(name, pct)) {
                autos::Note(L"Заряд " + name + L": " + std::to_wstring(pct) + L"%");
                g_card.kind = L"Мало заряда наушников";
                g_card.Show(BoostCard::DONE, name + L": " + std::to_wstring(pct) + L"%",
                            L"Скоро сядут - пора на зарядку", CurrentTheme(*g_cfg), 12000, Toast::G_WARN);
            }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg, void (*changed)()) {
    g_cfg = cfg;
    g_changed = changed;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MoonRadialBtBattery";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    g_card.Create(inst);
    g_at = GetTickCount64() - 50000;  // первый замер через 10 с после запуска
}

void Tick() {
    g_card.Tick();
    ULONGLONG now = GetTickCount64();
    if (now - g_at < 60000 || g_busy) return;
    g_at = now;
    g_busy = true;
    HWND w = g_wnd;
    if (!bg::Run(
            [w] {
                auto* v = new std::vector<std::pair<std::wstring, int>>(Read());
                if (!PostMessageW(w, WM_BT, 0, (LPARAM)v)) delete v;
            },
            THREAD_PRIORITY_BELOW_NORMAL))
        g_busy = false;
}

void Shutdown() {
    g_card.Destroy();
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
}

std::wstring Status() { return g_line; }

}  // namespace btbattery

namespace btbattery {

std::wstring Diag() {
    std::wstring s = Line(Read());
    return s.empty() ? L"заряд не отдаёт ни одно подключённое Bluetooth-устройство" : s;
}

}  // namespace btbattery
