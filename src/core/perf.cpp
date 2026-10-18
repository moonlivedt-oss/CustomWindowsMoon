#include "core/perf.h"

#include <cstdio>
#include <map>
#include <string>

namespace perf {

namespace {
struct Stat {
    long long count = 0;
    double total = 0, max = 0;
};
std::map<std::string, Stat>& Stats() {
    static std::map<std::string, Stat> s;
    return s;
}
}  // namespace

bool Enabled() {
    static int on = -1;
    if (on < 0) {
        wchar_t v[8] = {};
        on = GetEnvironmentVariableW(L"RADIAL_PROFILE", v, 8) && v[0] == L'1';
    }
    return on == 1;
}

void Record(const char* name, double ms) {
    Stat& s = Stats()[name];
    ++s.count;
    s.total += ms;
    if (ms > s.max) s.max = ms;
}

void Flush() {
    if (!Enabled() || Stats().empty()) return;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    p = p.substr(0, p.find_last_of(L'\\')) + L"\\profile.log";
    FILE* f = _wfopen(p.c_str(), L"w");
    if (!f) return;
    fprintf(f, "%-28s %8s %10s %10s\n", "scope", "count", "avg ms", "max ms");
    for (auto& [name, s] : Stats())
        fprintf(f, "%-28s %8lld %10.3f %10.3f\n", name.c_str(), s.count, s.total / s.count, s.max);
    fclose(f);
}

}  // namespace perf
