// «Телефон рядом»: HTTP-сервер на время передачи. Свой поток принимает соединения, каждое - в своём коротком
// потоке (не больше 16 сразу), ответ всегда с Connection: close. Без ключа в пути ничего не отдаётся.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>

#include <algorithm>

#include "core/text.h"
#include "phone/phone_internal.h"

namespace phone::detail {

namespace {

SOCKET g_listen = INVALID_SOCKET;
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<int> g_conns{0};
bool g_wsa = false;

bool SendAll(SOCKET c, const char* p, size_t n) {
    while (n > 0) {
        int k = send(c, p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

const char* Reason(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 206:
            return "Partial Content";
        case 301:
            return "Moved Permanently";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 411:
            return "Length Required";
        case 413:
            return "Payload Too Large";
        case 416:
            return "Range Not Satisfiable";
        case 500:
            return "Internal Server Error";
        case 507:
            return "Insufficient Storage";
        default:
            return "Error";
    }
}

std::string Head(int code, const std::string& type, long long length, const std::string& extra = "") {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + Reason(code) + "\r\n";
    if (!type.empty()) h += "Content-Type: " + type + "\r\n";
    h += "Content-Length: " + std::to_string(length) + "\r\n";
    h += "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n"
         "Referrer-Policy: no-referrer\r\n" +
         extra + "\r\n";
    return h;
}

void Reply(SOCKET c, int code, const std::string& type, const std::string& body, const std::string& extra = "") {
    std::string h = Head(code, type, (long long)body.size(), extra);
    if (SendAll(c, h.data(), h.size())) SendAll(c, body.data(), body.size());
}

void Text(SOCKET c, int code, const std::string& body) { Reply(c, code, "text/plain; charset=utf-8", body); }

// тело запроса: сначала то, что пришло вместе с заголовком, потом из сокета
struct Body {
    SOCKET c;
    std::string pre;
    size_t used = 0;
    long long left = 0;
    int Read(char* buf, int n) {
        if (left <= 0) return 0;
        int k = 0;
        if (used < pre.size()) {
            k = (int)std::min<size_t>({(size_t)n, pre.size() - used, (size_t)left});
            memcpy(buf, pre.data() + used, (size_t)k);
            used += (size_t)k;
        } else {
            k = recv(c, buf, (int)std::min<long long>(n, left), 0);
            if (k <= 0) return -1;
        }
        left -= k;
        return k;
    }
};

std::string FilesJson() {  // под замком
    std::string j = "[";
    for (size_t i = 0; i < S().files.size(); ++i) {
        const Offered& f = S().files[i];
        if (i) j += ",";
        j += "{\"name\":" + Json(f.name) + ",\"size\":" + std::to_string(f.size) +
             ",\"done\":" + (f.done ? "true" : "false") + "}";
    }
    return j + "]";
}

void Seen() {
    bool first = false;
    {
        Lock l;
        first = !S().seen;
        S().seen = GetTickCount64();
    }
    if (first) Post(EV_SEEN);
}

void Poll(SOCKET c, const Request& r) {
    Seen();
    long long since = -1;
    auto it = r.query.find("seq");
    if (it != r.query.end()) since = atoll(it->second.c_str());
    for (int i = 0; i < 130 && !g_stop; ++i) {  // долгий опрос: до 20 с ждём перемен
        {
            Lock l;
            if (!S().on) break;
            if (S().seq != since) break;
        }
        Sleep(150);
    }
    std::string j;
    {
        Lock l;
        S().seen = GetTickCount64();
        j = "{\"seq\":" + std::to_string(S().seq) + ",\"clip\":" + Json(S().clip) + ",\"files\":" + FilesJson() + "}";
    }
    Reply(c, 200, "application/json; charset=utf-8", j);
}

void Download(SOCKET c, const Request& r, size_t n) {
    Seen();
    Offered f;
    {
        Lock l;
        if (n >= S().files.size()) return Text(c, 404, "нет такого файла");
        f = S().files[n];
    }
    HANDLE h = CreateFileW(f.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Text(c, 404, "файл пропал с компьютера");
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    long long size = sz.QuadPart, from = 0, to = size - 1;
    bool partial = false;
    auto rg = r.headers.find("range");
    if (rg != r.headers.end()) {
        if (!ParseRange(rg->second, size, from, to)) {
            CloseHandle(h);
            return Reply(c, 416, "", "", "Content-Range: bytes */" + std::to_string(size) + "\r\n");
        }
        partial = true;
    }
    std::string ascii;
    for (wchar_t ch : f.name) ascii += ch >= 32 && ch < 127 && ch != L'"' && ch != L'\\' ? (char)ch : '_';
    std::string extra = "Accept-Ranges: bytes\r\nContent-Disposition: attachment; filename=\"" + ascii +
                        "\"; filename*=UTF-8''" + UrlEncode(Narrow(f.name)) + "\r\n";
    if (partial)
        extra += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" +
                 std::to_string(size) + "\r\n";
    long long len = size > 0 ? to - from + 1 : 0;
    std::string head = Head(partial ? 206 : 200, ContentType(f.name), len, extra);
    bool ok = SendAll(c, head.data(), head.size());
    if (ok && r.method == "HEAD") {
        CloseHandle(h);
        return;
    }
    LARGE_INTEGER at{};
    at.QuadPart = from;
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    std::vector<char> buf(256 << 10);
    long long left = len;
    while (ok && left > 0 && !g_stop) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)std::min<long long>(left, (long long)buf.size()), &got, nullptr) || !got)
            break;
        ok = SendAll(c, buf.data(), got);
        left -= got;
        if (ok) {
            Lock l;
            S().seen = GetTickCount64();
        }
    }
    CloseHandle(h);
    if (ok && left == 0 && to == size - 1) {  // дошли до конца файла
        bool first = false, all = true;
        {
            Lock l;
            if (n < S().files.size()) {
                first = !S().files[n].done;
                S().files[n].done = true;
                ++S().seq;
            }
            for (const Offered& o : S().files) all = all && o.done;
            if (all && !S().allDone) S().allDone = GetTickCount64();
        }
        if (first) Post(EV_FILE_DONE, f.name);
    }
}

std::wstring Unique(const std::wstring& dir, const std::wstring& name) {
    std::wstring p = dir + L"\\" + name;
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    for (int i = 2; i < 10000; ++i) {
        p = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    }
    return L"";
}

FILETIME FromUnixMs(long long ms) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)ms * 10000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    return ft;
}

void Upload(SOCKET c, const Request& r, const std::string& pre) {
    Seen();
    auto te = r.headers.find("transfer-encoding");
    if (te != r.headers.end() || !r.headers.count("content-length")) return Text(c, 411, "нужна длина");
    auto q = [&](const char* k) {
        auto it = r.query.find(k);
        return it == r.query.end() ? std::string() : it->second;
    };
    std::wstring name = SafeName(Widen(q("name")));
    long long mt = atoll(q("mt").c_str());
    bool photo = q("kind") == "photo";
    std::wstring dir = photo ? DatedDir(PhotosDir(), mt) : DownloadsDir();
    if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    DWORD da = dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(dir.c_str());
    if (da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY)) return Text(c, 500, "не создать папку");
    ULARGE_INTEGER freeB{};
    if (GetDiskFreeSpaceExW(dir.c_str(), &freeB, nullptr, nullptr) &&
        (long long)freeB.QuadPart < r.length + (256ll << 20))
        return Text(c, 507, "на компьютере мало места");
    std::wstring tmp = dir + L"\\." + name + L"." + std::to_wstring(GetTickCount64()) + L".part";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Text(c, 500, "не записать");
    Body b{c, pre, 0, r.length};
    std::vector<char> buf(256 << 10);
    bool ok = true;
    while (b.left > 0 && !g_stop) {
        int k = b.Read(buf.data(), (int)buf.size());
        if (k <= 0) {
            ok = false;
            break;
        }
        DWORD w = 0;
        if (!WriteFile(h, buf.data(), (DWORD)k, &w, nullptr) || w != (DWORD)k) {
            ok = false;
            break;
        }
        Lock l;
        S().seen = GetTickCount64();
    }
    ok = ok && b.left == 0;
    if (ok && mt > 0) {  // дата снимка - дата файла
        FILETIME ft = FromUnixMs(mt);
        SetFileTime(h, &ft, nullptr, &ft);
    }
    CloseHandle(h);
    std::wstring final = ok ? Unique(dir, name) : L"";
    if (!ok || final.empty() || !MoveFileExW(tmp.c_str(), final.c_str(), 0)) {
        DeleteFileW(tmp.c_str());
        return Text(c, ok ? 500 : 400, ok ? "не записать" : "оборвалось");
    }
    SetFileAttributesW(final.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!IsMedia(name)) {  // как браузер: файл пришёл извне - Windows спросит перед запуском
        HANDLE z =
            CreateFileW((final + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (z != INVALID_HANDLE_VALUE) {
            const char mark[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
            DWORD w = 0;
            WriteFile(z, mark, sizeof(mark) - 1, &w, nullptr);
            CloseHandle(z);
        }
    }
    {
        Lock l;
        ++S().received;
        S().receivedBytes += r.length;
    }
    Post(EV_RECEIVED, final);
    Text(c, 200, "ok");
}

void ClipIn(SOCKET c, const Request& r, const std::string& pre) {
    Seen();
    if (r.length > (1 << 20)) return Text(c, 413, "слишком длинно");
    Body b{c, pre, 0, r.length};
    std::string text((size_t)r.length, '\0');
    size_t at = 0;
    while (b.left > 0) {
        int k = b.Read(text.data() + at, (int)(text.size() - at));
        if (k <= 0) return Text(c, 400, "оборвалось");
        at += (size_t)k;
    }
    Post(EV_CLIP, Widen(text));
    Text(c, 200, "ok");
}

void Handle(SOCKET c, const Request& r, const std::string& pre) {
    std::string token;
    {
        Lock l;
        if (!S().on) return Text(c, 404, "передача закончилась");
        token = S().token;
    }
    std::string base = "/" + token;
    if (r.path.compare(0, base.size(), base) != 0 || (r.path.size() > base.size() && r.path[base.size()] != '/')) {
        if (r.path != "/favicon.ico") {
            Lock l;
            ++S().bad;
        }
        Post(EV_BAD);
        return Text(c, 404, "нет");
    }
    std::string rest = r.path.substr(base.size());
    if (rest.empty()) return Reply(c, 301, "", "", "Location: " + base + "/\r\n");
    if (rest == "/" && (r.method == "GET" || r.method == "HEAD")) {
        Seen();
        return Reply(
            c, 200, "text/html; charset=utf-8", PageHtml(),
            "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'\r\n");
    }
    if (rest == "/poll") return Poll(c, r);
    if (rest.compare(0, 3, "/f/") == 0 && (r.method == "GET" || r.method == "HEAD"))
        return Download(c, r, (size_t)atoi(rest.c_str() + 3));
    if (rest == "/up" && (r.method == "PUT" || r.method == "POST")) return Upload(c, r, pre);
    if (rest == "/clip" && r.method == "POST") return ClipIn(c, r, pre);
    if (rest == "/cmd" && r.method == "POST") {
        Seen();
        auto it = r.query.find("c");
        float v = Command(it == r.query.end() ? "" : it->second);
        char j[64];
        snprintf(j, sizeof(j), "{\"vol\":%.3f}", v);
        return Reply(c, 200, "application/json", j);
    }
    Text(c, 404, "нет");
}

DWORD WINAPI Conn(void* p) {
    SOCKET c = (SOCKET)(UINT_PTR)p;
    DWORD to = 30000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    std::string data;
    char buf[4096];
    size_t end = std::string::npos;
    while (data.size() < 16384) {
        int k = recv(c, buf, sizeof(buf), 0);
        if (k <= 0) break;
        data.append(buf, (size_t)k);
        if ((end = data.find("\r\n\r\n")) != std::string::npos) break;
    }
    Request r;
    if (end != std::string::npos && ParseHead(data.substr(0, end + 2), r)) {
        Handle(c, r, data.substr(end + 4));
    } else if (!data.empty()) {
        Text(c, 400, "не понял");
    }
    shutdown(c, SD_SEND);
    closesocket(c);
    --g_conns;
    return 0;
}

DWORD WINAPI AcceptLoop(void*) {
    while (!g_stop) {
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(g_listen, &fs);
        timeval tv{0, 300000};
        if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        if (g_conns >= 16) {
            closesocket(c);
            continue;
        }
        ++g_conns;
        HANDLE t = CreateThread(nullptr, 0, Conn, (void*)(UINT_PTR)c, 0, nullptr);
        if (t) {
            CloseHandle(t);
        } else {
            closesocket(c);
            --g_conns;
        }
    }
    return 0;
}

}  // namespace

bool ServerOn() { return g_thread != nullptr; }

bool ServerStart(int& port, bool loopback) {
    if (g_thread) return true;
    if (!g_wsa) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) return false;
        g_wsa = true;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    bool bound = false;
    for (int p = 8780; p < 8800 && !bound; ++p) {
        a.sin_port = htons((u_short)p);
        if (bind(s, (sockaddr*)&a, sizeof(a)) == 0) bound = true, port = p;
    }
    if (!bound || listen(s, 16) != 0) {
        closesocket(s);
        return false;
    }
    g_listen = s;
    g_stop = false;
    g_thread = CreateThread(nullptr, 0, AcceptLoop, nullptr, 0, nullptr);
    if (!g_thread) {
        closesocket(s);
        g_listen = INVALID_SOCKET;
        return false;
    }
    return true;
}

void ServerStop() {
    if (!g_thread) return;
    g_stop = true;
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    closesocket(g_listen);
    g_listen = INVALID_SOCKET;
    for (int i = 0; i < 40 && g_conns > 0; ++i) Sleep(50);  // соединения видят g_stop и бросают передачу
}

}  // namespace phone::detail
