// История замеров скорости и сравнение «через туннель / напрямую» - без сети, под тестами.
#include <algorithm>
#include <cwchar>
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

std::wstring SpeedCompare(const std::vector<Speed>& history) {
    std::vector<double> td, tu, dd, du;
    // последние 20 замеров каждого вида: старые сети и тарифы не в счёт
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->down <= 0) continue;
        auto& d = it->tunnel ? td : dd;
        auto& u = it->tunnel ? tu : du;
        if (d.size() >= 20) continue;
        d.push_back(it->down);
        if (it->up > 0) u.push_back(it->up);
    }
    if (td.empty() || dd.empty()) return L"";
    auto part = [](const wchar_t* name, const std::vector<double>& d, const std::vector<double>& u) {
        return std::wstring(name) + L" ↓ " + Mbps(Median(d)) + (u.empty() ? L"" : L" / ↑ " + Mbps(Median(u))) + L" (" +
               std::to_wstring(d.size()) + L")";
    };
    return part(L"Обычно через туннель", td, tu) + L", " + part(L"напрямую", dd, du);
}

}  // namespace net
