#pragma once
// Какой режим питания ставить (autos): по источнику питания, режимам и настоящей нагрузке. Чистое, под тестами.
//
// Раньше «производительность» держалась всё время, пока открыт Unreal Editor или идёт игра: редактор полчаса
// простаивал, а ноутбук на зарядке шумел - ASUS вешает на ползунок «Макс. производительность» свой профиль
// вентиляторов. Теперь максимум - только пока процессор и правда занят (сборка, шейдеры, игра), простой -
// снова сбалансированный. Включается после enterMs подряд выше порога, выключается после leaveMs подряд ниже
// нижнего порога: между ними держится как было, чтобы не дёргалось.
#include <windows.h>

#include <string>

namespace powerpick {

enum Mode { SAVER, BALANCED, BEST };

struct Load {
    double enter = 0.25, leave = 0.10;  // доля всего процессора (0..1)
    ULONGLONG enterMs = 15000, leaveMs = 90000;
    bool busy = false;
    ULONGLONG highSince = 0, lowSince = 0;

    // Очередной замер. Возвращает, занят ли процессор (с запасом).
    bool Want(ULONGLONG now, double cpu);
    void Reset() { busy = false, highSince = lowSince = 0; }
};

// heavy - игра или редактор; busy - Load::Want
Mode Pick(bool battery, bool heavy, bool busy);
// «на зарядке, Unreal, простаивает» - для журнала и студии
std::wstring Reason(bool battery, bool game, bool dev, bool busy);

}  // namespace powerpick
