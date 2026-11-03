// Выход VPN с лучшим пингом: правила «мимо VPN» из конфига sing-box, учёт замеров и выбор страны (под тестами).
#include <algorithm>
#include <map>

#include "core/text.h"
#include "net/vpnexit.h"

namespace vpnexit {

namespace {

// строки в кавычках внутри [...] начиная с from
std::vector<std::string> Strings(const std::string& s, size_t from) {
    std::vector<std::string> out;
    size_t open = s.find('[', from), close = s.find(']', from);
    if (open == std::string::npos || close == std::string::npos || close < open) return out;
    for (size_t i = open; i < close;) {
        size_t a = s.find('"', i);
        if (a == std::string::npos || a > close) break;
        size_t b = s.find('"', a + 1);
        if (b == std::string::npos || b > close) break;
        out.push_back(s.substr(a + 1, b - a - 1));
        i = b + 1;
    }
    return out;
}

}  // namespace

std::vector<std::wstring> ParseBypass(const std::string& j) {
    std::vector<std::wstring> out;
    for (size_t at = j.find("\"process_name\""); at != std::string::npos; at = j.find("\"process_name\"", at + 1)) {
        // объект правила вокруг: назад до '{', вперёд до парной '}'
        size_t open = j.rfind('{', at);
        if (open == std::string::npos) continue;
        int depth = 0;
        size_t close = open;
        for (; close < j.size(); ++close) {
            if (j[close] == '{') ++depth;
            if (j[close] == '}' && --depth == 0) break;
        }
        std::string rule = j.substr(open, close - open + 1);
        if (rule.find("\"direct\"") == std::string::npos) continue;
        for (const std::string& p : Strings(j, at)) {
            std::wstring w = Lower(Widen(p));
            if (EndsWithI(w, L".exe")) w.resize(w.size() - 4);
            if (!w.empty() && std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
        }
    }
    return out;
}

std::vector<Sample> Parse(const std::wstring& tsv) {
    std::vector<Sample> out;
    for (const std::wstring& line : Split(tsv, L'\n')) {
        std::vector<std::wstring> f = Split(line, L'\t');
        if (f.size() < 4) continue;
        Sample s{f[0], f[1], _wtoi(f[2].c_str()), _wtoi64(f[3].c_str())};
        if (!s.game.empty() && !s.country.empty() && s.ms > 0) out.push_back(s);
    }
    return out;
}

std::wstring Save(const std::vector<Sample>& list, size_t keep) {
    std::wstring s;
    for (size_t i = list.size() > keep ? list.size() - keep : 0; i < list.size(); ++i)
        s += list[i].game + L"\t" + list[i].country + L"\t" + std::to_wstring(list[i].ms) + L"\t" +
             std::to_wstring(list[i].when) + L"\n";
    return s;
}

std::wstring Better(const std::vector<Sample>& list, const std::wstring& game, const std::wstring& country, int ms,
                    long long now, int* betterMs) {
    std::map<std::wstring, std::vector<int>> by;
    for (const Sample& s : list)
        if (s.game == game && s.country != country && now - s.when <= 30ll * 86400) by[s.country].push_back(s.ms);
    std::wstring best;
    int bestMs = 0;
    for (auto& [c, v] : by) {
        std::sort(v.begin(), v.end());
        int med = v[v.size() / 2];
        if (med * 4 <= ms * 3 && ms - med >= 20 && (best.empty() || med < bestMs)) best = c, bestMs = med;
    }
    if (betterMs) *betterMs = bestMs;
    return best;
}

}  // namespace vpnexit
