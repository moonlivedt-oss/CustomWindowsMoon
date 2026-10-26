// «Телефон рядом»: правила безопасности без сокетов и окон (под тестами) - ключи в пути, https для запомненных.
#include <string>
#include <vector>

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

}  // namespace phone
