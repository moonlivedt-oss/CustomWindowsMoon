// Конвертер броском: тип файла, имя результата, битрейт и команды ffmpeg (под тестами).
#include <algorithm>

#include "core/text.h"
#include "life/convert.h"

namespace convert {

Kind KindOf(const std::wstring& path) {
    static const wchar_t* kImg[] = {L".png", L".jpg", L".jpeg", L".webp", L".heic", L".heif", L".avif",
                                    L".bmp", L".tif", L".tiff", L".gif",  L".jxr",  L".ico"};
    static const wchar_t* kVid[] = {L".mp4", L".mov", L".mkv", L".webm", L".avi", L".wmv", L".m4v", L".flv", L".3gp"};
    for (const wchar_t* e : kImg)
        if (EndsWithI(path, e)) return IMAGE;
    for (const wchar_t* e : kVid)
        if (EndsWithI(path, e)) return VIDEO;
    return EndsWithI(path, L".pdf") ? PDF : OTHER;
}

std::wstring OutName(const std::wstring& path, const std::wstring& ext, const std::wstring& tag) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    std::wstring stem =
        dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? path.substr(0, dot) : path;
    return stem + (tag.empty() ? L"" : L" (" + tag + L")") + ext;
}

int VideoKbps(long long targetBytes, double sec) {
    if (sec <= 0) return 0;
    double total = targetBytes * 8.0 * 0.94 / 1000.0 / sec;  // кбит/с на всё
    return std::max(64, (int)(total - 96));
}

std::wstring FfmpegArgs(const std::wstring& op, const std::wstring& in, const std::wstring& out, int kbps) {
    std::wstring q = L"-hide_banner -nostdin -y -i \"" + in + L"\" ";
    if (op == L"img") return q + L"\"" + out + L"\"";  // картинка: формат - по расширению
    if (op == L"gif")
        return q +
               L"-t 15 -vf \"fps=15,scale='min(480,iw)':-2:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];"
               L"[b][p]paletteuse=dither=bayer\" -loop 0 \"" +
               out + L"\"";
    if (op == L"fit")  // сжать до размера: битрейт по длительности, кадр не больше 1280 по ширине
        return q + L"-vf \"scale='min(1280,iw)':-2\" -c:v libx264 -preset medium -b:v " + std::to_wstring(kbps) +
               L"k -maxrate " + std::to_wstring(kbps * 3 / 2) + L"k -bufsize " + std::to_wstring(kbps * 2) +
               L"k -c:a aac -b:a 96k -movflags +faststart \"" + out + L"\"";
    return q + L"-c:v libx264 -preset medium -crf 22 -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart \"" +
           out + L"\"";
}

}  // namespace convert
