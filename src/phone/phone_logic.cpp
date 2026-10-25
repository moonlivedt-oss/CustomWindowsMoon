// Телефон рядом: разбор запросов, имена файлов, адреса и выбор пути - без сокетов и окон (под тестами).
#include <ctime>

#include "core/text.h"
#include "phone/phone.h"

namespace phone {

namespace {

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string LowerA(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string TrimA(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string Decode(const std::string& s, bool plus) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && Hex(s[i + 1]) >= 0 && Hex(s[i + 2]) >= 0) {
            out += (char)(Hex(s[i + 1]) * 16 + Hex(s[i + 2]));
            i += 2;
        } else if (plus && s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace

std::string UrlDecode(const std::string& s) { return Decode(s, true); }

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool ParseHead(const std::string& head, Request& r) {
    r = Request();
    size_t eol = head.find("\r\n");
    std::string first = head.substr(0, eol);
    size_t s1 = first.find(' '), s2 = first.rfind(' ');
    if (s1 == std::string::npos || s2 == s1 || first.compare(s2 + 1, 5, "HTTP/") != 0) return false;
    r.method = first.substr(0, s1);
    std::string target = first.substr(s1 + 1, s2 - s1 - 1);
    if (target.empty() || target[0] != '/') return false;
    size_t q = target.find('?');
    r.path = Decode(target.substr(0, q), false);
    if (q != std::string::npos) {
        std::string qs = target.substr(q + 1);
        size_t at = 0;
        while (at <= qs.size()) {
            size_t amp = qs.find('&', at);
            std::string part = qs.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
            if (!part.empty()) {
                size_t eq = part.find('=');
                r.query[UrlDecode(part.substr(0, eq))] = eq == std::string::npos ? "" : UrlDecode(part.substr(eq + 1));
            }
            if (amp == std::string::npos) break;
            at = amp + 1;
        }
    }
    size_t at = eol == std::string::npos ? head.size() : eol + 2;
    while (at < head.size()) {
        size_t e = head.find("\r\n", at);
        std::string line = head.substr(at, e == std::string::npos ? std::string::npos : e - at);
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon != std::string::npos) r.headers[LowerA(TrimA(line.substr(0, colon)))] = TrimA(line.substr(colon + 1));
        if (e == std::string::npos) break;
        at = e + 2;
    }
    auto cl = r.headers.find("content-length");
    if (cl != r.headers.end()) {
        long long n = 0;
        for (char c : cl->second) {
            if (c < '0' || c > '9' || n > (1ll << 50)) return false;
            n = n * 10 + (c - '0');
        }
        r.length = n;
    }
    return true;
}

std::wstring SafeName(const std::wstring& name) {
    std::wstring n = name;
    size_t slash = n.find_last_of(L"/\\");
    if (slash != std::wstring::npos) n = n.substr(slash + 1);
    std::wstring out;
    for (wchar_t c : n) {
        if (c < 32 || wcschr(L"<>:\"|?*", c)) c = L'_';
        out += c;
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(0, 1);
    if (out.size() > 120) {  // хвост с расширением сохраняем
        size_t dot = out.find_last_of(L'.');
        std::wstring ext = dot != std::wstring::npos && out.size() - dot <= 10 ? out.substr(dot) : L"";
        out = out.substr(0, 120 - ext.size()) + ext;
    }
    std::wstring stem = Lower(out.substr(0, out.find(L'.')));
    static const wchar_t* kDevices[] = {L"con",  L"prn",  L"aux",  L"nul",  L"com1", L"com2", L"com3", L"com4",
                                        L"com5", L"com6", L"com7", L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3",
                                        L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9"};
    for (const wchar_t* d : kDevices)
        if (stem == d) out = L"_" + out;
    if (out.empty() || out == L"_") out = L"файл";
    return out;
}

std::wstring DatedDir(const std::wstring& base, long long unixMs) {
    time_t t = unixMs > 0 ? (time_t)(unixMs / 1000) : time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t d[16];
    swprintf(d, 16, L"%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return base + L"\\" + d;
}

bool IsMedia(const std::wstring& name) {
    static const wchar_t* kExt[] = {L".jpg", L".jpeg", L".png", L".heic", L".heif", L".webp", L".gif",
                                    L".dng", L".mp4",  L".mov", L".3gp",  L".mkv",  L".webm"};
    for (const wchar_t* e : kExt)
        if (EndsWithI(name, e)) return true;
    return false;
}

std::string ContentType(const std::wstring& name) {
    static const struct {
        const wchar_t* ext;
        const char* type;
    } kTypes[] = {{L".jpg", "image/jpeg"},      {L".jpeg", "image/jpeg"},
                  {L".png", "image/png"},       {L".gif", "image/gif"},
                  {L".webp", "image/webp"},     {L".heic", "image/heic"},
                  {L".mp4", "video/mp4"},       {L".mov", "video/quicktime"},
                  {L".webm", "video/webm"},     {L".mp3", "audio/mpeg"},
                  {L".pdf", "application/pdf"}, {L".txt", "text/plain; charset=utf-8"},
                  {L".zip", "application/zip"}, {L".apk", "application/vnd.android.package-archive"}};
    for (const auto& t : kTypes)
        if (EndsWithI(name, t.ext)) return t.type;
    return "application/octet-stream";
}

bool ParseRange(const std::string& value, long long size, long long& from, long long& to) {
    std::string v = TrimA(value);
    if (v.compare(0, 6, "bytes=") != 0 || size <= 0 || v.find(',') != std::string::npos) return false;
    v = v.substr(6);
    size_t dash = v.find('-');
    if (dash == std::string::npos) return false;
    auto num = [](const std::string& s, long long& out) {
        if (s.empty()) return false;
        out = 0;
        for (char c : s) {
            if (c < '0' || c > '9' || out > (1ll << 50)) return false;
            out = out * 10 + (c - '0');
        }
        return true;
    };
    std::string a = TrimA(v.substr(0, dash)), b = TrimA(v.substr(dash + 1));
    long long x = 0, y = 0;
    if (a.empty()) {  // последние n байт
        if (!num(b, y) || y == 0) return false;
        from = y >= size ? 0 : size - y;
        to = size - 1;
        return true;
    }
    if (!num(a, x) || x >= size) return false;
    from = x;
    to = size - 1;
    if (!b.empty()) {
        if (!num(b, y) || y < x) return false;
        if (y < size) to = y;
    }
    return true;
}

std::string Json(const std::wstring& s) {
    std::string u = Narrow(s), out = "\"";
    for (unsigned char c : u) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 32) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else {
            out += (char)c;
        }
    }
    return out + "\"";
}

std::string Token(const unsigned char* bytes, size_t n) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        v = (v << 8) | bytes[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += abc[(v >> bits) & 63];
        }
    }
    if (bits > 0) out += abc[(v << (6 - bits)) & 63];
    return out;
}

bool PrivateIp(uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1) || (ip >> 16) == ((192u << 8) | 168);
}

bool HotspotIp(uint32_t ip) { return (ip >> 8) == ((192u << 16) | (168u << 8) | 137u); }

uint32_t PickAddress(const std::vector<Adapter>& list) {
    uint32_t best = 0;
    int bestScore = 0;
    for (const Adapter& a : list) {
        if (a.virt || !PrivateIp(a.ip)) continue;
        int score = HotspotIp(a.ip) ? 4 : a.type == 71 ? 3 : a.type == 6 ? 2 : 1;
        if (score > bestScore) bestScore = score, best = a.ip;
    }
    return best;
}

std::wstring IpText(uint32_t ip) {
    return std::to_wstring(ip >> 24) + L"." + std::to_wstring((ip >> 16) & 255) + L"." +
           std::to_wstring((ip >> 8) & 255) + L"." + std::to_wstring(ip & 255);
}

std::vector<Way> Ways(bool lan, bool bluetooth, long long bytes) {
    bool small = bluetooth && bytes > 0 && bytes <= kBluetoothMax;
    std::vector<Way> w;
    if (!lan && small) w.push_back(WAY_BLUETOOTH);  // сети нет совсем, а файл мелкий - быстрее всего так
    w.push_back(WAY_PHONE_HOTSPOT);                 // телефон с собой всегда, а раздачу ноутбука не везде включить
    w.push_back(WAY_PC_HOTSPOT);
    if (lan && small) w.push_back(WAY_BLUETOOTH);
    return w;
}

}  // namespace phone
