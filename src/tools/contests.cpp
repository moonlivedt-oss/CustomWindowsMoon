// Сроки из ContestReminder (Л7): contests.json самой напоминалки.
#include <ctime>

#include "core/text.h"
#include "tools/contests.h"

namespace tools {

std::vector<Contest> ContestsSoon() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\ContestReminder\\contests.json", p, MAX_PATH);
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(1 << 20, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return SoonContests(ParseContests(Widen(s)), (LONGLONG)time(nullptr));
}

}  // namespace tools
