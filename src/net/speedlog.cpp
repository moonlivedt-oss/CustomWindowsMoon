// История замеров скорости и сравнение «через туннель / напрямую» - без сети, под тестами.
#include <algorithm>
#include <cwchar>
#include <map>
#include <tuple>
#include <string>

#include "net/speed.h"

namespace net {

namespace {

constexpr size_t kKeep = 200;

std::wstring File() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\net\\speed.tsv", p, MAX_PATH);
    return p;
}

std::wstring Mbps(double v) {
    wchar_t b[32];
    swprintf(b, 32, v >= 100 ? L"%.0f" : L"%.1f", v);
    return b;
}

double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

}  // namespace

std::wstring SpeedRow(const Speed& s) {
    wchar_t b[96];
    swprintf(b, 96, L"%llu\t%.2f\t%.2f\t%d\t%d\t", s.when, s.down, s.up, s.pingMs, s.tunnel ? 1 : 0);
    std::wstring via = s.via;
    std::replace(via.begin(), via.end(), L'\t', L' ');
    std::replace(via.begin(), via.end(), L'\n', L' ');
    return b + via;
}

bool ParseSpeedRow(const std::wstring& line, Speed& s) {
    std::vector<std::wstring> p;
    size_t a = 0;
    for (size_t t; (t = line.find(L'\t', a)) != std::wstring::npos; a = t + 1) p.push_back(line.substr(a, t - a));
    p.push_back(line.substr(a));
    if (p.size() < 6 || p[0].empty() || !iswdigit(p[0][0])) return false;
    s.when = _wcstoui64(p[0].c_str(), nullptr, 10);
    s.down = _wtof(p[1].c_str());
    s.up = _wtof(p[2].c_str());
    s.pingMs = _wtoi(p[3].c_str());
    s.tunnel = p[4] == L"1";
    s.via = p[5];
    return s.when != 0;
}

std::vector<Speed> SpeedHistory() {
    std::vector<Speed> out;
    HANDLE f = CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::string raw(256 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, raw.data(), (DWORD)raw.size(), &got, nullptr);
    CloseHandle(f);
    raw.resize(got);
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), text.data(), n);
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find(L'\n', a);
        std::wstring line = text.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        Speed s;
        if (ParseSpeedRow(line, s)) out.push_back(s);
        if (e == std::wstring::npos) break;
        a = e + 1;
    }
    return out;
}

void SaveSpeed(const Speed& s) {
    std::vector<Speed> all = SpeedHistory();
    all.push_back(s);
    if (all.size() > kKeep) all.erase(all.begin(), all.end() - kKeep);
    std::wstring text;
    for (const Speed& x : all) text += SpeedRow(x) + L"\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string raw(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), raw.data(), n, nullptr, nullptr);
    std::wstring path = File(), tmp = path + L".tmp";
    CreateDirectoryW(path.substr(0, path.rfind(L'\\')).c_str(), nullptr);
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    bool ok = WriteFile(f, raw.data(), (DWORD)raw.size(), &put, nullptr) && put == raw.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring SpeedText(const Speed& s) {
    std::wstring t = L"↓ " + Mbps(s.down) + L" Мбит/с, ↑ " + Mbps(s.up);
    if (s.pingMs >= 0) t += L", задержка " + std::to_wstring(s.pingMs) + L" мс";
    t += s.tunnel ? L", через туннель" : L", напрямую";
    if (!s.via.empty()) t += L" (" + s.via + L")";
    return t;
}

SpeedMedian SpeedMedianOf(const std::vector<Speed>& history, bool tunnel) {
    // последние 20 замеров вида: старые сети и тарифы не в счёт
    std::vector<double> d, u;
    for (auto it = history.rbegin(); it != history.rend() && d.size() < 20; ++it) {
        if (it->down <= 0 || it->tunnel != tunnel) continue;
        d.push_back(it->down);
        if (it->up > 0) u.push_back(it->up);
    }
    return {Median(d), Median(u), (int)d.size()};
}

std::wstring SpeedCompare(const std::vector<Speed>& history) {
    SpeedMedian t = SpeedMedianOf(history, true), d = SpeedMedianOf(history, false);
    if (!t.n || !d.n) return L"";
    auto part = [](const wchar_t* name, const SpeedMedian& m) {
        return std::wstring(name) + L" ↓ " + Mbps(m.down) + (m.up > 0 ? L" / ↑ " + Mbps(m.up) : L"") + L" (" +
               std::to_wstring(m.n) + L")";
    };
    return part(L"Обычно через туннель", t) + L", " + part(L"напрямую", d);
}

std::vector<SpeedDay> SpeedDays(const std::vector<Speed>& history, size_t days) {
    struct Acc {
        std::vector<double> d, u, p;
    };
    std::map<std::tuple<int, int, int, bool>, Acc> by;
    for (const Speed& s : history) {
        if (s.down <= 0) continue;
        FILETIME ft{(DWORD)s.when, (DWORD)(s.when >> 32)}, loc;
        SYSTEMTIME st;
        if (!FileTimeToLocalFileTime(&ft, &loc) || !FileTimeToSystemTime(&loc, &st)) continue;
        Acc& a = by[{st.wYear, st.wMonth, st.wDay, s.tunnel}];
        a.d.push_back(s.down);
        if (s.up > 0) a.u.push_back(s.up);
        if (s.pingMs >= 0) a.p.push_back(s.pingMs);
    }
    std::vector<SpeedDay> out;
    for (const auto& [k, a] : by) {
        SpeedDay d;
        std::tie(d.year, d.month, d.day, d.tunnel) = k;
        d.down = Median(a.d);
        d.up = Median(a.u);
        d.pingMs = a.p.empty() ? -1 : (int)Median(a.p);
        d.n = (int)a.d.size();
        out.push_back(d);
    }
    // последние days дней (в дне может быть две записи - туннель и напрямую)
    size_t seen = 0, from = out.size();
    while (from > 0) {
        bool newDay = from == out.size() || out[from - 1].year != out[from].year ||
                      out[from - 1].month != out[from].month || out[from - 1].day != out[from].day;
        if (newDay && ++seen > days) break;
        --from;
    }
    out.erase(out.begin(), out.begin() + from);
    return out;
}

std::vector<SpeedExit> SpeedByExit(const std::vector<Speed>& history) {
    std::map<std::wstring, std::vector<const Speed*>> by;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (!it->tunnel || it->down <= 0) continue;
        size_t comma = it->via.rfind(L',');
        if (comma == std::wstring::npos) continue;
        size_t a = it->via.find_first_not_of(L' ', comma + 1);
        if (a == std::wstring::npos) continue;
        std::vector<const Speed*>& v = by[it->via.substr(a)];
        if (v.size() < 20) v.push_back(&*it);
    }
    std::vector<SpeedExit> out;
    for (const auto& [country, v] : by) {
        std::vector<double> d, u, p;
        for (const Speed* s : v) {
            d.push_back(s->down);
            if (s->up > 0) u.push_back(s->up);
            if (s->pingMs >= 0) p.push_back(s->pingMs);
        }
        out.push_back({country, Median(d), Median(u), p.empty() ? -1 : (int)Median(p), (int)v.size()});
    }
    std::stable_sort(out.begin(), out.end(), [](const SpeedExit& a, const SpeedExit& b) { return a.down > b.down; });
    return out;
}

std::wstring SpeedExitAdvice(const std::vector<SpeedExit>& exits) {
    if (exits.size() < 2) return L"";
    const SpeedExit &best = exits.front(), &next = exits[1];
    if (best.down < next.down * 1.15) return L"Выходы туннеля по скорости почти равны";  // разница в шуме замеров
    return L"Быстрее всего через «" + best.country + L"»: ↓ " + Mbps(best.down) + L" против ↓ " + Mbps(next.down) +
           L" у «" + next.country + L"»";
}

}  // namespace net
