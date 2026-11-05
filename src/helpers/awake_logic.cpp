// «Не спать» на время: что считать недокачанным, когда загрузка кончилась, текст остатка (под тестами).
#include "core/text.h"
#include "helpers/awake.h"

namespace awake {

bool PartialDownload(const std::wstring& name) {
    static const wchar_t* kExt[] = {L".crdownload", L".part", L".partial", L".opdownload", L".download",
                                    L".!ut",        L".!qb",  L".aria2",   L".tmp.part",   L".downloading"};
    for (const wchar_t* e : kExt)
        if (EndsWithI(name, e)) return true;
    return false;
}

bool DoneWatch::Feed(ULONGLONG now, bool busy) {
    if (busy) {
        seenBusy_ = true;
        lastBusy_ = now;
        return false;
    }
    if (!seenBusy_) return now - start_ >= 10 * 60000ull;  // ничего не качается с самого начала
    return now - lastBusy_ >= 5 * 60000ull;
}

std::wstring Left(ULONGLONG ms) {
    ULONGLONG m = (ms + 59999) / 60000;
    if (ms < 60000) return L"ещё меньше минуты";
    if (m < 60) return L"ещё " + std::to_wstring(m) + L" мин";
    wchar_t b[40];
    swprintf(b, 40, L"ещё %llu ч %02llu мин", m / 60, m % 60);
    return b;
}

}  // namespace awake
