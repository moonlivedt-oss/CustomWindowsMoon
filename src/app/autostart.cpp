#include "app/autostart.h"

#include <string>

#include <windows.h>

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunName[] = L"MoonRadialMenu";

std::wstring ExeCommand() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return L"\"" + std::wstring(exe) + L"\"";
}

std::wstring RunValue() {
    wchar_t buf[MAX_PATH + 16];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// Диспетчер задач выключает автозапуск не удаляя Run, а флагом в StartupApproved:
// первый байт нечётный (03) - выключено, чётный (02) - включено.
bool DisabledByTaskManager() {
    BYTE v[12];
    DWORD size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunName, RRF_RT_REG_BINARY, nullptr, v, &size) != ERROR_SUCCESS)
        return false;
    return size > 0 && (v[0] & 1);
}

void ClearApproved() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, kRunName);
    RegCloseKey(k);
}

void WriteRun(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = ExeCommand();
        RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunName);
    }
    RegCloseKey(k);
}
}  // namespace

bool AutostartOn() { return !RunValue().empty() && !DisabledByTaskManager(); }

void SetAutostart(bool on) {
    WriteRun(on);
    ClearApproved();  // включение из меню снимает и запрет Диспетчера задач
}

void AutostartRefresh() {
    std::wstring cur = RunValue();
    if (!cur.empty() && cur != ExeCommand()) WriteRun(true);  // exe переехал - путь за ним
}
