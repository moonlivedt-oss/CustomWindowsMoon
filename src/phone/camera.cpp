// «Телефон рядом»: камера телефона на ПК. Страница (только по https - getUserMedia) шлёт кадры JPEG по своей Wi-Fi
// сети, ПК показывает их в обычном окне «Камера телефона»: его выбирают как окно в OBS («Захват окна») или в Discord
// («Демонстрация - окно»). Настоящая веб-камера Windows (MFCreateVirtualCamera) требует COM-источник кадров в
// отдельной DLL, зарегистрированной для службы Frame Server (права администратора) - см. README.
#include <winsock2.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include <algorithm>
#include <memory>

#include "phone/phone_internal.h"

namespace phone::detail {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::string g_frame;                 // последний кадр (под g_lock)
std::atomic<bool> g_pending{false};  // EV_CAM уже в очереди - следующий кадр просто заменит этот
std::atomic<bool> g_closed{false};   // окно закрыли на ПК: кадры не принимаем, пока телефон не начнёт заново
std::atomic<ULONGLONG> g_frameAt{0};
HWND g_wnd = nullptr;
std::unique_ptr<Gdiplus::Bitmap> g_bmp;  // кадр, разобранный в основном потоке

const wchar_t kClass[] = L"MoonRadialPhoneCam";

void Paint(HWND w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc;
    GetClientRect(w, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw > 0 && ch > 0) {
        Gdiplus::Bitmap back(cw, ch, PixelFormat32bppRGB);
        {
            Gdiplus::Graphics g(&back);
            g.Clear(Gdiplus::Color(255, 0, 0, 0));
            if (g_bmp) {  // по размеру окна с сохранением пропорций, по краям - чёрное
                float bw = (float)g_bmp->GetWidth(), bh = (float)g_bmp->GetHeight();
                float k = std::min(cw / bw, ch / bh);
                g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
                g.DrawImage(g_bmp.get(), (cw - bw * k) / 2, (ch - bh * k) / 2, bw * k, bh * k);
            }
            bool stale = !g_bmp || GetTickCount64() - g_frameAt > 3000;
            if (stale) {
                Gdiplus::Font f(L"Segoe UI", 12);
                Gdiplus::SolidBrush b(Gdiplus::Color(220, 241, 236, 244));
                Gdiplus::StringFormat sf;
                sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                // кадр уже был - полоска снизу поверх последнего кадра, а не надпись посреди картинки
                Gdiplus::RectF box = g_bmp ? Gdiplus::RectF(0, ch - 32.0f, (float)cw, 32.0f)
                                           : Gdiplus::RectF(0, 0, (float)cw, (float)ch);
                Gdiplus::SolidBrush shade(Gdiplus::Color(170, 0, 0, 0));
                if (g_bmp) g.FillRectangle(&shade, box);
                g.DrawString(g_bmp ? L"Нет новых кадров с телефона" : L"Ждём кадры: «Камера» на странице телефона", -1,
                             &f, box, &sf, &b);
            }
        }
        Gdiplus::Graphics out(dc);
        out.DrawImage(&back, 0, 0);
    }
    EndPaint(w, &ps);
}

LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint(w);
            return 0;
        case WM_TIMER:  // «нет новых кадров» без кадров не перерисовалось бы
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            g_closed = true;
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            g_wnd = nullptr;
            g_bmp.reset();
            Post(EV_NOTE, L"окно камеры телефона закрыто");
            return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

void Create(int fw, int fh) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        reg = RegisterClassExW(&wc) != 0;
    }
    // окно - под кадр, но не больше половины рабочей области
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int maxW = (wa.right - wa.left) / 2, maxH = (wa.bottom - wa.top) / 2;
    float k = std::min({1.0f, (float)maxW / fw, (float)maxH / fh});
    RECT r{0, 0, (LONG)(fw * k), (LONG)(fh * k)};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    g_wnd = CreateWindowExW(0, kClass, L"Камера телефона", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return;
    SetTimer(g_wnd, 1, 1000, nullptr);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    Post(EV_NOTE, L"камера телефона - в окне «Камера телефона»");
}

}  // namespace

bool CamFrame(std::string jpg, bool start) {
    if (start) g_closed = false;
    if (g_closed) return false;
    AcquireSRWLockExclusive(&g_lock);
    g_frame.swap(jpg);
    ReleaseSRWLockExclusive(&g_lock);
    g_frameAt = GetTickCount64();
    if (!g_pending.exchange(true)) Post(EV_CAM);
    return true;
}

void CamShow() {
    g_pending = false;
    std::string jpg;
    AcquireSRWLockShared(&g_lock);
    jpg = g_frame;
    ReleaseSRWLockShared(&g_lock);
    if (jpg.empty() || g_closed) return;
    IStream* st = SHCreateMemStream((const BYTE*)jpg.data(), (UINT)jpg.size());
    if (!st) return;
    {
        Gdiplus::Bitmap dec(st);  // декодер держит поток - копируем кадр в свой Bitmap
        if (dec.GetLastStatus() == Gdiplus::Ok && dec.GetWidth() && dec.GetHeight()) {
            int w = (int)dec.GetWidth(), h = (int)dec.GetHeight();
            auto copy = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppRGB);
            {
                Gdiplus::Graphics g(copy.get());
                g.DrawImage(&dec, 0, 0, w, h);
            }
            g_bmp = std::move(copy);
            if (!g_wnd) Create(w, h);
        }
    }
    st->Release();
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

void CamStop() {
    if (g_wnd) DestroyWindow(g_wnd);
    g_closed = false;
    AcquireSRWLockExclusive(&g_lock);
    g_frame.clear();
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace phone::detail
