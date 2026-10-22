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
// замеры идут и из фоновых потоков (сторожа, слежка)
SRWLOCK g_lock = SRWLOCK_INIT;
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
    AcquireSRWLockExclusive(&g_lock);
    Stat& s = Stats()[name];
    ++s.count;
    s.total += ms;
    if (ms > s.max) s.max = ms;
    ReleaseSRWLockExclusive(&g_lock);
}

void Flush() {
    if (!Enabled()) return;
    AcquireSRWLockExclusive(&g_lock);
    std::map<std::string, Stat> stats = Stats();
    ReleaseSRWLockExclusive(&g_lock);
    if (stats.empty()) return;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    p = p.substr(0, p.find_last_of(L'\\')) + L"\\profile.log";
    FILE* f = _wfopen(p.c_str(), L"w");
    if (!f) return;
    fprintf(f, "%-28s %8s %10s %10s\n", "scope", "count", "avg ms", "max ms");
    for (auto& [name, s] : stats)
        fprintf(f, "%-28s %8lld %10.3f %10.3f\n", name.c_str(), s.count, s.total / s.count, s.max);
    fclose(f);
}

}  // namespace perf
