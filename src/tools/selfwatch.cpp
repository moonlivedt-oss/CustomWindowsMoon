#include "tools/selfwatch.h"

#include <psapi.h>

#include <algorithm>

#include "tools/tools.h"

namespace tools {

SelfSample SampleSelf() {
    SelfSample s;
    s.at = GetTickCount64();
    HANDLE me = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof(pm);
    if (GetProcessMemoryInfo(me, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm)))
        s.work = pm.WorkingSetSize, s.priv = pm.PrivateUsage;
    GetProcessHandleCount(me, &s.handles);
    s.gdi = GetGuiResources(me, GR_GDIOBJECTS);
    s.user = GetGuiResources(me, GR_USEROBJECTS);
    return s;
}

std::wstring SelfLine(const SelfSample& s) {
    return L"своя память: рабочий набор " + Bytes(s.work) + L", частные " + Bytes(s.priv) + L", дескрипторов " +
           std::to_wstring(s.handles) + L", GDI " + std::to_wstring(s.gdi) + L", USER " + std::to_wstring(s.user);
}

bool SelfLeak(const std::vector<SelfSample>& s, std::wstring& what) {
    what.clear();
    if (s.size() < 2) return false;
    const SelfSample& last = s.back();
    constexpr ULONGLONG kDayMs = 24ull * 3600 * 1000;
    if (last.at - s.front().at < 23ull * 3600 * 1000) return false;  // меньше суток наблюдений - рано судить
    SelfSample low = last;
    for (const SelfSample& x : s) {
        if (last.at - x.at > kDayMs) continue;
        low.priv = std::min(low.priv, x.priv);
        low.handles = std::min(low.handles, x.handles);
        low.gdi = std::min(low.gdi, x.gdi);
        low.user = std::min(low.user, x.user);
    }
    auto add = [&](const std::wstring& t) { what += (what.empty() ? L"" : L", ") + t; };
    if (last.priv - low.priv > kSelfPrivGrow) add(L"частные байты +" + Bytes(last.priv - low.priv));
    if (last.handles - low.handles > kSelfHandleGrow)
        add(L"дескрипторы +" + std::to_wstring(last.handles - low.handles));
    if (last.gdi - low.gdi > kSelfGdiGrow) add(L"GDI +" + std::to_wstring(last.gdi - low.gdi));
    if (last.user - low.user > kSelfUserGrow) add(L"USER +" + std::to_wstring(last.user - low.user));
    return !what.empty();
}

}  // namespace tools
