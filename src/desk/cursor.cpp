#include "desk/cursor.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/files.h"
#include "core/text.h"

namespace cursor {

namespace {

struct Slot {
    DWORD id;             // OCR_*
    const wchar_t* name;  // значение в HKCU\Control Panel\Cursors
    const wchar_t* aero;  // стандартный файл, если в схеме пусто
};

const Slot kSlots[] = {
    {32512, L"Arrow", L"aero_arrow.cur"},   {32513, L"IBeam", L"beam_r.cur"},
    {32515, L"Crosshair", L"cross_r.cur"},  {32516, L"UpArrow", L"aero_up.cur"},
    {32642, L"SizeNWSE", L"aero_nwse.cur"}, {32643, L"SizeNESW", L"aero_nesw.cur"},
    {32644, L"SizeWE", L"aero_ew.cur"},     {32645, L"SizeNS", L"aero_ns.cur"},
    {32646, L"SizeAll", L"aero_move.cur"},  {32648, L"No", L"aero_unavail.cur"},
    {32649, L"Hand", L"aero_link.cur"},     {32651, L"Help", L"aero_helpsel.cur"},
    {32631, L"NWPen", L"aero_pen.cur"},     {32671, L"Pin", L"aero_pin.cur"},
    {32672, L"Person", L"aero_person.cur"},
};

bool g_applied = false;
std::wstring g_lastKey;

std::wstring SchemeFile(const Slot& s) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = sizeof(buf);
    std::wstring path;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", s.name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, buf, &n) == ERROR_SUCCESS)
        path = ExpandPath(buf);
    if (path.empty()) {
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        path = std::wstring(win) + L"\\Cursors\\" + s.aero;
    }
    return path;
}

// Белое - в цвет, чёрное остаётся чёрным, полутона - пропорционально яркости.
HCURSOR Recolor(HCURSOR src, COLORREF color) {
    ICONINFO ii{};
    if (!GetIconInfo(src, &ii)) return nullptr;
    HCURSOR out = nullptr;
    if (ii.hbmColor) {
        BITMAP bm{};
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
        std::vector<DWORD> px((size_t)bm.bmWidth * bm.bmHeight);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, ii.hbmColor, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS)) {
            bool hasAlpha = false;
            for (DWORD p : px) hasAlpha |= (p >> 24) != 0;
            int cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
            // заливка в разных схемах не всегда чисто белая: самый светлый пиксель становится цветом целиком
            int top = 1;
            for (DWORD p : px) {
                if (hasAlpha && !(p >> 24)) continue;
                top = std::max(top, (int)((((p >> 16) & 255) * 299 + ((p >> 8) & 255) * 587 + (p & 255) * 114) / 1000));
            }
            for (DWORD& p : px) {
                DWORD a = p >> 24;
                if (hasAlpha && !a) continue;
                int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                int lum = std::min(255, (r * 299 + g * 587 + b * 114) / 1000 * 255 / top);
                p = (p & 0xFF000000u) | ((DWORD)(cr * lum / 255) << 16) | ((DWORD)(cg * lum / 255) << 8) |
                    (DWORD)(cb * lum / 255);
            }
            HBITMAP color32 = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, nullptr, nullptr, 0);
            SetDIBits(dc, color32, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS);
            ICONINFO ni{FALSE, ii.xHotspot, ii.yHotspot, ii.hbmMask, color32};
            out = (HCURSOR)CreateIconIndirect(&ni);
            DeleteObject(color32);
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

bool ParseHex(const std::wstring& v, COLORREF& out) {
    if (v.size() != 7 || v[0] != L'#') return false;
    unsigned long x = wcstoul(v.c_str() + 1, nullptr, 16);
    out = RGB((x >> 16) & 0xFF, (x >> 8) & 0xFF, x & 0xFF);
    return true;
}

}  // namespace

void Restore() {
    if (!g_applied) return;
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);  // Windows перечитывает свою схему
    g_applied = false;
    g_lastKey.clear();
}

void Apply(const Config& cfg) {
    COLORREF color = RGB(255, 255, 255);
    bool tint = ParseHex(cfg.cursorColor, color);
    if (!tint && cfg.cursorSize <= 0) {
        Restore();
        return;
    }
    std::wstring key = cfg.cursorColor + L"|" + std::to_wstring(cfg.cursorSize);
    if (g_applied && key == g_lastKey) return;  // уже стоит
    Restore();
    UINT dpi = GetDpiForSystem();
    int size = cfg.cursorSize > 0 ? cfg.cursorSize * (int)dpi / 96 : 0;
    for (const Slot& s : kSlots) {
        std::wstring file = SchemeFile(s);
        if (EndsWithI(file, L".ani")) continue;
        HCURSOR src = (HCURSOR)LoadImageW(nullptr, file.c_str(), IMAGE_CURSOR, size, size,
                                          LR_LOADFROMFILE | (size ? 0 : LR_DEFAULTSIZE));
        if (!src) continue;
        HCURSOR made = tint ? Recolor(src, color) : CopyCursor(src);
        DestroyCursor(src);
        if (made) SetSystemCursor(made, s.id);  // Windows забирает курсор себе, удалять не нужно
    }
    g_applied = true;
    g_lastKey = key;
}

}  // namespace cursor
