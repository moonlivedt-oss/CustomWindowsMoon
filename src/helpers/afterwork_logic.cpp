// Сон после работы: что считается работой и отсчёт (без окон, под тестами).
#include "helpers/afterwork.h"
#include "system/guard.h"

namespace afterwork {

std::vector<std::wstring> SteamLibraries(const std::wstring& vdf) {
    std::vector<std::wstring> out;
    for (size_t at = 0; (at = vdf.find(L"\"path\"", at)) != std::wstring::npos;) {
        at += 6;
        size_t open = vdf.find(L'"', at), close = open == std::wstring::npos ? open : vdf.find(L'"', open + 1);
        // значение - на той же строке
        if (close == std::wstring::npos || vdf.find(L'\n', at) < open) continue;
        std::wstring path;
        for (size_t i = open + 1; i < close; ++i) {
            if (vdf[i] == L'\\' && i + 1 < close && vdf[i + 1] == L'\\') ++i;
            path += vdf[i];
        }
        if (!path.empty()) out.push_back(path);
        at = close + 1;
    }
    return out;
}

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
