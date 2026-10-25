// Служебный режим «Телефона рядом»: сервер без окна программы - проверка curl'ом и снимок карточки с QR.
#include <windows.h>

#include <vector>

#include "app/devcli_internal.h"
#include "core/files.h"
#include "core/themes.h"
#include "core/config.h"
#include "life/awake.h"
#include "phone/phone.h"

namespace devcli {

namespace {

LRESULT CALLBACK PhoneProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == phone::WM_PHONE) {
        phone::OnMessage(wp, lp);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void SaveScreenPng(int x, int y, int w, int h, const std::wstring& out) {
    using namespace Gdiplus;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    {
        Bitmap shot(bmp, nullptr);
        CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        shot.Save(out.c_str(), &png, nullptr);
    }
    DeleteObject(bmp);
}

}  // namespace

std::vector<Item> LifeRing(const std::wstring& what) {
    if (what == L"awake") return awake::RingItems();
    return {};
}

// --phone-serve <сек> <снимок.png | -> [файлы...]: передача открыта на столько секунд (без файлов - страница
// телефона), ссылка - в phone_out.txt рядом с exe; RADIAL_PHONE_LOCAL=1 - только 127.0.0.1 (без брандмауэра)
int PhoneServe(int seconds, const std::wstring& snap, const std::vector<std::wstring>& files) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdip gdip;
    static Config cfg = LoadOwnConfig();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PhoneProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MoonRadialPhoneServe";
    RegisterClassExW(&wc);
    HWND app = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    phone::Init(inst, app, &cfg);
    if (files.empty())
        phone::OpenPage();
    else
        phone::Offer(files);
    WriteOut(L"phone_out.txt", phone::Url() + L"\n");
    bool shot = snap.empty() || snap == L"-";
    for (ULONGLONG start = GetTickCount64(), until = start + (ULONGLONG)seconds * 1000; GetTickCount64() < until;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        phone::Tick();
        if (!shot && GetTickCount64() - start > 1600) {  // карточка въехала - в картинку
            shot = true;
            RECT wa;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            UINT dpi = GetDpiForSystem();
            int w = MulDiv(470, dpi, 96), h = MulDiv(330, dpi, 96);
            SaveScreenPng(wa.right - w, wa.bottom - h, w, h, snap);
        }
        if (!phone::Active()) break;
        Sleep(16);
    }
    std::wstring left = phone::Status();
    phone::Shutdown();
    WriteOut(L"phone_out.txt", phone::Url() + L"\nостаток: " + left + L"\n");
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

}  // namespace devcli
