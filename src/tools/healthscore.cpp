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

std::vector<AppPart> AppHealth(const AppHealthIn& in) {
    std::vector<AppPart> v;
    auto add = [&](const wchar_t* name, AppPart::State s, const std::wstring& why, int fix = 0) {
        v.push_back({name, s, why, fix});
    };
    if (in.gameMode)
        add(L"перехват мыши", AppPart::OFF, L"снят на время игры");
    else if (!in.mouseWanted)
        add(L"перехват мыши", AppPart::OFF, L"не нужен: углы с колесом, двойной клик и Alt + колесо выключены");
    else if (!in.mouseHooked)
        add(L"перехват мыши", AppPart::BAD, L"не стоит - углы и двойной клик не работают", 1);
    else if (in.mouseSilent)
        add(L"перехват мыши", AppPart::BAD, L"Windows сняла его молча (программа долго не отвечала)", 1);
    else
        add(L"перехват мыши", AppPart::OK, L"стоит");

    if (in.autostartOn)
        add(L"автозапуск", AppPart::OK, L"включён");
    else if (in.autostartUserOff)
        add(L"автозапуск", AppPart::BAD, L"выключен в Диспетчере задач", 2);
    else if (in.autostartWanted == 1)
        add(L"автозапуск", AppPart::BAD, L"пропал, хотя был включён", 2);
    else
        add(L"автозапуск", AppPart::OFF, L"выключен");

    if (in.toolsExe)
        add(L"RadialTools", AppPart::OK, L"на месте");
    else
        add(L"RadialTools", AppPart::BAD, L"пропал - очистка и закрытие программ не работают до пересборки");

    if (in.adminBlocked)
        add(L"задачи с правами", AppPart::BAD, L"задачу не пускает антивирус - каждое действие спрашивает разрешение",
            3);
    else if (in.adminReady)
        add(L"задачи с правами", AppPart::OK, L"помощник на месте");
    else if (in.adminExists)
        add(L"задачи с правами", AppPart::BAD, L"помощник устарел - обновится при первом действии с правами", 3);
    else
        add(L"задачи с правами", AppPart::OFF, L"не созданы - спросит разрешение, когда понадобятся");

    unsigned long long late = in.watchPeriodMs * 3 + 30000;
    if (!in.watchAgeMs)
        add(L"слежка", in.uptimeMs > late ? AppPart::BAD : AppPart::OK,
            in.uptimeMs > late ? L"не прошла ни разу" : L"запускается");
    else if (in.watchAgeMs > late)
        add(L"слежка", AppPart::BAD, L"молчит " + std::to_wstring(in.watchAgeMs / 60000) + L" мин");
    else
        add(L"слежка", AppPart::OK, L"работает");
    return v;
}

std::wstring AppHealthLine(const std::vector<AppPart>& parts) {
    std::wstring bad, ok;
    for (const AppPart& p : parts) {
        if (p.state == AppPart::BAD) bad += (bad.empty() ? L"" : L", ") + p.name + L" (" + p.why + L")";
        if (p.state == AppPart::OK) ok += (ok.empty() ? L"" : L", ") + p.name;
    }
    if (!bad.empty()) return L"Отвалилось: " + bad;
    return ok.empty() ? L"Всё выключено нарочно" : L"Всё работает: " + ok;
}

}  // namespace tools
