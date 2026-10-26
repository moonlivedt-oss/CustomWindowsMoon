// «Телефон рядом»: то, что Chrome даёт только защищённой странице. Корневой сертификат для телефона, страница как
// приложение (manifest, service worker, значок), «Поделиться - на ПК» (share_target: POST multipart - файлы, текст,
// ссылка) и камера телефона (кадры JPEG - в окно «Камера телефона» на ПК).
#include <winsock2.h>
#include <objidl.h>
#include <gdiplus.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>

#include "core/text.h"
#include "phone/phone_internal.h"

namespace phone::detail {

namespace {

std::string Q(const Request& r, const char* k) {
    auto it = r.query.find(k);
    return it == r.query.end() ? std::string() : it->second;
}

// значок приложения: тёмный квадрат (Android обрежет его кругом), красный экран ПК и белый телефон рядом
std::string IconPng(int size) {
    using namespace Gdiplus;
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(255, 0x16, 0x13, 0x1a));
        float u = size / 100.0f;
        auto Round = [&](Brush& b, float x, float y, float w, float h, float r) {
            GraphicsPath p;
            p.AddArc(x, y, r * 2, r * 2, 180, 90);
            p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
            p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
            p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
            p.CloseFigure();
            g.FillPath(&b, &p);
        };
        SolidBrush red(Color(255, 0xc8, 0x32, 0x3f)), white(Color(255, 0xf1, 0xec, 0xf4)),
            dark(Color(255, 0x16, 0x13, 0x1a));
        Round(red, 20 * u, 28 * u, 50 * u, 34 * u, 4 * u);
        g.FillRectangle(&red, 40 * u, 62 * u, 10 * u, 7 * u);
        g.FillRectangle(&red, 32 * u, 68 * u, 26 * u, 4 * u);
        Round(dark, 58 * u, 40 * u, 24 * u, 38 * u, 5 * u);  // обводка телефона цветом фона
        Round(white, 61 * u, 43 * u, 18 * u, 32 * u, 3.5f * u);
    }
    std::string out;
    IStream* st = nullptr;
    CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) && bmp.Save(st, &png, nullptr) == Gdiplus::Ok) {
        HGLOBAL h = nullptr;
        GetHGlobalFromStream(st, &h);
        STATSTG ss{};
        st->Stat(&ss, STATFLAG_NONAME);
        if (void* p = GlobalLock(h)) {
            out.assign((const char*)p, (size_t)ss.cbSize.QuadPart);
            GlobalUnlock(h);
        }
    }
    if (st) st->Release();
    return out;
}

// POST /share: то, чем поделились из другого приложения. Файлы - как загрузка (фото и видео - в
// «Изображения\Телефон\дата», остальное - в Загрузки), текст - в буфер ПК, ссылка - на выбор на странице.
// Ответ - 303 на страницу с итогом в #s=... (обновление страницы не пошлёт всё заново)
void Share(Conn& cn, const Request& r) {
    Seen();
    auto ct = r.headers.find("content-type");
    std::string boundary = ct == r.headers.end() ? "" : Boundary(ct->second);
    if (boundary.empty() || r.length <= 0) return Text(cn, 400, "ждал multipart/form-data");
    struct Cur {
        bool file = false;
        HANDLE h = INVALID_HANDLE_VALUE;
        std::wstring tmp, dir, name;
        std::string field, value;
        long long size = 0;
    } cur;
    std::string title, text, url, err;
    int files = 0;
    long long bytes = 0;
    std::wstring last;
    auto dropTmp = [&] {
        if (cur.h != INVALID_HANDLE_VALUE) CloseHandle(cur.h);
        cur.h = INVALID_HANDLE_VALUE;
        if (!cur.tmp.empty()) DeleteFileW(cur.tmp.c_str());
        cur.tmp.clear();
    };
    Multipart mp(boundary);
    mp.onBegin = [&](const FormPart& p) {
        cur = Cur();
        cur.file = p.file;
        cur.field = p.name;
        if (!p.file || p.filename.empty()) return true;  // пустая часть файла - Chrome шлёт её, когда файлов нет
        cur.name = SafeName(Widen(p.filename));
        cur.dir = IsMedia(cur.name) ? DatedDir(PhotosDir(), 0) : DownloadsDir();
        SHCreateDirectoryExW(nullptr, cur.dir.c_str(), nullptr);
        ULARGE_INTEGER freeB{};
        if (GetDiskFreeSpaceExW(cur.dir.c_str(), &freeB, nullptr, nullptr) &&
            (long long)freeB.QuadPart < r.length + (256ll << 20)) {
            err = "на компьютере мало места";
            return false;
        }
        unsigned char rnd[6];
        BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        cur.tmp = cur.dir + L"\\." + cur.name + L"." + Widen(Token(rnd, sizeof(rnd))) + L".part";
        cur.h = CreateFileW(cur.tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (cur.h == INVALID_HANDLE_VALUE) {
            cur.tmp.clear();
            err = "не записать";
            return false;
        }
        return true;
    };
    mp.onData = [&](const char* p, size_t n) {
        if (cur.file) {
            if (cur.h == INVALID_HANDLE_VALUE) return true;
            DWORD w = 0;
            if (!WriteFile(cur.h, p, (DWORD)n, &w, nullptr) || w != n) return (err = "не записать"), false;
            cur.size += (long long)n;
            return true;
        }
        if (cur.value.size() + n > (1 << 20)) return (err = "текст длиннее мегабайта"), false;
        cur.value.append(p, n);
        return true;
    };
    mp.onEnd = [&] {
        if (!cur.file) {
            if (cur.field == "title") title = cur.value;
            if (cur.field == "text") text = cur.value;
            if (cur.field == "url") url = cur.value;
            return true;
        }
        if (cur.h == INVALID_HANDLE_VALUE) return true;
        CloseHandle(cur.h);
        cur.h = INVALID_HANDLE_VALUE;
        std::wstring final = UniquePath(cur.dir, cur.name);
        if (final.empty() || !MoveFileExW(cur.tmp.c_str(), final.c_str(), 0)) return (err = "не записать"), false;
        cur.tmp.clear();
        SetFileAttributesW(final.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!IsMedia(cur.name)) MarkFromInternet(final);
        ++files;
        bytes += cur.size;
        last = final;
        Post(EV_RECEIVED, final);
        return true;
    };
    std::vector<char> buf(256 << 10);
    bool ok = true;
    while (ok && cn.bodyLeft > 0 && !Stopping()) {
        int k = cn.Read(buf.data(), (int)buf.size());
        if (k <= 0) {
            ok = false;
            err = "оборвалось";
            break;
        }
        ok = mp.Feed(buf.data(), (size_t)k);
        Lock l;
        S().seen = GetTickCount64();
    }
    if (ok && !mp.Done()) ok = false, err = err.empty() ? "тело не дописано" : err;
    if (!ok) {
        dropTmp();
        if (err.empty()) err = "не понял, чем поделились";
        Post(EV_NOTE, L"«Поделиться» не принято: " + Widen(err));
        return Text(cn, 400, err);
    }
    if (files) {
        Lock l;
        S().received += files;
        S().receivedBytes += bytes;
    }
    std::string link = ShareLink(url, text);
    std::string clip = !text.empty() ? text : url.empty() ? title : url;
    bool clipped = false;
    if (link.empty() && !clip.empty()) {  // просто текст - сразу в буфер ПК
        Post(EV_CLIP, Widen(clip));
        clipped = true;
    }
    if (files || clipped || !link.empty())
        Post(EV_NOTE, L"«Поделиться»: " + std::to_wstring(files) + L" файл(ов)" + (clipped ? L", текст в буфер" : L"") +
                          (link.empty() ? L"" : L", ссылка " + Widen(link)));
    std::string sum =
        "{\"n\":" + std::to_string(files) + ",\"t\":" + (clipped ? "1" : "0") + ",\"l\":" + phone::Json(Widen(link)) +
        ",\"x\":" + phone::Json(Widen(link.empty() || clip == link ? std::string() : clip.substr(0, 2000))) + "}";
    Reply(cn, 303, "", "", "Location: ./#s=" + UrlEncode(sum) + "\r\n");
}

// PUT /cam: кадр JPEG камеры телефона; start=1 - снова открыть окно, если на ПК его закрыли
void Cam(Conn& cn, const Request& r) {
    {
        Lock l;
        S().seen = GetTickCount64();
    }
    std::string jpg;
    if (!ReadAllBody(cn, r, jpg, 8 << 20)) return Text(cn, 413, "кадр больше 8 МБ");
    if (jpg.size() < 4 || (unsigned char)jpg[0] != 0xFF || (unsigned char)jpg[1] != 0xD8)
        return Text(cn, 400, "ждал JPEG");
    if (!CamFrame(std::move(jpg), Q(r, "start") == "1")) return Text(cn, 410, "окно камеры закрыто на ПК");
    Text(cn, 200, "ok");
}

}  // namespace

bool HandleShare(Conn& cn, const Request& r, const std::string& rest) {
    bool get = r.method == "GET" || r.method == "HEAD";
    if (rest == "/ca.crt" && get) {  // свой корневой сертификат: телефон ставит его как «Сертификат ЦС»
        Seen();
        std::string der;
        {
            Lock l;
            der = S().caDer;
        }
        if (der.empty())
            Text(cn, 404, "https на этом ПК не включился");
        else  // не application/x-x509-ca-cert: Android 11+ на такой тип ругается и не сохраняет файл
            Reply(cn, 200, "application/octet-stream", der,
                  "Content-Disposition: attachment; filename=\"MoonDesk-CA.crt\"\r\n");
        return true;
    }
    if (rest == "/manifest.json" && get) {
        Reply(cn, 200, "application/manifest+json; charset=utf-8", ManifestJson());
        return true;
    }
    if (rest == "/sw.js" && get) {
        Reply(cn, 200, "text/javascript; charset=utf-8", ServiceWorkerJs());
        return true;
    }
    if (rest == "/icon.png" && get) {
        int s = std::clamp(atoi(Q(r, "s").c_str()), 48, 512);
        Reply(cn, 200, "image/png", IconPng(s));
        return true;
    }
    if (rest == "/share" && r.method == "POST") {
        Share(cn, r);
        return true;
    }
    if (rest == "/cam" && (r.method == "PUT" || r.method == "POST")) {
        Cam(cn, r);
        return true;
    }
    return false;
}

}  // namespace phone::detail
