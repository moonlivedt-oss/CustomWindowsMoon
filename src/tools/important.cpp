// «Важное» в кольце утилит: порядок и предел дел (important.h).
#include "tools/important.h"

#include "tools/tools.h"

namespace tools {

std::vector<ImportantTodo> ImportantTodos(const ImportantIn& in, size_t limit) {
    std::vector<ImportantTodo> out;
    auto take = [&](const std::wstring& key, const std::wstring& why) {
        if (out.size() < limit) out.push_back({key, why});
    };
    for (size_t i = 0; i < in.findScores.size() && i < 12; ++i)
        if (in.findScores[i] >= in.dangerScore) take(L"sec-find:" + std::to_wstring(i), L"Опасное");
    if (in.broken) take(L"rec-unbreak", L"Испорчено");
    if (in.reboot) take(L"sys-reboot", L"Windows ждёт перезагрузки");
    for (const std::wstring& root : in.lowDisks) take(L"scan:" + root, L"Мало места");
    if (in.freeable >= 2ull * 1024 * 1024 * 1024)
        take(in.quickPlaces > 1 ? L"clean-all" : L"clean:", L"Можно освободить " + Bytes(in.freeable));
    if (in.updatesFailed) take(L"sys-wu", L"Обновления не ставятся");
    if (in.deviceErrors) take(L"sys-dev", L"Устройство с ошибкой");
    if (in.driverNewer) take(L"driver", L"Вышел новый драйвер");
    if (in.balanceLowered) take(L"balance-restore", L"Приоритет придержан");
    for (size_t i = 0; i < in.git.size() && i < 2; ++i)
        if (in.git[i].ahead >= 5 || in.git[i].changed >= 20) take(L"git:" + std::to_wstring(i), L"Давно не отправлено");
    return out;
}

bool HeavyDefer(bool onBattery, LONGLONG now, LONGLONG last) {
    return onBattery && last > 0 && last <= now && now - last < 24 * 3600;
}

}  // namespace tools
