// Выбор режима питания по нагрузке (без окон, под тестами).
#include "app/powerpick.h"

namespace powerpick {

bool Load::Want(ULONGLONG now, double cpu) {
    if (cpu >= enter) {
        lowSince = 0;
        if (!highSince) highSince = now;
        if (now - highSince >= enterMs) busy = true;
    } else if (cpu < leave) {
        highSince = 0;
        if (!lowSince) lowSince = now;
        if (now - lowSince >= leaveMs) busy = false;
    } else {
        highSince = lowSince = 0;  // между порогами - держим как есть, отсчёты заново
    }
    return busy;
}

Mode Pick(bool battery, bool heavy, bool busy) {
    if (battery) return heavy ? BALANCED : SAVER;
    return heavy && busy ? BEST : BALANCED;  // зарядка одна максимум не включает
}

std::wstring Reason(bool battery, bool game, bool dev, bool busy) {
    std::wstring r = battery ? L"от батареи" : L"на зарядке";
    if (!game && !dev) return r;
    r += game ? L", игра" : L", Unreal";
    if (!battery) r += busy ? L", нагрузка" : L", простаивает";
    return r;
}

}  // namespace powerpick
