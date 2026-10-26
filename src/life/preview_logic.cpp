// Предпросмотр: тип файла, вписывание картинки, подпись (под тестами).
#include <algorithm>

#include "core/text.h"
#include "life/preview.h"

namespace preview {

Kind KindOf(const std::wstring& path) {
    static const wchar_t* kImg[] = {L".png", L".jpg",  L".jpeg", L".bmp",  L".gif",  L".webp",
                                    L".tif", L".tiff", L".ico",  L".heic", L".avif", L".jxr"};
    static const wchar_t* kTxt[] = {L".txt",  L".md",  L".log", L".ini",   L".cfg",      L".json", L".xml",  L".yml",
                                    L".yaml", L".csv", L".tsv", L".cpp",   L".h",        L".hpp",  L".c",    L".cs",
                                    L".py",   L".js",  L".ts",  L".tsx",   L".jsx",      L".css",  L".html", L".bat",
                                    L".cmd",  L".ps1", L".sh",  L".gd",    L".toml",     L".rs",   L".go",   L".java",
                                    L".kt",   L".lua", L".sql", L".cmake", L".gitignore"};
    for (const wchar_t* e : kImg)
        if (EndsWithI(path, e)) return IMAGE;
    for (const wchar_t* e : kTxt)
        if (EndsWithI(path, e)) return TEXT;
    return OTHER;
}

RECT Fit(int w, int h, const RECT& box) {
    int bw = box.right - box.left, bh = box.bottom - box.top;
    if (w <= 0 || h <= 0 || bw <= 0 || bh <= 0) return box;
    double k = std::min({(double)bw / w, (double)bh / h, 2.0});
    int fw = std::max(1, (int)(w * k)), fh = std::max(1, (int)(h * k));
    int x = box.left + (bw - fw) / 2, y = box.top + (bh - fh) / 2;
    return {x, y, x + fw, y + fh};
}

std::wstring Caption(const std::wstring& name, long long bytes, int w, int h, size_t at, size_t count) {
    std::wstring s = name;
    if (bytes >= 0) {
        wchar_t b[32];
        if (bytes < (1 << 20))
            swprintf(b, 32, L"%lld КБ", std::max(1ll, bytes >> 10));
        else
            swprintf(b, 32, L"%.1f МБ", bytes / 1048576.0);
        std::wstring t = b;
        std::replace(t.begin(), t.end(), L'.', L',');
        s += L"   " + t;
    }
    if (w > 0 && h > 0) s += L"   " + std::to_wstring(w) + L" x " + std::to_wstring(h);
    if (count > 1) s += L"   " + std::to_wstring(at + 1) + L" из " + std::to_wstring(count);
    return s;
}

}  // namespace preview
