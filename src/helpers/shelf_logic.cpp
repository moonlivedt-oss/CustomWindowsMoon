// Полка: список файлов без повторов и подпись (под тестами).
#include "core/text.h"
#include "helpers/shelf.h"

namespace shelf {

int Merge(std::vector<std::wstring>& shelf, const std::vector<std::wstring>& add, size_t max) {
    int n = 0;
    for (const std::wstring& f : add) {
        if (f.empty() || shelf.size() >= max) continue;
        bool dup = false;
        for (const std::wstring& s : shelf) dup = dup || Lower(s) == Lower(f);
        if (dup) continue;
        shelf.push_back(f);
        ++n;
    }
    return n;
}

std::wstring Title(const std::vector<std::wstring>& shelf) {
    return shelf.empty() ? L"Полка пуста" : L"Полка: " + Plural((int)shelf.size(), L"файл", L"файла", L"файлов");
}

}  // namespace shelf
