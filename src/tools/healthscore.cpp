#include "tools/healthscore.h"

#include <algorithm>

namespace tools {

HealthOut HealthScore(const HealthIn& in) {
    HealthOut out;
    auto minus = [&](int p, const std::wstring& why) {
        if (p > 0) out.minus.push_back({p, why});
    };
    for (const HealthIn::Disk& d : in.disks) {
        std::wstring name = d.root.substr(0, 2);
        if (d.freePct < 10)
            minus(20, name + L" почти заполнен (" + std::to_wstring(d.freePct) + L"% свободно)");
        else if (d.freePct < 20)
            minus(8, name + L": свободно " + std::to_wstring(d.freePct) + L"%");
        if (d.days >= 0 && d.days < 14)
            minus(10, name + L": места хватит на " + std::to_wstring(d.days) + L" дн.");
        else if (d.days >= 0 && d.days < 30)
            minus(4, name + L": места хватит на " + std::to_wstring(d.days) + L" дн.");
    }
    if (in.memLoad >= 90)
        minus(10, L"память занята на " + std::to_wstring(in.memLoad) + L"%");
    else if (in.memLoad >= 80)
        minus(4, L"память занята на " + std::to_wstring(in.memLoad) + L"%");
    if (in.smartBad) minus(25 * in.smartBad, L"диск в плохом состоянии (SMART)");
    if (in.smartWarn) minus(10 * in.smartWarn, L"диск предупреждает (SMART)");
    if (in.danger) minus(std::min(30, 15 * in.danger), L"опасные находки «Защиты»: " + std::to_wstring(in.danger));
    if (in.odd) minus(std::min(10, 3 * in.odd), L"странные находки «Защиты»: " + std::to_wstring(in.odd));
    if (in.batteryWear >= 30)
        minus(8, L"батарея потеряла " + std::to_wstring(in.batteryWear) + L"% ёмкости");
    else if (in.batteryWear >= 20)
        minus(4, L"батарея потеряла " + std::to_wstring(in.batteryWear) + L"% ёмкости");
    if (in.reboot) minus(5, L"Windows ждёт перезагрузки");
    std::stable_sort(out.minus.begin(), out.minus.end(),
                     [](const HealthOut::Minus& a, const HealthOut::Minus& b) { return a.points > b.points; });
    int total = 0;
    for (const HealthOut::Minus& m : out.minus) total += m.points;
    out.score = std::max(0, 100 - total);
    return out;
}

}  // namespace tools
