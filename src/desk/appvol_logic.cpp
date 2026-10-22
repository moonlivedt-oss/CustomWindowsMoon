// Громкость по программам: решение по снимку сеансов и файл уровней (без звука и окон, под тестами).
#include <cmath>
#include <cwchar>

#include "desk/appvol.h"

namespace appvol {

Plan PlanVolumes(const std::vector<SessionNow>& now, std::map<DWORD, float>& last,
                 const std::map<std::wstring, float>& saved, bool first) {
    Plan p;
    std::map<DWORD, float> next;
    for (const SessionNow& s : now) {
        if (!s.pid || s.exe.empty() || next.count(s.pid)) continue;  // системные звуки и повторы pid - мимо
        auto was = last.find(s.pid);
        float level = s.level;
        if (was == last.end()) {
            auto keep = saved.find(s.exe);
            if (!first && keep != saved.end() && std::fabs(keep->second - s.level) > 0.02f) {
                p.set.push_back({s.pid, keep->second});
                level = keep->second;
            }
        } else if (std::fabs(was->second - s.level) > 0.01f) {
            p.save[s.exe] = s.level;
        }
        next[s.pid] = level;
    }
    last.swap(next);
    return p;
}

std::wstring SavedText(const std::map<std::wstring, float>& saved) {
    std::wstring s;
    for (const auto& [exe, v] : saved) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%.2f", v);
        s += exe + L"\t" + buf + L"\n";
    }
    return s;
}

std::map<std::wstring, float> ParseSaved(const std::wstring& text) {
    std::map<std::wstring, float> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        size_t tab = line.find(L'\t');
        if (tab != std::wstring::npos && tab > 0) {
            float v = (float)_wtof(line.c_str() + tab + 1);
            if (v >= 0 && v <= 1) out[line.substr(0, tab)] = v;
        }
        at = end + 1;
    }
    return out;
}

}  // namespace appvol
