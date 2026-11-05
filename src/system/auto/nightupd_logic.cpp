// Обновление программ ночью: окно времени, исключения, причины неудач и итог (под тестами).
#include "core/text.h"
#include "system/auto/nightupd.h"

namespace nightupd {

namespace {

bool MaskMatch(const std::wstring& s, const std::wstring& m) {  // * - сколько угодно знаков, без учёта регистра
    std::wstring a = Lower(s), b = Lower(m);
    size_t si = 0, mi = 0, star = std::wstring::npos, back = 0;
    while (si < a.size()) {
        if (mi < b.size() && b[mi] == L'*') {
            star = mi++;
            back = si;
        } else if (mi < b.size() && b[mi] == a[si]) {
            ++mi, ++si;
        } else if (star != std::wstring::npos) {
            mi = star + 1;
            si = ++back;
        } else {
            return false;
        }
    }
    while (mi < b.size() && b[mi] == L'*') ++mi;
    return mi == b.size();
}

}  // namespace

bool InWindow(int hour, int minute) { return hour >= 2 && (hour < 5 || (hour == 5 && minute < 30)); }

bool Skipped(const std::wstring& id, const std::wstring& name, const std::wstring& masks) {
    for (const std::wstring& m : Split(masks, L'|'))
        if (!m.empty() && (MaskMatch(id, m) || MaskMatch(name, m))) return true;
    return false;
}

std::wstring Why(int code, const std::wstring& out) {
    std::wstring o = Lower(out);
    if (o.find(L"agreement") != std::wstring::npos || o.find(L"соглашени") != std::wstring::npos)
        return L"нужно ваше согласие с условиями";
    switch ((unsigned)code) {
        case 0:
            return L"";
        case 0xFFFFFFFFu:
            return L"не уложился в 15 минут (ждёт окно прав?)";
        case 0x8A150101u:
            return L"программа была запущена";
        case 0x8A150102u:
            return L"шла другая установка";
        case 0x8A150109u:
        case 0x8A150111u:
            return L"нужна перезагрузка";
        case 0x8A15010Bu:
            return L"отменено (окно прав или установщика)";
        case 0x8A15002Bu:
            return L"обновление не подходит к этой версии";
    }
    wchar_t b[48];
    swprintf(b, 48, L"ошибка 0x%08X", (unsigned)code);
    return b;
}

std::wstring Summary(const std::vector<Result>& r) {
    std::wstring ok, bad;
    int nOk = 0, nBad = 0;
    for (const Result& x : r) {
        if (x.code == 0) {
            if (nOk < 4) ok += (nOk ? L", " : L"") + x.name;
            ++nOk;
        } else {
            if (nBad < 3) bad += (nBad ? L", " : L"") + x.name + L" (" + x.why + L")";
            ++nBad;
        }
    }
    if (nOk > 4) ok += L" и ещё " + std::to_wstring(nOk - 4);
    if (nBad > 3) bad += L" и ещё " + std::to_wstring(nBad - 3);
    std::wstring s;
    if (nOk) s = L"обновлено " + std::to_wstring(nOk) + L": " + ok;
    if (nBad) s += (s.empty() ? L"" : L"; ") + std::wstring(L"не вышло ") + std::to_wstring(nBad) + L": " + bad;
    return s.empty() ? L"обновлять было нечего" : s;
}

}  // namespace nightupd
