// «Телефон рядом»: HTTP-сервер на время передачи. Свой поток принимает соединения, каждое - в своём потоке (не
// больше 24 сразу); соединение живёт между запросами (keep-alive), пока клиент не закроет или 30 с тишины. Без ключа
// в пути ничего не отдаётся: /<ключ сессии>/... или /d/<ключ запомненного телефона>/...
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>

#include "core/text.h"
#include "phone/phone_internal.h"

namespace phone::detail {

std::atomic<long long> g_sent{0}, g_recv{0}, g_sendLeft{0};

namespace {

SOCKET g_listen = INVALID_SOCKET, g_listenTls = INVALID_SOCKET;
thread_local Tls* t_tls = nullptr;  // соединение по https (каждое соединение - в своём потоке)
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<int> g_conns{0};
bool g_wsa = false;

const char* Reason(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 206:
            return "Partial Content";
        case 303:
            return "See Other";
        case 301:
            return "Moved Permanently";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 409:
            return "Conflict";
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

std::string FilesJson() {  // под замком
    std::string j = "[";
    for (size_t i = 0; i < S().files.size(); ++i) {
        const Offered& f = S().files[i];
        if (i) j += ",";
        j += "{\"name\":" + phone::Json(f.name) + ",\"size\":" + std::to_string(f.size) +
             ",\"done\":" + (f.done ? "true" : "false") + "}";
    }
    return j + "]";
}

void Poll(Conn& cn, const Request& r) {
    Seen();
    long long since = -1;
    auto it = r.query.find("seq");
    if (it != r.query.end()) since = atoll(it->second.c_str());
    for (int i = 0; i < 130 && !g_stop; ++i) {  // долгий опрос: до 20 с ждём перемен
        {
            Lock l;
            if (!S().on || S().seq != since) break;
        }
        Sleep(150);
    }
    std::string j;
    {
        Lock l;
        S().seen = GetTickCount64();
        std::vector<std::pair<std::string, long long>> z;
        for (const Offered& f : S().files) z.push_back({ZipName(f.name), f.size});
        j = "{\"seq\":" + std::to_string(S().seq) + ",\"clip\":" + phone::Json(S().clip) +
            ",\"img\":" + std::to_string(S().clipImg) + ",\"files\":" + FilesJson() +
            ",\"zip\":" + (z.size() > 1 && ZipFits(z) ? "true" : "false") +
            ",\"input\":" + (InputAllowed() ? "true" : "false") +
            ",\"screen\":" + (ScreenAllowed() ? "true" : "false") + ",\"fs\":" + (FilesAllowed() ? "true" : "false") +
            ",\"paired\":" + (S().standing ? "true" : "false") + ",\"tls\":" + std::to_string(S().tlsPort) +
            ",\"sec\":" + (cn.tls ? "true" : "false") + ",\"find\":" + std::to_string(S().findSeq) + ",\"links\":[";
        for (size_t k = 0; k < S().links.size(); ++k) j += (k ? "," : "") + phone::Json(S().links[k]);
        j += "],\"shares\":[";
        std::vector<std::wstring> roots = ShareRoots();
        for (size_t k = 0; k < roots.size(); ++k)
            j += (k ? "," : "") + phone::Json(roots[k].substr(roots[k].find_last_of(L'\\') + 1));
        j += "]}";
    }
    JsonReply(cn, j);
}

void ClipIn(Conn& cn, const Request& r) {
    Seen();
    std::string text;
    if (!ReadAllBody(cn, r, text, 1 << 20)) return Text(cn, 413, "слишком длинно или оборвалось");
    Post(EV_CLIP, Widen(text));
    Text(cn, 200, "ok");
}

// ключ в пути: /<сессия> или /d/<запомненный телефон>; вернёт длину префикса, 0 - чужой
size_t KeyPrefix(const std::string& path, int* device) {
    Lock l;
    if (!S().on) return 0;
    return MatchKey(path, S().token, S().devices, device);
}

void Handle(Conn& cn, const Request& r) {
    {
        Lock l;
        if (!S().on) return Text(cn, 404, "передача закончилась");
    }
    int device = -1;
    size_t pre = KeyPrefix(r.path, &device);
    if (!pre) {
        if (r.path != "/favicon.ico") {
            Lock l;
            ++S().bad;
        }
        Post(EV_BAD);
        return Text(cn, 404, "нет");
    }
    std::string base = r.path.substr(0, pre), rest = r.path.substr(pre);
    // Ключ запомненного телефона - постоянный: по http его видит любой в той же Wi-Fi. Когда https открыт, страница
    // уходит туда, остальное по http отклоняется. Ключ сессии (QR) по http работает - это первое знакомство.
    int tlsPort = 0;
    {
        Lock l;
        tlsPort = S().tlsPort;
    }
    if (device >= 0 && !cn.tls && tlsPort) {
        auto host = r.headers.find("host");
        std::string to =
            HttpsLocation(host == r.headers.end() ? "" : host->second, tlsPort, base + (rest.empty() ? "/" : rest));
        if ((r.method == "GET" || r.method == "HEAD") && (rest.empty() || rest == "/") && !to.empty())
            return Reply(cn, 301, "", "", "Location: " + to + "\r\n");
        return Text(cn, 403, "запомненный телефон - только по https");
    }
    if (rest.empty()) return Reply(cn, 301, "", "", "Location: " + base + "/\r\n");
    if (rest == "/" && (r.method == "GET" || r.method == "HEAD")) {
        Seen();
        return Reply(cn, 200, "text/html; charset=utf-8", PageHtml(),
                     "Content-Security-Policy: default-src 'self'; img-src 'self' blob: data:; media-src 'self'; "
                     "worker-src 'self'; manifest-src 'self'; "
                     "style-src 'unsafe-inline'; script-src 'unsafe-inline'\r\n");
    }
    if (rest == "/poll") return Poll(cn, r);
    if (rest == "/clip" && r.method == "POST") return ClipIn(cn, r);
    if (HandleFiles(cn, r, rest) || HandleMore(cn, r, rest) || HandleRemote(cn, r, rest) || HandleShare(cn, r, rest) ||
        HandleExtra(cn, r, rest) || HandleApp(cn, r, rest))
        return;
    Text(cn, 404, "нет");
}

DWORD WINAPI ConnThread(void* p) {
    Conn cn;
    cn.c = (SOCKET)((UINT_PTR)p & ~(UINT_PTR)1);
    cn.tls = ((UINT_PTR)p & 1) != 0;
    DWORD to = 30000;
    setsockopt(cn.c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(cn.c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    BOOL nodelay = TRUE;  // тачпад: мелкие ответы - сразу
    setsockopt(cn.c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    char buf[8192];
    if (cn.tls && !(t_tls = TlsAccept(cn.c))) cn.close = true;  // сертификат не принят или не TLS
    for (int requests = 0; !g_stop && !cn.close && requests < 10000; ++requests) {
        size_t end;
        while ((end = cn.in.find("\r\n\r\n")) == std::string::npos && cn.in.size() < 16384) {
            int k = NetRecv(cn.c, buf, sizeof(buf));
            if (k <= 0) {
                cn.close = true;
                break;
            }
            cn.in.append(buf, (size_t)k);
        }
        if (cn.close) break;
        Request r;
        if (end == std::string::npos || !ParseHead(cn.in.substr(0, end + 2), r)) {
            cn.close = true;
            Text(cn, 400, "не понял");
            break;
        }
        cn.in.erase(0, end + 4);
        cn.bodyLeft = r.length;
        auto conn = r.headers.find("connection");
        if (conn != r.headers.end() && Lower(Widen(conn->second)) == L"close") cn.close = true;
        if (r.headers.count("transfer-encoding")) cn.close = true;  // кусочного тела не разбираем
        Handle(cn, r);
        if (cn.bodyLeft > 0) cn.close = true;  // тело не дочитали - дальше в соединении не разобрать
    }
    TlsClose(t_tls);
    t_tls = nullptr;
    shutdown(cn.c, SD_SEND);
    closesocket(cn.c);
    --g_conns;
    return 0;
}

DWORD WINAPI AcceptLoop(void*) {
    while (!g_stop) {
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(g_listen, &fs);
        if (g_listenTls != INVALID_SOCKET) FD_SET(g_listenTls, &fs);
        timeval tv{0, 300000};
        if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
        for (SOCKET l : {g_listen, g_listenTls}) {
            if (l == INVALID_SOCKET || !FD_ISSET(l, &fs)) continue;
            SOCKET c = accept(l, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            if (g_conns >= 24) {
                closesocket(c);
                continue;
            }
            ++g_conns;
            // младший бит - https (номер сокета кратен 4)
            HANDLE t =
                CreateThread(nullptr, 0, ConnThread, (void*)((UINT_PTR)c | (l == g_listenTls ? 1 : 0)), 0, nullptr);
            if (t) {
                CloseHandle(t);
            } else {
                closesocket(c);
                --g_conns;
            }
        }
    }
    return 0;
}

}  // namespace

std::atomic<bool>& Stopping() { return g_stop; }

int NetRecv(UINT_PTR c, char* buf, int n) { return t_tls ? TlsRecv(t_tls, buf, n) : recv(c, buf, n, 0); }

int Conn::Read(char* buf, int n) {
    if (bodyLeft <= 0) return 0;
    int k = 0;
    if (!in.empty()) {
        k = (int)std::min<long long>({(long long)n, (long long)in.size(), bodyLeft});
        memcpy(buf, in.data(), (size_t)k);
        in.erase(0, (size_t)k);
    } else {
        k = NetRecv(c, buf, (int)std::min<long long>(n, bodyLeft));
        if (k <= 0) {
            close = true;
            return -1;
        }
    }
    bodyLeft -= k;
    g_recv += k;
    return k;
}

bool SendAll(UINT_PTR c, const char* p, size_t n) {
    if (t_tls) {
        for (size_t at = 0; at < n;) {  // кусками, чтобы скорость на карточке шла ровно
            size_t len = std::min<size_t>(n - at, 1 << 20);
            if (!TlsSend(t_tls, p + at, len)) return false;
            at += len;
            g_sent += (long long)len;
        }
        return true;
    }
    while (n > 0) {
        int k = send(c, p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
        g_sent += k;
    }
    return true;
}

std::string Head(Conn& cn, int code, const std::string& type, long long length, const std::string& extra) {
    if (cn.bodyLeft > 0) cn.close = true;  // тело запроса не дочитано - соединение после ответа закроется
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + Reason(code) + "\r\n";
    if (!type.empty()) h += "Content-Type: " + type + "\r\n";
    if (length >= 0)
        h += "Content-Length: " + std::to_string(length) + "\r\n";
    else
        cn.close = true;  // длина неизвестна - конец ответа = конец соединения
    h += std::string("Cache-Control: no-store\r\nConnection: ") + (cn.close ? "close" : "keep-alive") +
         "\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n" + extra + "\r\n";
    return h;
}

void Reply(Conn& cn, int code, const std::string& type, const std::string& body, const std::string& extra) {
    std::string h = Head(cn, code, type, (long long)body.size(), extra);
    if (!SendAll(cn.c, h.data(), h.size()) || !SendAll(cn.c, body.data(), body.size())) cn.close = true;
}

void Text(Conn& cn, int code, const std::string& body) { Reply(cn, code, "text/plain; charset=utf-8", body); }

void JsonReply(Conn& cn, const std::string& body) { Reply(cn, 200, "application/json; charset=utf-8", body); }

bool ReadAllBody(Conn& cn, const Request& r, std::string& out, long long max) {
    if (r.length > max) return false;
    out.assign((size_t)r.length, '\0');
    size_t at = 0;
    while (cn.bodyLeft > 0) {
        int k = cn.Read(out.data() + at, (int)(out.size() - at));
        if (k <= 0) return false;
        at += (size_t)k;
    }
    return true;
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

bool ServerOn() { return g_thread != nullptr; }

SOCKET Listen(sockaddr_in a, int from, int to, int& port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
    for (int p = from; p < to; ++p) {
        a.sin_port = htons((u_short)p);
        if (bind(s, (sockaddr*)&a, sizeof(a)) == 0 && listen(s, 16) == 0) {
            port = p;
            return s;
        }
    }
    closesocket(s);
    return INVALID_SOCKET;
}

bool ServerStart(int& port, int& tlsPort, bool loopback) {
    if (g_thread) return true;
    if (!g_wsa) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) return false;
        g_wsa = true;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    SOCKET s = Listen(a, 8780, 8800, port);
    if (s == INVALID_SOCKET) return false;
    // https - на 8800-8819 (тот же сдвиг, что у http, - адрес не меняется между запусками)
    tlsPort = 0;
    g_listenTls = Listen(a, 8800 + (port - 8780), 8820, tlsPort);
    if (g_listenTls == INVALID_SOCKET) g_listenTls = Listen(a, 8800, 8820, tlsPort);
    g_listen = s;
    g_stop = false;
    g_thread = CreateThread(nullptr, 0, AcceptLoop, nullptr, 0, nullptr);
    if (!g_thread) {
        closesocket(s);
        if (g_listenTls != INVALID_SOCKET) closesocket(g_listenTls);
        g_listen = g_listenTls = INVALID_SOCKET;
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
    if (g_listenTls != INVALID_SOCKET) closesocket(g_listenTls);
    g_listen = g_listenTls = INVALID_SOCKET;
    for (int i = 0; i < 40 && g_conns > 0; ++i) Sleep(50);  // соединения видят g_stop и бросают передачу
    if (g_conns == 0) TlsReset();
}

}  // namespace phone::detail
