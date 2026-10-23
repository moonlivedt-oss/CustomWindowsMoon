// Обновить программы (Л12): «winget upgrade» без окна - список; обновление - в видимом окне.
#include <windows.h>
#include <shellapi.h>

#include "tools/tools.h"
#include "tools/winget.h"

namespace tools {

std::wstring WingetExe() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\winget.exe", p, MAX_PATH);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES ? p : L"";
}

bool WingetUpgrades(std::vector<Upgrade>& out) {
    out.clear();
    std::wstring exe = WingetExe();
    if (exe.empty()) return false;
    std::wstring text;
    // без --accept-source-agreements: соглашения - не наше дело; без ввода winget их просто не примет
    int code = RunCapture(L"\"" + exe + L"\" upgrade --disable-interactivity", text, 120000);
    out = ParseWingetUpgrade(text);
    return code != -1 && (code == 0 || !out.empty() || text.find(L"-----") != std::wstring::npos);
}

bool WingetUpgradeWindow(const std::wstring& id) {
    std::wstring exe = WingetExe();
    if (exe.empty() || id.find_first_of(L"\"&|<>^ ") != std::wstring::npos) return false;  // ИД - без спецсимволов
    std::wstring args = L"/k chcp 65001 >nul && \"" + exe + L"\" upgrade --id " + id + L" -e";
    return (INT_PTR)ShellExecuteW(nullptr, nullptr, L"cmd.exe", args.c_str(), nullptr, SW_SHOWNORMAL) > 32;
}

}  // namespace tools
