#pragma once
// Отмена броска файлов на папку кольца. Проводник переносит или копирует сам (иногда в фоне, уже после Drop, и
// «быстрый» перенос возвращает DROPEFFECT_NONE), поэтому что было на деле - смотрим по файлам: исходник пропал, а в
// папке появился - перенесён; есть оба, а в папке новый - скопирован. «Вернуть»: перенесённое - обратно на место,
// скопированное - в Корзину (через RadialTools.exe recycle-copy, сама программа ничего не удаляет).
#include <windows.h>

#include <string>
#include <vector>

namespace dropundo {

struct Moved {
    std::wstring from, to;
    bool copy = false;
};

struct Result {
    std::vector<Moved> done;
    int pending = 0;  // ещё не понятно (Проводник работает или спросил о замене) - стоит подождать
};

// t0 - время броска (FILETIME в 100 нс): скопированное должно быть создано не раньше.
Result Resolve(const std::vector<std::wstring>& files, const std::wstring& dir, ULONGLONG t0);
ULONGLONG Now();
// Вернуть: итог одной строкой («Вернул 3 из 3»).
std::wstring Undo(const std::vector<Moved>& done);
// Подпись для карточки: «Перенесено 2 в «Проекты»», «Скопировано 1 ...».
std::wstring Title(const std::vector<Moved>& done, const std::wstring& dir);

}  // namespace dropundo
