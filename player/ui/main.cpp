// Точка входа PlayerMoon: COM в однопоточном режиме (диалоги, WIC) и запуск приложения.
// Осведомлённость о DPI (Per-Monitor V2) задаётся манифестом ui/res/app.manifest.

#include <windows.h>

#include <objbase.h>
#include <roapi.h>
#include <shobjidl.h>

#include "ui/app.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // WinRT media controls need a Windows Runtime apartment, not just classic COM.
    if (FAILED(RoInitialize(RO_INIT_SINGLETHREADED))) return 1;
    SetCurrentProcessExplicitAppUserModelID(L"MoonDesk.PlayerMoon");
    int code = 0;
    {
        ui::App app;
        code = app.Run(instance, show);
    }
    RoUninitialize();
    return code;
}
