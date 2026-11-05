// Недельный отчёт: числа для сравнения и разница к прошлому отчёту (weekreport.h) - без окон и файлов.
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>

#include "system/auto/weekreport.h"
#include "system/tools.h"

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

namespace {
// «1 раз», «3 раза», «12 раз»
std::wstring Times(int n) {
    int d = n % 10, h = n % 100;
    return std::to_wstring(n) + (d >= 2 && d <= 4 && (h < 12 || h > 14) ? L" раза" : L" раз");
}
}  // namespace

std::vector<std::wstring> Waste(const std::vector<std::wstring>& journal) {
    std::vector<std::wstring> out;
    // «Игровой режим выключен (SnippingTool.exe, шёл 12 с)»: секунды, а не минуты - режим зря дёргался
    auto shortModes = [&](const std::wstring& prefix, const wchar_t* what, const wchar_t* fix) {
        std::map<std::wstring, int> by;
        int total = 0;
        for (const std::wstring& t : journal) {
            if (t.rfind(prefix, 0) != 0) continue;
            size_t ran = t.rfind(L", шёл ");
            if (ran == std::wstring::npos || t.size() < 3 || t.compare(t.size() - 3, 3, L" с)") != 0) continue;
            size_t open = t.find(L'(');
            if (open == std::wstring::npos || open > ran) continue;
            ++by[t.substr(open + 1, ran - open - 1)];
            ++total;
        }
        if (total < 2) return;  // один случайный раз - не повод
        auto top =
            std::max_element(by.begin(), by.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        out.push_back(std::wstring(what) + L" " + Times(total) +
                      L" включался меньше чем на минуту, чаще "
                      L"всего из-за «" +
                      top->first + L"». " + fix);
    };
    shortModes(L"Игровой режим выключен (", L"Игровой режим",
               L"Если это не игра - добавьте её в «Не игры» (студия: Стол - Красота).");
    shortModes(L"Режим разработки выключен (", L"Режим разработки",
               L"Если редактор открывали на минуту - это нормально, иначе проверьте список редакторов.");
    // «Тихая чистка (...): освобождено 0 байт»
    int emptyCleans = 0;
    for (const std::wstring& t : journal)
        if (t.rfind(L"Тихая чистка", 0) == 0 && t.find(L"освобождено 0 ") != std::wstring::npos) ++emptyCleans;
    if (emptyCleans >= 2)
        out.push_back(L"Чистка " + Times(emptyCleans) +
                      L" ничего не нашла - мусора почти нет, вручную её можно запускать реже.");
    // «Сборка CMake упала: путь» - одна и та же папка раз за разом
    std::map<std::wstring, int> builds;
    for (const std::wstring& t : journal) {
        size_t at = t.find(L" упала: ");
        if (t.rfind(L"Сборка", 0) == 0 && at != std::wstring::npos) ++builds[t.substr(at + 8)];
    }
    for (const auto& [path, n] : builds)
        if (n >= 3) {
            size_t slash = path.find_last_of(L"\\/");
            out.push_back(L"Сборка в «" + (slash == std::wstring::npos ? path : path.substr(slash + 1)) + L"» падала " +
                          Times(n) + L" за неделю - стоит один раз посмотреть почему.");
        }
    return out;
}

}  // namespace weekreport
