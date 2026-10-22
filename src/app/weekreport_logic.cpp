// Недельный отчёт: числа для сравнения и разница к прошлому отчёту (weekreport.h) - без окон и файлов.
#include <cmath>
#include <cwchar>

#include "app/weekreport.h"
#include "tools/tools.h"

namespace weekreport {

namespace {

std::vector<std::wstring> Cols(const std::wstring& line, wchar_t sep) {
    std::vector<std::wstring> p;
    size_t a = 0;
    for (size_t t; (t = line.find(sep, a)) != std::wstring::npos; a = t + 1) p.push_back(line.substr(a, t - a));
    p.push_back(line.substr(a));
    return p;
}

std::wstring Mbps(double v) {
    wchar_t b[32];
    swprintf(b, 32, v >= 100 ? L"%.0f" : L"%.1f", v);
    return b;
}

}  // namespace

// when, to, drops, туннель, напрямую, падения, износ, диски «C:\=свободно=всего;D:\=...»
std::wstring NumbersRow(const Data& d) {
    wchar_t b[160];
    swprintf(b, 160, L"%lld\t%ls\t%d\t%.2f\t%.2f\t%d\t%d\t", d.when, d.to.c_str(), d.drops, d.tunnelDown, d.directDown,
             d.crashes, d.batteryWear);
    std::wstring s = b;
    for (size_t i = 0; i < d.disks.size(); ++i)
        s += (i ? L";" : L"") + d.disks[i].root + L"=" + std::to_wstring(d.disks[i].free) + L"=" +
             std::to_wstring(d.disks[i].total);
    return s;
}

bool ParseNumbers(const std::wstring& line, Data& d) {
    std::vector<std::wstring> p = Cols(line, L'\t');
    if (p.size() < 8) return false;
    d = Data();
    d.when = _wtoi64(p[0].c_str());
    d.to = p[1];
    d.drops = _wtoi(p[2].c_str());
    d.tunnelDown = _wtof(p[3].c_str());
    d.directDown = _wtof(p[4].c_str());
    d.crashes = _wtoi(p[5].c_str());
    d.batteryWear = _wtoi(p[6].c_str());
    if (!p[7].empty())
        for (const std::wstring& one : Cols(p[7], L';')) {
            std::vector<std::wstring> q = Cols(one, L'=');
            if (q.size() == 3)
                d.disks.push_back({q[0], _wcstoui64(q[1].c_str(), nullptr, 10), _wcstoui64(q[2].c_str(), nullptr, 10)});
        }
    return d.when > 0;
}

std::vector<std::wstring> Deltas(const Data& now, const Data& prev) {
    std::vector<std::wstring> out;
    if (!prev.when) return out;
    const LONGLONG kMin = 1024ll * 1024 * 1024;  // меньше гигабайта - шум
    for (const Disk& d : now.disks)
        for (const Disk& p : prev.disks) {
            if (_wcsicmp(d.root.c_str(), p.root.c_str())) continue;
            LONGLONG delta = (LONGLONG)d.free - (LONGLONG)p.free;
            if (delta >= kMin || delta <= -kMin)
                out.push_back(d.root.substr(0, 2) + L" свободно " + (delta > 0 ? L"+" : L"-") +
                              tools::Bytes((ULONGLONG)(delta > 0 ? delta : -delta)));
        }
    if (now.drops != prev.drops)
        out.push_back(L"обрывов интернета " + std::to_wstring(now.drops) + L" (было " + std::to_wstring(prev.drops) +
                      L")");
    auto speed = [&](const wchar_t* name, double a, double b) {
        if (a <= 0 || b <= 0 || std::fabs(a - b) < b * 0.15) return;  // разница меньше 15% - шум замеров
        int pct = (int)std::lround((a - b) * 100 / b);
        out.push_back(std::wstring(name) + (pct > 0 ? L" быстрее на " : L" медленнее на ") +
                      std::to_wstring(std::abs(pct)) + L"% (↓ " + Mbps(a) + L" против " + Mbps(b) + L")");
    };
    speed(L"через туннель", now.tunnelDown, prev.tunnelDown);
    speed(L"напрямую", now.directDown, prev.directDown);
    if (now.crashes && now.crashes != prev.crashes)
        out.push_back(L"падений программы: " + std::to_wstring(now.crashes));
    if (now.batteryWear >= 0 && prev.batteryWear >= 0 && now.batteryWear > prev.batteryWear)
        out.push_back(L"износ батареи " + std::to_wstring(now.batteryWear) + L"% (было " +
                      std::to_wstring(prev.batteryWear) + L"%)");
    return out;
}

}  // namespace weekreport
