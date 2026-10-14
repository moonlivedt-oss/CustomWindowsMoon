#include "desk/icontheme.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <memory>
#include <vector>

using namespace Gdiplus;

namespace icontheme {

namespace {

const CLSID kPng = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};

struct Res {
    WORD id, lang;
    std::vector<BYTE> data;
};

// Сиреневый: оттенок 250-305 градусов, заметная насыщенность.
bool Lilac(int r, int g, int b, float& v, float* sat = nullptr) {
    int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    if (mx < 40 || mx - mn < 18) return false;
    float d = (float)(mx - mn), h;
    if (mx == r)
        h = 60 * std::fmod((g - b) / d, 6.0f);
    else if (mx == g)
        h = 60 * ((b - r) / d + 2);
    else
        h = 60 * ((r - g) / d + 4);
    if (h < 0) h += 360;
    v = mx / 255.0f;
    if (sat) *sat = d / mx;
    return h >= 250 && h <= 305 && d / mx > 0.15f;
}

struct Mapper {
    COLORREF body, accent;
    // Перекрасить 32-битные пиксели BGRA (без предумножения). true - что-то поменялось.
    bool Apply(uint32_t* px, size_t n) const {
        size_t opaque = 0, lilac = 0;
        float v;
        for (size_t i = 0; i < n; ++i) {
            uint32_t p = px[i];
            if ((p >> 24) < 16) continue;
            ++opaque;
            if (Lilac((p >> 16) & 255, (p >> 8) & 255, p & 255, v)) ++lilac;
        }
        // только значки, где сиреневого заметно много: папки и родня
        if (!opaque || lilac * 4 < opaque) return false;
        for (size_t i = 0; i < n; ++i) {
            uint32_t p = px[i];
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            float sat = 0;
            if (!Lilac(r, g, b, v, &sat)) continue;
            // светлый сиреневый (корпус, около 0.94) - в корпус, тёмный (рисунки) - в акцент
            // корпус бледный (насыщенность около 0.25), рисунки на папках сочнее (около 0.35)
            bool part = sat > 0.3f;
            COLORREF c = part ? accent : body;
            // детали - ярким акцентом (темнее только самые тёмные края), корпус - со своей светотенью
            float s = part ? std::min(1.0f, v / 0.6f) : v / 0.94f;
            int nr = std::min(255, (int)(GetRValue(c) * s)), ng = std::min(255, (int)(GetGValue(c) * s)),
                nb = std::min(255, (int)(GetBValue(c) * s));
            px[i] = (p & 0xFF000000u) | (nr << 16) | (ng << 8) | nb;
        }
        return true;
    }
};

bool IsPng(const std::vector<BYTE>& d) { return d.size() > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N'; }

bool RecolorPng(std::vector<BYTE>& data, const Mapper& m) {
    IStream* st = SHCreateMemStream(data.data(), (UINT)data.size());
    if (!st) return false;
    std::unique_ptr<Bitmap> src(Bitmap::FromStream(st));
    bool changed = false;
    if (src && src->GetLastStatus() == Ok) {
        int w = (int)src->GetWidth(), h = (int)src->GetHeight();
        Bitmap copy(w, h, PixelFormat32bppARGB);
        {
            Graphics g(&copy);
            g.SetCompositingMode(CompositingModeSourceCopy);
            g.DrawImage(src.get(), 0, 0, w, h);
        }
        BitmapData bd;
        Rect r(0, 0, w, h);
        if (copy.LockBits(&r, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Ok) {
            if (bd.Stride == w * 4) changed = m.Apply((uint32_t*)bd.Scan0, (size_t)w * h);
            copy.UnlockBits(&bd);
        }
        if (changed) {
            IStream* outSt = SHCreateMemStream(nullptr, 0);
            if (outSt && copy.Save(outSt, &kPng, nullptr) == Ok) {
                STATSTG s{};
                outSt->Stat(&s, STATFLAG_NONAME);
                std::vector<BYTE> out((size_t)s.cbSize.QuadPart);
                LARGE_INTEGER zero{};
                outSt->Seek(zero, STREAM_SEEK_SET, nullptr);
                ULONG got = 0;
                outSt->Read(out.data(), (ULONG)out.size(), &got);
                out.resize(got);
                data.swap(out);
            } else {
                changed = false;
            }
            if (outSt) outSt->Release();
        }
    }
    src.reset();
    st->Release();
    return changed;
}

// Обычная иконка: BITMAPINFOHEADER, высота вдвое (цвет + маска), берём только 32 бита.
bool RecolorDib(std::vector<BYTE>& data, const Mapper& m) {
    if (data.size() < sizeof(BITMAPINFOHEADER)) return false;
    auto* bih = (BITMAPINFOHEADER*)data.data();
    if (bih->biBitCount != 32 || bih->biCompression != BI_RGB) return false;
    int w = bih->biWidth, h = std::abs(bih->biHeight) / 2;
    size_t need = bih->biSize + (size_t)w * h * 4;
    if (data.size() < need) return false;
    return m.Apply((uint32_t*)(data.data() + bih->biSize), (size_t)w * h);
}

std::vector<Res> Icons(const std::wstring& dll) {
    std::vector<Res> out;
    HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) return out;
    struct Ctx {
        HMODULE h;
        std::vector<Res>* out;
    } ctx{h, &out};
    EnumResourceNamesW(
        h, RT_ICON,
        [](HMODULE mod, LPCWSTR type, LPWSTR name, LONG_PTR lp) -> BOOL {
            if (!IS_INTRESOURCE(name)) return TRUE;
            EnumResourceLanguagesW(
                mod, type, name,
                [](HMODULE mod2, LPCWSTR type2, LPCWSTR name2, WORD lang, LONG_PTR lp2) -> BOOL {
                    auto* c = (Ctx*)lp2;
                    HRSRC r = FindResourceExW(mod2, type2, name2, lang);
                    HGLOBAL g = r ? LoadResource(mod2, r) : nullptr;
                    DWORD n = r ? SizeofResource(mod2, r) : 0;
                    if (g && n) {
                        auto* p = (const BYTE*)LockResource(g);
                        c->out->push_back({(WORD)(ULONG_PTR)name2, lang, std::vector<BYTE>(p, p + n)});
                    }
                    return TRUE;
                },
                lp);
            return TRUE;
        },
        (LONG_PTR)&ctx);
    FreeLibrary(h);
    return out;
}

bool CopyTree(const std::wstring& from, const std::wstring& to) {
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring a = from + L"\\" + n, b = to + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok &= CopyTree(a, b);
        else
            ok &= CopyFileW(a.c_str(), b.c_str(), FALSE) != FALSE;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return ok;
}

void ListDlls(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ListDlls(p, out);
        else if (n.size() > 4 &&
                 (!_wcsicmp(n.c_str() + n.size() - 4, L".dll") || !_wcsicmp(n.c_str() + n.size() - 4, L".mun")))
            out.push_back(p);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

}  // namespace

std::wstring SourceFolder() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk\\Engine\\Mods\\icon-resource-redirect\\Settings",
                     L"themeFolder", RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS)
        return L"";
    return buf;
}

bool Build(COLORREF body, COLORREF accent, const std::wstring& name, std::wstring& out, std::wstring& message,
           const std::function<void(int)>& progress) {
    std::wstring src = SourceFolder();
    if (src.empty() || GetFileAttributesW((src + L"\\theme.ini").c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = L"В Windhawk не выбрана папка темы значков";
        return false;
    }
    // своя копия не должна перекрашивать сама себя: берём исходник, если он уже наш
    wchar_t ad[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk\\icons", ad, MAX_PATH);
    SHCreateDirectoryExW(nullptr, ad, nullptr);
    out = std::wstring(ad) + L"\\" + name;
    if (!_wcsnicmp(src.c_str(), ad, wcslen(ad))) {
        message = L"В Windhawk уже стоит тема RadialMenu: верни исходную тему и попробуй снова";
        return false;
    }
    if (!CopyTree(src, out)) {
        message = L"Не удалось скопировать тему";
        return false;
    }
    std::vector<std::wstring> dlls;
    ListDlls(out, dlls);
    Mapper m{body, accent};
    size_t done = 0, changedTotal = 0;
    for (size_t di = 0; di < dlls.size(); ++di) {
        auto icons = Icons(dlls[di]);
        std::vector<Res*> changed;
        for (size_t i = 0; i < icons.size(); ++i) {
            Res& r = icons[i];
            if (IsPng(r.data) ? RecolorPng(r.data, m) : RecolorDib(r.data, m)) changed.push_back(&r);
            if (progress && i % 64 == 0)
                progress((int)((di + (double)i / std::max<size_t>(1, icons.size())) * 100 / dlls.size()));
        }
        if (!changed.empty()) {
            HANDLE u = BeginUpdateResourceW(dlls[di].c_str(), FALSE);
            if (!u) continue;
            for (Res* r : changed)
                UpdateResourceW(u, RT_ICON, MAKEINTRESOURCEW(r->id), r->lang, r->data.data(), (DWORD)r->data.size());
            if (EndUpdateResourceW(u, FALSE)) changedTotal += changed.size();
        }
        ++done;
    }
    if (progress) progress(100);
    message = L"Перекрашено значков: " + std::to_wstring(changedTotal);
    return changedTotal > 0;
}

}  // namespace icontheme
