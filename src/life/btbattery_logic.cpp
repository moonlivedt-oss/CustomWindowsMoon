// Заряд Bluetooth-наушников: общее имя устройства, порог карточки, строка для подсказки (под тестами).
#include "core/text.h"
#include "life/btbattery.h"

namespace btbattery {

std::wstring BaseName(const std::wstring& deviceName) {
    std::wstring n = Trim(deviceName);
    static const wchar_t* kPrefix[] = {L"Транспорт AVRCP ", L"Avrcp Transport ", L"AVRCP Transport "};
    for (const wchar_t* p : kPrefix)
        if (n.rfind(p, 0) == 0) n = n.substr(wcslen(p));
    static const wchar_t* kSuffix[] = {L" Hands-Free HF AG", L" Hands-Free HF",   L" Hands-Free AG",
                                       L" Hands-Free",       L" A2DP SNK",        L" A2DP SRC",
                                       L" Stereo",           L" Avrcp Transport", L" LE"};
    for (bool again = true; again;) {
        again = false;
        for (const wchar_t* s : kSuffix)
            if (n.size() > wcslen(s) && EndsWithI(n, s)) {
                n.resize(n.size() - wcslen(s));
                again = true;
            }
    }
    return Trim(n);
}

bool LowWatch::Feed(const std::wstring& name, int percent) {
    bool& told = told_[name];
    if (percent > 25) told = false;  // зарядили - в следующий раз снова скажем
    if (percent < 0 || percent > 15 || told) return false;
    told = true;
    return true;
}

std::wstring Line(const std::vector<std::pair<std::wstring, int>>& levels) {
    std::wstring s;
    for (const auto& [name, pct] : levels) s += (s.empty() ? L"" : L", ") + name + L": " + std::to_wstring(pct) + L"%";
    return s;
}

}  // namespace btbattery
