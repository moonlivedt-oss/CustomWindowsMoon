#include "core/edgecheck.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>

namespace edgecheck {

namespace {

std::string ReadAll(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    std::string s;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (4 << 20)) {
        s.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr)) got = 0;
        s.resize(got);
    }
    CloseHandle(f);
    return s;
}

bool Running(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = !_wcsicmp(pe.szExeFile, exe);
    CloseHandle(snap);
    return found;
}

}  // namespace

bool PowerToysEnabled(const std::string& json, const char* name) {
    size_t sec = json.find("\"enabled\"");
    if (sec == std::string::npos) return false;
    size_t open = json.find('{', sec), close = open == std::string::npos ? open : json.find('}', open);
    if (close == std::string::npos) return false;
    std::string key = std::string("\"") + name + "\"";
    size_t at = json.find(key, open);
    if (at == std::string::npos || at > close) return false;
    size_t colon = json.find(':', at + key.size());
    if (colon == std::string::npos || colon > close) return false;
    size_t v = json.find_first_not_of(" \t\r\n", colon + 1);
    return v != std::string::npos && json.compare(v, 4, "true") == 0;
}

std::wstring Conflict() {
    static ULONGLONG at = 0;
    static std::wstring last;
    ULONGLONG now = GetTickCount64();
    if (at && now - at < 30000) return last;
    at = now;
    last.clear();
    wchar_t base[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\PowerToys\\settings.json", base, MAX_PATH)) return last;
    std::string json = ReadAll(base);
    if (!json.empty() && PowerToysEnabled(json, "CursorWrap") && Running(L"PowerToys.exe"))
        last =
            L"PowerToys CursorWrap переносит курсор через край экрана на другую сторону - до края он не доходит, и "
            L"кольца у краёв не откроются. Выключите CursorWrap в PowerToys или открывайте кольца из углов.";
    return last;
}

}  // namespace edgecheck
