// «Телефон рядом», «Поделиться - на ПК»: разбор multipart/form-data потоком (тело приходит кусками любого размера,
// граница может разрезаться между кусками) и ссылка из того, чем поделились (под тестами).
#include <cctype>

#include "phone/phone.h"

namespace phone {

namespace {

std::string LowerA(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string TrimA(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

std::string Pct(const std::string& s) {  // %XX без «+ - пробел» (filename*)
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        int h = i + 2 < s.size() && s[i] == '%' ? Hex(s[i + 1]) : -1, l = h >= 0 ? Hex(s[i + 2]) : -1;
        if (h >= 0 && l >= 0) {
            out += (char)(h * 16 + l);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// параметры заголовка: «form-data; name="a"; filename="b"» -> {name: a, filename: b}; значения в кавычках с \"
std::map<std::string, std::string> Params(const std::string& v) {
    std::map<std::string, std::string> out;
    size_t i = v.find(';');
    while (i != std::string::npos && i < v.size()) {
        ++i;
        size_t eq = v.find('=', i);
        if (eq == std::string::npos) break;
        std::string key = LowerA(TrimA(v.substr(i, eq - i))), val;
        size_t j = eq + 1;
        while (j < v.size() && (v[j] == ' ' || v[j] == '\t')) ++j;
        if (j < v.size() && v[j] == '"') {
            for (++j; j < v.size() && v[j] != '"'; ++j) {
                if (v[j] == '\\' && j + 1 < v.size()) ++j;
                val += v[j];
            }
            i = v.find(';', j);
        } else {
            size_t e = v.find(';', j);
            val = TrimA(v.substr(j, e == std::string::npos ? std::string::npos : e - j));
            i = e;
        }
        out[key] = val;
    }
    return out;
}

bool Space(char c) { return (unsigned char)c <= ' '; }

bool StartsHttp(const std::string& s, size_t at) {
    std::string head = LowerA(s.substr(at, 8));
    return head.compare(0, 7, "http://") == 0 || head.compare(0, 8, "https://") == 0;
}

}  // namespace

std::string Boundary(const std::string& contentType) {
    if (LowerA(TrimA(contentType.substr(0, contentType.find(';')))) != "multipart/form-data") return "";
    std::string b = Params(contentType)["boundary"];
    // RFC 2046: 1-70 знаков; управляющих и переводов строк быть не может
    if (b.empty() || b.size() > 70) return "";
    for (char c : b)
        if ((unsigned char)c < ' ' || c == 127) return "";
    return b;
}

Multipart::Multipart(const std::string& boundary) : delim_("\r\n--" + boundary), buf_("\r\n") {}

bool Multipart::Feed(const char* p, size_t n) {
    if (state_ == BAD) return false;
    if (state_ == DONE) return true;  // эпилог - мимо
    buf_.append(p, n);
    for (;;) {
        if (state_ == PREAMBLE || state_ == BODY) {
            size_t pos = buf_.find(delim_);
            if (pos == std::string::npos) {
                size_t keep = delim_.size() - 1;  // хвост может оказаться началом границы
                if (buf_.size() > keep) {
                    size_t give = buf_.size() - keep;
                    if (state_ == BODY && onData && !onData(buf_.data(), give)) return (state_ = BAD), false;
                    buf_.erase(0, give);
                }
                return true;
            }
            if (state_ == BODY) {
                if (pos && onData && !onData(buf_.data(), pos)) return (state_ = BAD), false;
                if (onEnd && !onEnd()) return (state_ = BAD), false;
            }
            buf_.erase(0, pos + delim_.size());
            state_ = AFTER;
        } else if (state_ == AFTER) {
            size_t sp = 0;
            while (sp < buf_.size() && (buf_[sp] == ' ' || buf_[sp] == '\t')) ++sp;  // «transport padding»
            buf_.erase(0, sp);
            if (buf_.size() < 2) return true;
            if (buf_.compare(0, 2, "--") == 0) {
                buf_.clear();
                state_ = DONE;
                return true;
            }
            if (buf_.compare(0, 2, "\r\n") != 0) return (state_ = BAD), false;
            state_ = HEADERS;  // \r\n оставляем: заголовки ищутся как \r\n...\r\n\r\n
        } else if (state_ == HEADERS) {
            size_t pos = buf_.find("\r\n\r\n");
            if (pos == std::string::npos) {
                if (buf_.size() > 16384) return (state_ = BAD), false;
                return true;
            }
            FormPart part;
            std::string head = buf_.substr(2, pos >= 2 ? pos - 2 : 0);
            buf_.erase(0, pos + 4);
            size_t at = 0;
            while (at < head.size()) {
                size_t e = head.find("\r\n", at);
                std::string line = head.substr(at, e == std::string::npos ? std::string::npos : e - at);
                at = e == std::string::npos ? head.size() : e + 2;
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = LowerA(TrimA(line.substr(0, colon))), value = TrimA(line.substr(colon + 1));
                if (name == "content-type") {
                    part.type = LowerA(value);
                } else if (name == "content-disposition") {
                    std::map<std::string, std::string> pr = Params(value);
                    part.file = pr.count("filename") || pr.count("filename*");  // до pr[...]: он добавляет ключ
                    part.name = pr["name"];
                    part.filename = pr["filename"];
                    // браузер пишет «"», CR и LF в имени как %22, %0D, %0A
                    for (const char* esc : {"%22", "%0D", "%0A"})
                        for (size_t k; (k = part.filename.find(esc)) != std::string::npos;)
                            part.filename.replace(k, 3, 1, (char)(Hex(esc[1]) * 16 + Hex(esc[2])));
                    std::string star = pr["filename*"];  // RFC 5987: UTF-8''%D1%84...
                    if (LowerA(star.substr(0, 7)) == "utf-8''") part.filename = Pct(star.substr(7));
                }
            }
            if (onBegin && !onBegin(part)) return (state_ = BAD), false;
            state_ = BODY;
        } else {
            return state_ != BAD;
        }
    }
}

std::string ShareLink(const std::string& url, const std::string& text) {
    std::string u = TrimA(url);
    if (!u.empty() && StartsHttp(u, 0)) {
        bool clean = true;
        for (char c : u) clean = clean && !Space(c);
        if (clean) return u;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        if ((i && !Space(text[i - 1]) && text[i - 1] != '(' && text[i - 1] != '"') || !StartsHttp(text, i)) continue;
        size_t e = i;
        while (e < text.size() && !Space(text[e])) ++e;
        std::string link = text.substr(i, e - i);
        while (!link.empty() && std::string(".,;:!?)\"'").find(link.back()) != std::string::npos) link.pop_back();
        if (link.find("://") + 3 < link.size()) return link;
    }
    return "";
}

}  // namespace phone
