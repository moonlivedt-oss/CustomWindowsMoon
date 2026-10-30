#include "desk/playermoon.h"
#include <shellapi.h>
#include "core/files.h"

namespace playermoon {
std::wstring Path() { return ExeDir() + L"\\apps\\PlayerMoon\\PlayerMoon.exe"; }
bool Installed() { return Exists(Path()); }
bool Open(HWND owner) {
    if (!Installed()) {
        MessageBoxW(owner, L"В этой сборке нет PlayerMoon. Собери RadialMenu с RADIAL_PLAYER=ON или скопируй "
                           L"полную папку apps/PlayerMoon из поставки.", L"PlayerMoon", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    // The player's single-instance protocol activates its existing window without
    // replacing the queue. Set its working directory explicitly, never inherit one.
    auto file = Path();
    auto dir = ExeDir() + L"\\apps\\PlayerMoon";
    auto result = ShellExecuteW(owner, L"open", file.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)result > 32) return true;
    MessageBoxW(owner, L"Не удалось запустить PlayerMoon. Проверь целостность папки apps/PlayerMoon.",
                L"PlayerMoon", MB_OK | MB_ICONERROR);
    return false;
}
}
