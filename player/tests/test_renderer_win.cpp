#include "ui/renderer.h"
#include <objbase.h>
#include <cstdio>
#include <initializer_list>

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PlayerMoonRendererRegression";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"Renderer regression", WS_POPUP,
                                0, 0, 900, 640, nullptr, nullptr, wc.hInstance, nullptr);
    ui::Renderer renderer;
    int failures = !window || !renderer.Init(window);
    int frames = 0;
    for (int cycle = 0; !failures && cycle < 5; ++cycle) {
        for (const SIZE size : {SIZE{1200, 800}, SIZE{1920, 1080}, SIZE{420, 680}, SIZE{560, 92}, SIZE{900, 640}}) {
            SetWindowPos(window, nullptr, 0, 0, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
            renderer.Resize(size.cx, size.cy);
            renderer.Resize(0, 0);  // Minimize notifications must not overwrite the valid size.
            if (cycle == 2) renderer.ResetDevice();
            auto* dc = renderer.BeginFrame();
            if (!dc) { ++failures; break; }
            Microsoft::WRL::ComPtr<ID2D1Image> target;
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
            dc->GetTarget(&target);
            if (!target || FAILED(target.As(&bitmap))) ++failures;
            else {
                const auto pixels = bitmap->GetPixelSize();
                if (pixels.width != static_cast<UINT>(size.cx) || pixels.height != static_cast<UINT>(size.cy))
                    ++failures;
            }
            dc->Clear(D2D1::ColorF(0x15161d));
            if (!renderer.EndFrame()) ++failures;
            ++frames;
        }
    }
    // A Direct2D error must be reported and the next frame must obtain a fresh device.
    if (!failures) {
        auto* dc = renderer.BeginFrame();
        const auto generation = renderer.DeviceGeneration();
        if (!dc) ++failures;
        else {
            dc->PopAxisAlignedClip();  // Deliberately unbalanced: D2DERR_POP_CALL_DID_NOT_MATCH_PUSH.
            if (renderer.EndFrame()) ++failures;
            if (!renderer.BeginFrame() || renderer.DeviceGeneration() <= generation) ++failures;
            else if (!renderer.EndFrame()) ++failures;
        }
    }
    renderer.Shutdown();
    if (window) DestroyWindow(window);
    CoUninitialize();
    std::printf("Renderer: %d resize/mode frames, recovery, %d failures\n", frames, failures);
    return failures ? 1 : 0;
}
