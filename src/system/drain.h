#pragma once
// Что посадило батарею (Л38): после работы от батареи - «за 2 ч ушло 60%, больше всего - браузер и Unreal». Доля -
// по процессорному времени программ за то время, что ноутбук был от батареи (видеокарту и экран без прав не
// поделить). Здесь - только итог по готовым числам (под тестами); копит слежка toolsui.
#include <windows.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tools {

struct DrainIn {
    int minutes = 0;                        // сколько был от батареи
    int fromPct = -1, toPct = -1;           // заряд в начале и в конце
    std::map<std::wstring, ULONGLONG> cpu;  // имя exe -> процессорное время за это время, 100 нс
};

struct DrainOut {
    std::wstring title, text;
    std::vector<std::pair<std::wstring, int>> top;  // группа и её доля, %; крупные первыми, до 3
};

// Программа -> понятная группа: chrome/msedge/firefox - «браузер», UnrealEditor и его воркеры - «Unreal», javaw -
// «Minecraft», Code - «VS Code»; иначе имя без .exe
std::wstring DrainGroup(const std::wstring& exe);
// false - сказать нечего: от батареи меньше 20 минут или ушло меньше 5%
bool DrainSummary(const DrainIn& in, DrainOut& out);

}  // namespace tools
