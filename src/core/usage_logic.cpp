// Чем пользуются: файл и выбор «давно без дела» (без окон и файлов, под тестами).
#include <algorithm>
#include <cwchar>

#include "core/usage.h"

namespace usage {

std::wstring Text(const Data& d) {
    std::wstring s = L"_since\t" + std::to_wstring(d.since) + L"\n";
    for (const auto& [key, r] : d.rec)
        if (!key.empty() && key[0] != L'_' && key.find_first_of(L"\t\r\n") == std::wstring::npos)
            s += key + L"\t" + std::to_wstring(r.last) + L"\t" + std::to_wstring(r.count) + L"\n";
    return s;
}

Data Parse(const std::wstring& text) {
    Data d;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t t1 = line.find(L'\t');
        if (t1 == std::wstring::npos || t1 == 0) continue;
        std::wstring key = line.substr(0, t1);
        if (key == L"_since") {
            d.since = _wtoi(line.c_str() + t1 + 1);
            continue;
        }
        size_t t2 = line.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) continue;
        Rec r;
        r.last = _wtoi(line.c_str() + t1 + 1);
        r.count = _wtoi(line.c_str() + t2 + 1);
        if (r.last > 0 && r.count > 0) d.rec[key] = r;
    }
    return d;
}

int TrackedDays(const Data& d, int today) { return d.since > 0 && today >= d.since ? today - d.since : 0; }

std::vector<Idle> Unused(const Data& d, const std::vector<std::wstring>& enabled, int today, int idleDays) {
    std::vector<Idle> out;
    if (TrackedDays(d, today) < idleDays) return out;
    for (const std::wstring& key : enabled) {
        auto it = d.rec.find(key);
        if (it == d.rec.end()) {
            out.push_back({key, -1});
        } else if (today - it->second.last >= idleDays) {
            out.push_back({key, today - it->second.last});
        }
    }
    // ни разу - первыми, дальше - кто дольше без дела
    std::stable_sort(out.begin(), out.end(), [](const Idle& a, const Idle& b) {
        if ((a.days < 0) != (b.days < 0)) return a.days < 0;
        return a.days > b.days;
    });
    return out;
}

}  // namespace usage
