// Яркость по программе: разбор правил и решения при смене программы впереди (под тестами).
#include <algorithm>

#include "core/text.h"
#include "desktop/appbright.h"

namespace appbright {

std::map<std::wstring, Rule> ParseRules(const std::wstring& text) {
    std::map<std::wstring, Rule> out;
    for (const std::wstring& part : Split(text, L'|')) {
        size_t eq = part.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring exe = Lower(Trim(part.substr(0, eq))), v = Trim(part.substr(eq + 1));
        if (exe.empty() || v.empty()) continue;
        Rule r;
        r.relative = v[0] == L'+' || v[0] == L'-';
        r.value = std::clamp(_wtoi(v.c_str()), -100, 100);
        if (!r.relative && r.value < 0) continue;
        out[exe] = r;
    }
    return out;
}

std::wstring SaveRules(const std::map<std::wstring, Rule>& rules) {
    std::wstring s;
    for (const auto& [exe, r] : rules)
        s += (s.empty() ? L"" : L"|") + exe + L"=" + (r.relative && r.value >= 0 ? L"+" : L"") +
             std::to_wstring(r.value);
    return s;
}

int Target(int base, const Rule& r) { return std::clamp(r.relative ? base + r.value : r.value, 0, 100); }

int Planner::OnApp(const std::wstring& exe, int cur) {
    auto it = rules.find(Lower(exe));
    if (it != rules.end()) {
        if (active_.empty()) base_ = cur;  // пришли из обычной программы - её яркость и есть исходная
        active_ = it->first;
        set_ = Target(base_, it->second);
        return set_ == cur ? -1 : set_;
    }
    if (active_.empty()) return -1;
    active_.clear();
    int back = base_;
    set_ = base_ = -1;
    return back == cur ? -1 : back;
}

bool Planner::Learn(int cur) {
    if (active_.empty() || set_ < 0 || cur < 0 || cur == set_) return false;
    Rule& r = rules[active_];
    r.value = r.relative ? cur - base_ : cur;
    set_ = cur;
    return true;
}

}  // namespace appbright
