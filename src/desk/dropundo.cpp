#include "desk/dropundo.h"

#include <shellapi.h>

#include "core/text.h"
#include "tools/tools.h"

namespace dropundo {

namespace {

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

ULONGLONG Created(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((ULONGLONG)d.ftCreationTime.dwHighDateTime << 32) | d.ftCreationTime.dwLowDateTime;
}

std::wstring Leaf(const std::wstring& p) {
    std::wstring s = p;
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s.substr(s.find_last_of(L'\\') + 1);
}

}  // namespace

ULONGLONG Now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

Result Resolve(const std::vector<std::wstring>& files, const std::wstring& dir, ULONGLONG t0) {
    Result r;
    std::wstring d = dir;
    while (!d.empty() && d.back() == L'\\') d.pop_back();
    for (const std::wstring& f : files) {
        std::wstring to = d + L"\\" + Leaf(f);
        if (!_wcsicmp(to.c_str(), f.c_str())) continue;  // бросили в ту же папку - ничего не произошло
        bool src = Exists(f), dst = Exists(to);
        if (!src && dst)
            r.done.push_back({f, to, false});
        else if (src && dst && Created(to) + 20000000ull >= t0)  // 2 с запаса на часы файловой системы
            r.done.push_back({f, to, true});
        else if (src && !dst)
            ++r.pending;  // ещё копируется или Проводник спрашивает о замене
        // есть оба, но в папке старый файл - Проводник назвал копию по-своему («- копия»): вернуть не сможем
    }
    return r;
}

std::wstring Undo(const std::vector<Moved>& done) {
    int ok = 0, total = (int)done.size();
    std::vector<std::wstring> pairs;
    for (const Moved& m : done) {
        if (m.copy) {
            pairs.push_back(m.from);
            pairs.push_back(m.to);
            continue;
        }
        if (Exists(m.from) || !Exists(m.to)) continue;  // на старом месте уже что-то есть - не затираем
        std::wstring from = m.to + L'\0' + L'\0', to = m.from + L'\0' + L'\0';
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_MOVE;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION | FOF_NOERRORUI;
        if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) ++ok;
    }
    if (!pairs.empty()) ok += tools::RecycleCopies(pairs, nullptr);
    return L"Вернул " + std::to_wstring(ok) + L" из " + std::to_wstring(total);
}

std::wstring Title(const std::vector<Moved>& done, const std::wstring& dir) {
    int copies = 0;
    for (const Moved& m : done) copies += m.copy;
    int n = (int)done.size();
    const wchar_t* what = copies == n ? L"Скопировано " : copies ? L"Перенесено и скопировано " : L"Перенесено ";
    return what + Plural(n, L"файл", L"файла", L"файлов") + L" в «" + Leaf(dir) + L"»";
}

}  // namespace dropundo
