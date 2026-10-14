#include "core/night.h"

#include <windows.h>

#include "core/config.h"

namespace night {

namespace {
bool g_on = false;
}

bool Due(const Config& c) {
    if (!c.night || c.nightFrom == c.nightTo) return false;
    SYSTEMTIME t;
    GetLocalTime(&t);
    int h = t.wHour;
    // промежуток может переходить через полночь: 23 - 7
    return c.nightFrom < c.nightTo ? (h >= c.nightFrom && h < c.nightTo) : (h >= c.nightFrom || h < c.nightTo);
}

void Set(bool on) { g_on = on; }
bool On() { return g_on; }

}  // namespace night
