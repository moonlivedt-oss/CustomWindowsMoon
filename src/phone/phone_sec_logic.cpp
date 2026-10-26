// «Телефон рядом»: правила безопасности без сокетов и окон (под тестами) - ключи в пути, https для запомненных.
#include <cstdlib>
#include <string>
#include <vector>

#include "core/text.h"
#include "phone/phone.h"

namespace phone {

size_t MatchKey(const std::string& path, const std::string& token, const std::vector<std::string>& devices,
                int* device) {
    if (device) *device = -1;
    auto starts = [&](const std::string& base) {
        return !base.empty() && path.size() >= base.size() && path.compare(0, base.size(), base) == 0 &&
               (path.size() == base.size() || path[base.size()] == '/');
    };
    if (!token.empty() && starts("/" + token)) return token.size() + 1;
    for (size_t i = 0; i < devices.size(); ++i)
        if (!devices[i].empty() && starts("/d/" + devices[i])) {
            if (device) *device = (int)i;
            return devices[i].size() + 3;
        }
    return 0;
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

}  // namespace phone
