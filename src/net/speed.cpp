// Замер скорости через speed.cloudflare.com: /__down?bytes=N отдаёт N байт, /__up принимает тело запроса.
// Ходим так же, как браузер (системный прокси учитывается), значит и через туннель, если он включён.
#include "net/speed.h"

#include <winhttp.h>

#include <chrono>
#include <vector>

namespace net {

namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

HINTERNET Session() {
    HINTERNET s = WinHttpOpen(L"MoonRadial", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(L"MoonRadial", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 8000, 8000, 8000, 12000);
    return s;
}

// Скачать bytes байт, не дольше capSec: Мбит/с с первого байта (без установки соединения); 0 - не вышло.
double Download(HINTERNET c, ULONGLONG bytes, double capSec, std::atomic<bool>* cancel) {
    std::wstring path = L"/__down?bytes=" + std::to_wstring(bytes);
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    if (!r) return 0;
    double mbps = 0;
    if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        std::vector<char> buf(256 * 1024);
        ULONGLONG got = 0;
        DWORD n = 0;
        Clock::time_point t0 = Clock::now(), t = t0;
        while (WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n) && n) {
            got += n;
            t = Clock::now();
            if (Seconds(t0, t) > capSec || (cancel && *cancel)) break;
        }
        double sec = Seconds(t0, t);
        if (got > 256 * 1024 && sec > 0.05) mbps = got * 8 / sec / 1e6;
    }
    WinHttpCloseHandle(r);
    return mbps;
}

// Отдать bytes байт, не дольше capSec: время от начала отправки до ответа сервера.
double Upload(HINTERNET c, DWORD bytes, double capSec, std::atomic<bool>* cancel) {
    HINTERNET r = WinHttpOpenRequest(c, L"POST", L"/__up", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    if (!r) return 0;
    double mbps = 0;
    std::vector<char> chunk(64 * 1024, 'x');
    Clock::time_point t0 = Clock::now();
    DWORD sent = 0;
    bool ok = WinHttpSendRequest(r, L"Content-Type: application/octet-stream", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0,
                                 bytes, 0);
    while (ok && sent < bytes) {
        DWORD put = 0, want = std::min<DWORD>((DWORD)chunk.size(), bytes - sent);
        ok = WinHttpWriteData(r, chunk.data(), want, &put) && put;
        sent += put;
        if (Seconds(t0, Clock::now()) > capSec || (cancel && *cancel)) break;
    }
    // всё отдали - ждём ответ (до него данные могли ещё лежать в буферах); оборвали по времени - считаем отданное
    if (ok && sent == bytes) ok = WinHttpReceiveResponse(r, nullptr);
    double sec = Seconds(t0, Clock::now());
    if (ok && sent > 256 * 1024 && sec > 0.05) mbps = sent * 8.0 / sec / 1e6;
    WinHttpCloseHandle(r);
    return mbps;
}

}  // namespace

Speed MeasureSpeed(const Log& log, std::atomic<bool>* cancel) {
    Speed s;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    s.when = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    std::wstring adapter;
    s.tunnel = RouteTo(L"1.1.1.1", &adapter);
    log(L"Задержка...");
    s.pingMs = HttpRtt(5000);
    Exit e = ExitNode(5000);
    s.via = adapter;
    if (e.ok && !e.country.empty()) s.via += (s.via.empty() ? L"" : L", ") + CountryName(e.country);
    HINTERNET ses = Session();
    if (!ses) return s;
    if (HINTERNET c = WinHttpConnect(ses, L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        log(L"Загрузка...");
        // разгон: 10 МБ; быстрая сеть успела за секунду - второй заход крупнее, он и в счёт
        s.down = Download(c, 10'000'000, 6, cancel);
        if (s.down > 80 && !(cancel && *cancel)) s.down = std::max(s.down, Download(c, 100'000'000, 8, cancel));
        if (!(cancel && *cancel)) {
            log(L"Отдача...");
            s.up = Upload(c, s.down > 50 ? 25'000'000 : 5'000'000, 8, cancel);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(ses);
    return s;
}

}  // namespace net
