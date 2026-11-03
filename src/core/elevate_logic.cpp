// Задачи с правами: проверки пути и имени помощника без системы (под тестами).
#include <cwchar>

#include "core/elevate.h"

namespace elevate {

std::wstring HelperName(const std::wstring& sha256) { return L"RadialMenu-" + sha256.substr(0, 12) + L".exe"; }

bool InsideDir(const std::wstring& path, const std::wstring& dir) {
    if (dir.size() < 3 || path.size() <= dir.size() + 1) return false;
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0 || path[dir.size()] != L'\\') return false;
    // путь внутри - одно имя файла: без подпапок, «..», потоков NTFS (a.exe:x) и короткой записи 8.3
    std::wstring rest = path.substr(dir.size() + 1);
    return rest.find_first_of(L"\\/:~") == std::wstring::npos && rest != L"." && rest != L"..";
}

bool ConsentJob(const std::wstring& key) {
    for (const wchar_t* k :
           {L"deep", L"repair", L"wu-reset", L"defender-offline", L"adapter", L"giants", L"sys-item", L"net-cut", L"vpn-guard-setup"})
        if (key == k) return true;
    return false;
}

bool TrustedHelper(const std::wstring& exePath, const std::wstring& helperDir, const std::wstring& sha256) {
    if (sha256.size() != 64 || !InsideDir(exePath, helperDir)) return false;
    return !_wcsicmp(exePath.c_str() + helperDir.size() + 1, HelperName(sha256).c_str());
}

}  // namespace elevate
