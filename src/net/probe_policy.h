#pragma once

#include <string>

namespace net {

// Ответы контрольных адресов. Страница входа/редирект/произвольный HTTP 200 не доказывают доступ в интернет.
inline bool ConnectivityReply(unsigned endpoint, int status, const std::string& body) {
    if (endpoint == 0) return status == 204 && body.empty();
    if (status != 200) return false;
    if (endpoint == 1) {
        const std::string lines = "\n" + body;
        auto ip = lines.find("\nip=");
        auto loc = lines.find("\nloc=");
        return ip != std::string::npos && ip + 4 < lines.size() && lines[ip + 4] != '\n' &&
               loc != std::string::npos && loc + 5 < lines.size();
    }
    if (endpoint != 2) return false;
    auto end = body.find_last_not_of("\r\n \t");
    return end != std::string::npos && body.substr(0, end + 1) == "Microsoft Connect Test";
}

}  // namespace net
