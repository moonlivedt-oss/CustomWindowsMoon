// Сон после работы: что считается работой и отсчёт (без окон, под тестами).
#include "app/afterwork.h"
#include "tools/guard.h"

namespace afterwork {

std::wstring BusyReason(const std::vector<ProcNow>& procs, const std::wstring& masks, int freshPartials,
                        bool steamDownloading) {
    for (const ProcNow& p : procs) {
        if (guard::Match(p.name, masks)) return p.name;
        if (guard::Match(p.name, L"blender.exe") && p.cpu > 0.2) return L"рендер Blender";
    }
    if (freshPartials) return L"загрузка в браузере";
    if (steamDownloading) return L"загрузка Steam";
    return L"";
}

Countdown::Out Countdown::Feed(bool busy, ULONGLONG now, ULONGLONG quietMs, ULONGLONG countMs) {
    if (busy) {
        idleSince_ = actAt_ = 0;
        return WAIT;
    }
    if (!idleSince_) idleSince_ = now;
    if (!actAt_ && now - idleSince_ >= quietMs) actAt_ = now + countMs;
    if (!actAt_) return WAIT;
    return now >= actAt_ ? ACT : COUNT;
}

}  // namespace afterwork
