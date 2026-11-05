#pragma once
// «Важное» в кольце утилит (И21): какие дела вынести первым сектором и почему. Чистая функция над снимком данных
// (без Windows и без кольца) - под тестами; кольцо (toolsui_ring.cpp) ищет пункты по ключам и берёт их копии.
#include <windows.h>

#include <string>
#include <vector>

namespace tools {

struct ImportantIn {
    std::vector<int> findScores;         // очки находок «Защиты» по порядку списка (ключ sec-find:i)
    int dangerScore = 6;                 // от скольки очков находка опасна
    bool broken = false;                 // вирус что-то испортил (rec-unbreak)
    bool reboot = false;                 // Windows ждёт перезагрузки
    std::vector<std::wstring> lowDisks;  // корни дисков, где мало места
    ULONGLONG freeable = 0;              // можно освободить (0 - ещё не посчитано)
    int quickPlaces = 0;                 // сколько мест быстрой чистки: больше одного - «Всё сразу»
    bool updatesFailed = false, deviceErrors = false, driverNewer = false, balanceLowered = false;
    struct Repo {
        int ahead = 0, changed = 0;
    };
    std::vector<Repo> git;  // по порядку списка (ключ git:i)
};
struct ImportantTodo {
    std::wstring key;  // начало ключа пункта: sec-find:3, scan:C:\, clean-all...
    std::wstring why;  // причина - первой строкой в подсказке
};
// Дела по важности: опасные находки, испорченное, перезагрузка, мало места, что освободить, обновления, устройства,
// драйвер, придержанный приоритет, неотправленный git; не больше limit.
std::vector<ImportantTodo> ImportantTodos(const ImportantIn& in, size_t limit = 6);

// Тяжёлый фоновый проход от батареи (Н8): ждать зарядки, пока прошлый раз был меньше суток назад. Время - секунды
// unix; last 0 - ещё ни разу (не ждать), часы назад (last в будущем) - не ждать, иначе прохода не будет никогда.
bool HeavyDefer(bool onBattery, LONGLONG now, LONGLONG last);

}  // namespace tools
