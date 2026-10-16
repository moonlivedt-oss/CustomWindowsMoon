// Режим питания Windows 11 - тот же ползунок, что в Параметрах («Экономия энергии» ... «Макс. производительность»).
// Это «наложение» поверх схемы питания: прав администратора не нужно. Функции есть в powrprof.dll,
// но не во всех заголовках, поэтому берутся по имени.
#include <windows.h>

#include "tools/tools.h"

namespace tools {

namespace {

const GUID kSaver = {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
const GUID kBalanced = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
const GUID kBest = {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};

using GetOverlay = DWORD(WINAPI*)(GUID*);
using SetOverlay = DWORD(WINAPI*)(const GUID&);

HMODULE Powrprof() {
    static HMODULE m = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m;
}

}  // namespace

PowerMode GetPowerMode() {
    auto get = (GetOverlay)GetProcAddress(Powrprof(), "PowerGetEffectiveOverlayScheme");
    GUID g{};
    if (!get || get(&g) != 0) return POWER_UNKNOWN;
    if (IsEqualGUID(g, kSaver)) return POWER_SAVER;
    if (IsEqualGUID(g, kBest)) return POWER_BEST;
    if (IsEqualGUID(g, kBalanced)) return POWER_BALANCED;
    return POWER_UNKNOWN;
}

bool SetPowerMode(PowerMode m) {
    auto set = (SetOverlay)GetProcAddress(Powrprof(), "PowerSetActiveOverlayScheme");
    if (!set || m == POWER_UNKNOWN) return false;
    return set(m == POWER_SAVER ? kSaver : m == POWER_BEST ? kBest : kBalanced) == 0;
}

bool OnBattery() {
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && s.ACLineStatus == 0;
}

const wchar_t* PowerModeName(PowerMode m) {
    switch (m) {
        case POWER_SAVER:
            return L"экономия";
        case POWER_BALANCED:
            return L"сбалансированный";
        case POWER_BEST:
            return L"производительность";
        default:
            return L"свой";
    }
}

}  // namespace tools
