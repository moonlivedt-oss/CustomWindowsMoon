#include "app/rules.h"

#include <windows.h>
#include <tlhelp32.h>
#include <netlistmgr.h>

#include <algorithm>
#include <cwctype>

#include "app/autos.h"
#include "core/config.h"
#include "core/text.h"
#include "desk/corners.h"
#include "desk/volume.h"
#include "tools/guard.h"
#include "tools/tools.h"

namespace rules {

namespace {

std::wstring g_active;  // текст правила, что держится
bool g_saved = false;   // что было до первого правила - запомнено
tools::PowerMode g_power = tools::POWER_UNKNOWN;
float g_volume = -1;
bool g_themeSet = false;
ULONGLONG g_at = 0;

// Имена подключённых сетей (Network List Manager, без местоположения): раз в 30 с - список меняется редко
std::vector<std::wstring> Networks() {
    static std::vector<std::wstring> cached;
    static ULONGLONG at = 0;
    ULONGLONG now = GetTickCount64();
    if (at && now - at < 30000) return cached;
    at = now;
    cached.clear();
    INetworkListManager* nlm = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager, (void**)&nlm)))
        return cached;
    IEnumNetworks* list = nullptr;
    if (SUCCEEDED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &list)) && list) {
        INetwork* n = nullptr;
        while (list->Next(1, &n, nullptr) == S_OK && n) {
            BSTR name = nullptr;
            if (SUCCEEDED(n->GetName(&name)) && name) {
                cached.push_back(name);
                SysFreeString(name);
            }
            n->Release();
        }
        list->Release();
    }
    nlm->Release();
    return cached;
}

Facts Gather(const std::vector<Rule>& list) {
    Facts f;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) f.procs.push_back(pe.szExeFile);
        CloseHandle(snap);
    }
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        f.ac = ps.ACLineStatus != 0;
        if (ps.BatteryLifePercent <= 100) f.battery = ps.BatteryLifePercent;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    f.minutes = t.wHour * 60 + t.wMinute;
    f.weekend = t.wDayOfWeek == 0 || t.wDayOfWeek == 6;
    LASTINPUTINFO li{sizeof(li)};
    if (GetLastInputInfo(&li)) f.idleMin = (int)((GetTickCount() - li.dwTime) / 60000);
    // сети - только если о них спрашивает хоть одно правило (COM раз в 30 с незачем без нужды)
    for (const Rule& r : list)
        for (const std::wstring& w : r.when)
            if (Lower(w).find(L"net:") != std::wstring::npos) {
                f.networks = Networks();
                return f;
            }
    return f;
}

void SetVolume(float level) {
    float cur = vol::AddMaster(0);
    if (cur >= 0) vol::AddMaster(level - cur);
}

}  // namespace

void Release() {
    if (!g_saved) return;
    if (g_power != tools::POWER_UNKNOWN) tools::SetPowerMode(g_power);
    if (g_volume >= 0) SetVolume(g_volume);
    g_saved = false;
    g_active.clear();
}

void Tick(const Config& cfg, void (*onTheme)(const std::wstring&)) {
    ULONGLONG now = GetTickCount64();
    if (now - g_at < 3000) return;
    g_at = now;
    std::vector<Rule> list = Parse(cfg.rules);
    const Rule* hit = nullptr;
    if (!list.empty()) {
        Facts f = Gather(list);
        for (const Rule& r : list)
            if (Holds(r, f)) {
                hit = &r;
                break;
            }
    }
    std::wstring text = hit ? hit->text : L"";
    if (text == g_active) return;
    if (!hit) {  // правило кончилось - всё как было
        autos::Note(L"Правило кончилось: " + g_active + L" - режим и громкость как были");
        Release();
        if (g_themeSet && onTheme) onTheme(L"");
        g_themeSet = false;
        return;
    }
    Release();  // с правила на правило: сначала всё как было, потом новое - без хвостов прошлого
    g_saved = true;
    g_power = tools::GetPowerMode();
    g_volume = vol::AddMaster(0);
    if (!hit->power.empty())
        tools::SetPowerMode(hit->power == L"eco"    ? tools::POWER_SAVER
                            : hit->power == L"perf" ? tools::POWER_BEST
                                                    : tools::POWER_BALANCED);
    if (hit->mute)
        SetVolume(0);
    else if (hit->volume >= 0)
        SetVolume(hit->volume / 100.0f);
    if (onTheme && (!hit->theme.empty() || g_themeSet)) {
        onTheme(hit->theme);
        g_themeSet = !hit->theme.empty();
    }
    if (!hit->run.empty()) corners::Run(hit->run);
    g_active = text;
    autos::Note(L"Правило: " + text);
}

std::wstring ActiveText() { return g_active; }

}  // namespace rules
