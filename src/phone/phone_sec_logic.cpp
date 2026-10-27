// «Телефон рядом»: правила безопасности без сокетов и окон (под тестами) - ключи в пути, https для запомненных.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include "core/text.h"
#include "phone/phone.h"

namespace phone {

bool SameKey(const std::string& got, const std::string& key) {
    if (key.empty()) return false;
    // время зависит только от длины настоящего ключа: по задержке ответа ключ не подобрать по букве
    unsigned char diff = got.size() != key.size();
    for (size_t i = 0; i < key.size(); ++i) diff |= (unsigned char)(key[i] ^ (i < got.size() ? got[i] : 0));
    return diff == 0;
}

size_t MatchKey(const std::string& path, const std::string& token, const std::vector<std::string>& devices,
                int* device) {
    if (device) *device = -1;
    auto segment = [&](size_t from) {
        size_t e = path.find('/', from);
        return path.substr(from, e == std::string::npos ? std::string::npos : e - from);
    };
    if (path.compare(0, 3, "/d/") == 0) {  // запомненный телефон: сверить со всеми, без выхода на первом совпадении
        std::string got = segment(3);
        int found = -1;
        for (size_t i = 0; i < devices.size(); ++i)
            if (SameKey(got, devices[i]) && found < 0) found = (int)i;
        if (found < 0) return 0;
        if (device) *device = found;
        return got.size() + 3;
    }
    if (path.empty() || path[0] != '/') return 0;
    std::string got = segment(1);
    return SameKey(got, token) ? got.size() + 1 : 0;
}

std::string HttpsLocation(const std::string& host, int tlsPort, const std::string& path) {
    if (tlsPort <= 0 || tlsPort > 65535 || path.empty() || path[0] != '/') return "";
    std::string h = host;
    if (!h.empty() && h[0] == '[') return "";  // IPv6 сервер не слушает
    size_t colon = h.find(':');
    if (colon != std::string::npos) h.resize(colon);
    if (h.empty() || h.size() > 253) return "";
    for (char c : h)  // только имя или IPv4: Host из запроса не должен добавить в ответ ничего своего
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return "";
    for (char c : path)
        if ((unsigned char)c <= ' ' || c == 0x7F) return "";
    return "https://" + h + ":" + std::to_string(tlsPort) + path;
}

std::wstring CaFingerprint(const std::string& der) {
    unsigned char hash[32];
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (der.empty() || BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return L"";
    bool ok = BCryptHash(alg, nullptr, 0, (PUCHAR)der.data(), (ULONG)der.size(), hash, sizeof(hash)) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return L"";
    static const wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring out;
    for (int i = 0; i < 12; ++i) {
        if (i && i % 2 == 0) out += L' ';
        out += kHex[hash[i] >> 4];
        out += kHex[hash[i] & 15];
    }
    return out;
}

bool ControlPath(const std::string& rest) {
    // ещё файлы ПК (/fs: открыть, переименовать, в Корзину), ссылка в браузер ПК (/open) и пульт (/cmd: сон,
    // блокировка, экран)
    static const char* const kControl[] = {"/in",   "/type", "/combo",   "/tap", "/screen", "/mons",
                                           "/wins", "/win",  "/ringrun", "/fs",  "/open",   "/cmd"};
    for (const char* c : kControl)
        if (rest == c) return true;
    return false;
}

uint32_t BindAddress(uint32_t ip, bool loopback, const std::wstring& publicNet, const std::wstring& allowed) {
    if (loopback) return 0x7F000001;
    if (!ip || (!publicNet.empty() && publicNet != allowed)) return 0;
    return ip;
}

bool TokenIpAllowed(std::vector<uint32_t>& allowed, uint32_t ip) {
    if (allowed.empty()) {
        allowed.push_back(ip);  // первый, кто открыл страницу по QR, - тот, кто его снял
        return true;
    }
    return std::find(allowed.begin(), allowed.end(), ip) != allowed.end();
}

// ---- запомненные телефоны ----

std::vector<DeviceRec> ParseDevices(const std::string& text) {
    std::vector<DeviceRec> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        std::string line = text.substr(at, e == std::string::npos ? std::string::npos : e - at);
        at = e == std::string::npos ? text.size() : e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        for (size_t p = 0;;) {
            size_t t = line.find('\t', p);
            f.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) break;
            p = t + 1;
        }
        DeviceRec d;
        d.key = f[0];
        if (!UploadId(d.key)) continue;
        bool dup = false;
        for (const DeviceRec& o : out) dup = dup || o.key == d.key;
        if (dup) continue;
        if (f.size() > 1) d.name = CleanLine(Widen(f[1]), 60);
        if (f.size() > 2) d.added = atoll(f[2].c_str());
        d.seen = f.size() > 3 ? atoll(f[3].c_str()) : d.added;
        if (f.size() > 4) d.control = f[4] == "1";
        out.push_back(d);
    }
    return out;
}

std::string DevicesText(const std::vector<DeviceRec>& list) {
    std::string out;
    for (const DeviceRec& d : list)
        out += d.key + "\t" + Narrow(CleanLine(d.name, 60)) + "\t" + std::to_string(d.added) + "\t" +
               std::to_string(d.seen) + "\t" + (d.control ? "1" : "0") + "\n";
    return out;
}

bool DeviceStale(const DeviceRec& d, long long now) {
    long long last = d.seen > d.added ? d.seen : d.added;
    return last > 0 && now - last > kDeviceDays * 86400;
}

// ---- тормоз неверных ключей ----

bool BadGate::Banned(uint32_t ip, unsigned long long now) const {
    auto it = m_.find(ip);
    return it != m_.end() && it->second.until > now;
}

unsigned BadGate::Bad(uint32_t ip, unsigned long long now, unsigned long long* banMs) {
    if (banMs) *banMs = 0;
    if (m_.size() > 4096) {  // перебор адресов: забыть тех, кто давно затих и не в бане
        for (auto it = m_.begin(); it != m_.end();)
            it = it->second.until <= now && now - it->second.last > kForgetMs ? m_.erase(it) : std::next(it);
        if (m_.size() > 4096) m_.clear();
    }
    Rec& r = m_[ip];
    if (r.last && now - r.last > kForgetMs) r.count = 0;
    r.last = now;
    ++r.count;
    if (r.count >= kBanAt) {  // бан растёт с каждым разом: 5 минут, 10, 20... до суток
        ++r.bans;
        unsigned long long ms = kBanMs << std::min(r.bans - 1, 8);
        if (ms > 24ull * 3600 * 1000) ms = 24ull * 3600 * 1000;
        r.until = now + ms;
        r.count = 0;
        if (banMs) *banMs = ms;
        return 0;
    }
    if (r.count <= kFree) return 0;  // опечатка в закладке - без задержки
    unsigned d = 250u << std::min(r.count - kFree - 1, 4);
    return d > kMaxDelayMs ? kMaxDelayMs : d;
}

}  // namespace phone
