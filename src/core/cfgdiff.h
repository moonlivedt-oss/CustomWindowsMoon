#pragma once
// Чем две версии radial.cfg отличаются (П11): по смыслу, а не по строкам - настройки «ключ: было -> стало», секторы
// краёв и пункты второго кольца по подписи. Без файлов и окон, под тестами; студия показывает это у резервных копий.
#include <string>
#include <vector>

namespace cfgdiff {

struct Change {
    enum Kind { ADDED, REMOVED, CHANGED } kind = CHANGED;
    std::wstring where;  // «Настройки», «Левый край», «Левый край / Код»
    std::wstring what;   // «theme: crimson -> sunset», «сектор «Minecraft»», «пункт «web»»
};

// Что изменилось от older к newer: ADDED - есть только в newer, REMOVED - только в older.
std::vector<Change> Diff(const std::wstring& older, const std::wstring& newer);
// «+3, -1, изменено 2»; пусто - одинаковые
std::wstring Summary(const std::vector<Change>& changes);

}  // namespace cfgdiff
