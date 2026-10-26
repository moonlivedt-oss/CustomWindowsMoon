// Порядок на Рабочем столе и в снимках: что переносить и в какую папку месяца (под тестами).
#include "core/text.h"
#include "life/tidy.h"

namespace tidy {

bool Candidate(const std::wstring& name, DWORD attrs, ULONGLONG ageDays, ULONGLONG days) {
    if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return false;
    std::wstring n = Lower(name);
    if (n == L"desktop.ini" || n == L"thumbs.db") return false;
    for (const wchar_t* e : {L".lnk", L".url", L".appref-ms", L".website"})
        if (EndsWithI(n, e)) return false;
    return ageDays >= days;
}

std::wstring MonthDir(const std::wstring& folder, const FILETIME& written) {
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&written, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t m[16];
    swprintf(m, 16, L"%04d-%02d", st.wYear, st.wMonth);
    return folder + L"\\Архив\\" + m;
}

}  // namespace tidy
