#include "desk/startbg.h"

#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "gfx/color.h"
#include "gfx/draw.h"

using namespace Gdiplus;

namespace startbg {

namespace {

constexpr wchar_t kKey[] = L"Software\\Stardock\\Start8\\Start8.ini\\Start8";

std::wstring Start11Exe() {
    wchar_t pf[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%\\Stardock\\Start11\\Start11_64.exe", pf, MAX_PATH);
    return pf;
}

std::wstring OutPath() {
    wchar_t ad[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", ad, MAX_PATH);
    CreateDirectoryW(ad, nullptr);
    return std::wstring(ad) + L"\\start-bg.jpg";
}

bool JpegClsid(CLSID& out) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/jpeg")) {
            out = codecs[i].Clsid;
            return true;
        }
    return false;
}

void RestartStart11() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"Start11_64.exe")) continue;
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
            TerminateProcess(p, 0);
            WaitForSingleObject(p, 3000);
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
    ShellExecuteW(nullptr, nullptr, Start11Exe().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Меню и кнопку Start11 рисует его библиотека внутри Проводника: цвет подложки и кнопка
// подхватываются только после перезапуска Проводника.
void RestartExplorer() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"explorer.exe")) continue;
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
            TerminateProcess(p, 1);
            WaitForSingleObject(p, 3000);
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
    Sleep(1500);  // Windows обычно сам поднимает оболочку; если нет - запускаем
    if (!FindWindowW(L"Shell_TrayWnd", nullptr) && !FindWindowW(L"Progman", nullptr))
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

bool Installed() { return GetFileAttributesW(Start11Exe().c_str()) != INVALID_FILE_ATTRIBUTES; }

// Кнопка Пуск: форма из «Triangle Two» Start11 (три состояния столбиком по 40 px), заливка - цвета темы.
std::wstring MakeButton(const ThemeInfo& t) {
    wchar_t src[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%\\Stardock\\Start11\\StartButtons\\Triangle Two.png", src, MAX_PATH);
    std::unique_ptr<Bitmap> shape(Bitmap::FromFile(src));
    if (!shape || shape->GetLastStatus() != Ok) return L"";
    const int w = (int)shape->GetWidth(), h = (int)shape->GetHeight(), part = h / 3;
    Bitmap out(w, h, PixelFormat32bppARGB);
    // обычное - серебро в сталь, наведение - светлее, открытое меню - акцент
    Color tops[3] = {ToColor(t.gold), MixColor(ToColor(t.fg), ToColor(t.gold), 0.3),
                     MixColor(ToColor(t.ac), ToColor(t.fg), 0.25)};
    Color bottoms[3] = {ToColor(t.glow), ToColor(t.gold), ToColor(t.ac)};
    for (int y = 0; y < h; ++y) {
        int s = std::min(2, y / part);
        double k = (double)(y - s * part) / std::max(1, part - 1);
        Color c = MixColor(tops[s], bottoms[s], k);
        for (int x = 0; x < w; ++x) {
            Color p;
            shape->GetPixel(x, y, &p);
            out.SetPixel(x, y, Color(p.GetA(), c.GetR(), c.GetG(), c.GetB()));
        }
    }
    wchar_t ad[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\start-button.png", ad, MAX_PATH);
    CLSID png;
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png")) {
            png = codecs[i].Clsid;
            return out.Save(ad, &png, nullptr) == Ok ? ad : L"";
        }
    return L"";
}

void SetString(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2));
}

void SetDword(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, 4); }

bool Apply(const ThemeInfo& t, std::wstring& message) {
    COLORREF bgc = t.bg, acc = t.ac;
    if (!Installed()) {
        message = L"Start11 не найден";
        return false;
    }
    wchar_t wp[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
    std::unique_ptr<Bitmap> src(Bitmap::FromFile(wp));
    if (!src || src->GetLastStatus() != Ok || !src->GetWidth()) {
        message = L"Не удалось открыть обои";
        return false;
    }
    // меню Пуск у Start11 примерно 663 x 481; картинка вдвое больше, чтобы хватило на крупный масштаб
    const int W = 1326, H = 962;
    float sw = (float)src->GetWidth(), sh = (float)src->GetHeight();
    // левая половина обоев: небо и меч, без лица - под плитками меню оно только мешало бы
    float cw = sw * 0.5f, ch = cw * H / W;
    if (ch > sh) {
        ch = sh;
        cw = ch * W / H;
    }
    RectF crop(0, (sh - ch) * 0.35f, cw, ch);

    // размытие: уменьшить в 8 раз, размыть, растянуть обратно
    int sw8 = W / 8, sh8 = H / 8;
    Bitmap small(sw8, sh8, PixelFormat32bppRGB);
    {
        Graphics g(&small);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(src.get(), RectF(0, 0, (REAL)sw8, (REAL)sh8), crop.X, crop.Y, crop.Width, crop.Height, UnitPixel);
    }
    {
        BitmapData d;
        Rect r(0, 0, sw8, sh8);
        if (small.LockBits(&r, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppRGB, &d) == Ok) {
            if (d.Stride == sw8 * 4) BoxBlur((uint32_t*)d.Scan0, sw8, sh8, 3);
            small.UnlockBits(&d);
        }
    }
    Bitmap out(W, H, PixelFormat32bppRGB);
    {
        Graphics g(&out);
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(&small, RectF(-8, -8, (REAL)W + 16, (REAL)H + 16));
        Color bg = ToColor(bgc), ac = ToColor(acc);
        // притемнить: текст меню должен читаться; сверху (там обычно светлое небо) вуаль плотнее
        LinearGradientBrush veil(PointF(0, 0), PointF(0, (REAL)H), WithAlpha(bg, 238), WithAlpha(bg, 175));
        g.FillRectangle(&veil, 0, 0, W, H);
        LinearGradientBrush glow(PointF(0, (REAL)H * 0.45f), PointF(0, (REAL)H), WithAlpha(ac, 0), WithAlpha(ac, 95));
        g.FillRectangle(&glow, RectF(0, H * 0.45f, (REAL)W, H * 0.55f + 1));
        LinearGradientBrush top(PointF(0, 0), PointF(0, (REAL)H * 0.3f), Color(40, 255, 255, 255),
                                Color(0, 255, 255, 255));
        g.FillRectangle(&top, RectF(0, 0, (REAL)W, H * 0.3f));
    }
    CLSID jpg;
    std::wstring path = OutPath();
    if (!JpegClsid(jpg) || out.Save(path.c_str(), &jpg, nullptr) != Ok) {
        message = L"Не удалось сохранить картинку";
        return false;
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) {
        message = L"Настройки Start11 не найдены";
        return false;
    }
    SetString(k, L"BackgroundImage", path);
    // подложка меню: графит темы с каплей акцента (Start11 хранит цвет как COLORREF)
    Color menu = MixColor(ToColor(bgc), ToColor(acc), 0.08);
    SetDword(k, L"CustomColourTextureMenu", 1);
    SetDword(k, L"CustomColourTextureMenuV", RGB(menu.GetR(), menu.GetG(), menu.GetB()));
    SetDword(k, L"AccentColour", acc);
    // картинка почти непрозрачная: иначе сквозь неё просвечивает размытый стол и меню сереет
    SetDword(k, L"TextureAlphaValue", 235);
    std::wstring button = MakeButton(t);
    if (!button.empty()) SetString(k, L"Image", button);
    RegCloseKey(k);
    RestartStart11();
    RestartExplorer();
    message = L"Пуск обновлён: фон из обоев, подложка и кнопка в цветах темы";
    return true;
}

}  // namespace startbg
