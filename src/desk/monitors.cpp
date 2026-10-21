#include "desk/monitors.h"

#include <windows.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

#include <atomic>
#include <string>
#include <vector>

#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "desk/brightness.h"
#include "menu/menu.h"

namespace monitors {

namespace {

struct Mon {
    HMONITOR h = nullptr;
    std::wstring name;
    bool ddc = false, internal = false, primary = false;
    int level = -1;  // яркость %, -1 - неизвестно
    int input = -1;  // VCP 0x60, -1 - не отвечает
};

const Config* g_cfg = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Mon> g_list;  // под g_lock
std::atomic<bool> g_busy{false};

struct Input {
    int code;
    const wchar_t* name;
};
const Input kInputs[] = {
    {0x0F, L"DisplayPort 1"}, {0x10, L"DisplayPort 2"}, {0x11, L"HDMI 1"}, {0x12, L"HDMI 2"}, {0x1B, L"USB-C"}};

// физический монитор за HMONITOR: f получает дескриптор, вернуть - удалось ли
template <class F>
bool WithPhysical(HMONITOR mon, F f) {
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || !n) return false;
    std::vector<PHYSICAL_MONITOR> pm(n);
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, pm.data())) return false;
    bool ok = f(pm[0]);
    DestroyPhysicalMonitors(n, pm.data());
    return ok;
}

std::vector<Mon> Read() {
    std::vector<HMONITOR> hs;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            ((std::vector<HMONITOR>*)lp)->push_back(m);
            return TRUE;
        },
        (LPARAM)&hs);
    SYSTEM_POWER_STATUS ps{};
    bool laptop = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
    std::vector<Mon> out;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (HMONITOR h : hs) {
        Mon m;
        m.h = h;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(h, &mi);
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        WithPhysical(h, [&](const PHYSICAL_MONITOR& p) {
            m.name = p.szPhysicalMonitorDescription;
            DWORD lo = 0, cur = 0, hi = 0;
            if (GetMonitorBrightness(p.hPhysicalMonitor, &lo, &cur, &hi) && hi > lo) {
                m.ddc = true;
                m.level = (int)((cur - lo) * 100 / (hi - lo));
            }
            DWORD now = 0, max = 0;
            if (m.ddc && GetVCPFeatureAndVCPFeatureReply(p.hPhysicalMonitor, 0x60, nullptr, &now, &max))
                m.input = (int)(now & 0xFF);
            return true;
        });
        // без DDC: экран ноутбука (главный, есть батарея) - яркость через WMI; внешний без DDC - ничего не умеем
        if (!m.ddc && laptop && m.primary) {
            m.internal = true;
            int lvl = -1;
            if (brightness::Get(h, lvl)) m.level = lvl;
        }
        if (m.name.empty() || m.name == L"Generic PnP Monitor" || m.name == L"Универсальный монитор PnP")
            m.name = m.internal ? L"Экран ноутбука" : L"Монитор " + std::to_wstring(out.size() + 1);
        out.push_back(m);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

void Refresh() {
    if (g_busy.exchange(true)) return;
    if (!bg::Run([] {
            std::vector<Mon> v = Read();
            AcquireSRWLockExclusive(&g_lock);
            g_list = std::move(v);
            ReleaseSRWLockExclusive(&g_lock);
            g_busy = false;
        }))
        g_busy = false;
}

std::vector<Mon> List() {
    AcquireSRWLockShared(&g_lock);
    std::vector<Mon> v = g_list;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

Item Act(const std::wstring& name, const std::wstring& key, const std::wstring& note, const wchar_t* icon) {
    Item it;
    it.special = 3;
    it.name = name;
    it.path = key;
    it.note = note;
    it.iconFull = ExeDir() + L"\\icons\\" + icon;
    if (!Exists(it.iconFull)) it.iconFull = ExeDir() + L"\\icons\\desktop.png";
    return it;
}

}  // namespace

void Init(const Config* cfg) {
    g_cfg = cfg;
    Refresh();
}

void Run(const wchar_t* key) {
    std::wstring k = key;
    std::vector<Mon> list = List();
    size_t c1 = k.find(L':'), c2 = c1 == std::wstring::npos ? c1 : k.find(L':', c1 + 1);
    if (c1 == std::wstring::npos) return;
    size_t idx = (size_t)_wtoi(k.c_str() + c1 + 1);
    if (idx >= list.size()) return;
    Mon m = list[idx];
    std::wstring kind = k.substr(0, c1);
    int value = c2 == std::wstring::npos ? 0 : _wtoi(k.c_str() + c2 + 1);
    bg::Run([m, kind, value] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (kind == L"b")
            brightness::Set(m.h, value);
        else if (kind == L"in")
            WithPhysical(m.h, [&](const PHYSICAL_MONITOR& p) {
                return SetVCPFeature(p.hPhysicalMonitor, 0x60, (DWORD)value) != FALSE;
            });
        else if (kind == L"off")  // VCP 0xD6 питание: 5 - выключить (включается кнопкой или сменой входа)
            WithPhysical(
                m.h, [&](const PHYSICAL_MONITOR& p) { return SetVCPFeature(p.hPhysicalMonitor, 0xD6, 5) != FALSE; });
        if (SUCCEEDED(co)) CoUninitialize();
        Refresh();  // уровни в кольце - свежие к следующему открытию
    });
}

void ShowRing(RadialMenu& menu) {
    if (menu.IsOpen()) return;
    std::vector<Mon> list = List();
    Refresh();
    std::vector<Item> ring;
    for (size_t i = 0; i < list.size(); ++i) {
        const Mon& m = list[i];
        std::wstring n = std::to_wstring(i);
        Item g = Act(m.name, L"", m.level >= 0 ? L"Яркость " + std::to_wstring(m.level) + L"%" : L"Яркость неизвестна",
                     L"desktop.png");
        if (m.ddc || m.internal)
            for (int pct : {0, 25, 50, 75, 100})
                g.children.push_back(Act(L"Яркость " + std::to_wstring(pct) + L"%",
                                         L"b:" + n + L":" + std::to_wstring(pct), L"2 клика - поставить",
                                         L"brightness.png"));
        if (m.ddc) {
            for (const Input& in : kInputs)
                g.children.push_back(Act((in.code == m.input ? L"Сейчас: " : L"Вход: ") + std::wstring(in.name),
                                         L"in:" + n + L":" + std::to_wstring(in.code),
                                         L"Переключить вход монитора (DDC/CI). Пропадёт картинка, если там ничего нет",
                                         L"desktop.png"));
            g.children.push_back(
                Act(L"Выключить", L"off:" + n, L"Монитор погаснет; включается своей кнопкой", L"screen-off.png"));
        }
        if (g.children.empty()) g.note = L"Монитор не отвечает по DDC/CI (включите DDC/CI в его меню)";
        ring.push_back(std::move(g));
    }
    if (ring.empty())
        ring.push_back(Act(L"Ищу мониторы...", L"", L"Откройте кольцо ещё раз через секунду", L"desktop.png"));
    menu.onNet = [](const std::wstring& key) {
        if (!key.empty()) Run(key.c_str());
    };
    menu.netStatus = [] { return std::wstring(L"Мониторы: яркость, вход, питание"); };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForSystem();
    static Config fallback;
    menu.ShowNet(g_cfg ? g_cfg : &fallback, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

}  // namespace monitors

namespace monitors {

std::wstring Diag() {
    std::wstring s;
    for (const Mon& m : Read())
        s += m.name + (m.ddc ? L" | DDC" : L"") + (m.internal ? L" | ноутбук (WMI)" : L"") +
             (m.primary ? L" | главный" : L"") + L" | яркость " + std::to_wstring(m.level) + L" | вход " +
             std::to_wstring(m.input) + L"\r\n";
    return s;
}

}  // namespace monitors
