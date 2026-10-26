// «Телефон рядом», приложение на телефоне: разбор уведомлений, SMS и звонков, код из SMS, MAC и адрес для
// Wake-on-LAN, защита от потока карточек (под тестами).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <string>

#include "core/text.h"
#include "phone/phone.h"

namespace phone {

std::map<std::string, std::string> ParseForm(const std::string& body) {
    std::map<std::string, std::string> f;
    size_t i = 0;
    while (i <= body.size()) {
        size_t amp = body.find('&', i);
        if (amp == std::string::npos) amp = body.size();
        std::string part = body.substr(i, amp - i);
        size_t eq = part.find('=');
        if (!part.empty()) {
            std::string k = UrlDecode(part.substr(0, eq)),
                        v = eq == std::string::npos ? "" : UrlDecode(part.substr(eq + 1));
            if (!k.empty() && !f.count(k)) f[k] = v;  // повтор поля - первое
        }
        i = amp + 1;
    }
    return f;
}

std::wstring CleanLine(const std::wstring& s, size_t max) {
    std::wstring out;
    for (wchar_t c : s) {
        if ((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200B || c == 0xFEFF)
            continue;  // смена направления текста и невидимые: не дать подделать строку
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || c == 0x2028 || c == 0x2029 || c == 0xA0) c = L' ';
        if (c == L' ' && (out.empty() || out.back() == L' ')) continue;
        out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (max && out.size() > max) {
        size_t cut = max - 1;
        if (cut && out[cut - 1] >= 0xD800 && out[cut - 1] <= 0xDBFF) --cut;  // не рвать суррогатную пару
        out = out.substr(0, cut);
        while (!out.empty() && out.back() == L' ') out.pop_back();
        out += L'…';
    }
    return out;
}

bool ParseNotice(const std::string& body, Notice& n) {
    n = Notice();
    std::map<std::string, std::string> f = ParseForm(body);
    auto get = [&](const char* k) {
        auto it = f.find(k);
        return it == f.end() ? std::wstring() : Widen(it->second);
    };
    n.kind = f["kind"];
    if (n.kind != "note" && n.kind != "sms" && n.kind != "call" && n.kind != "missed" && n.kind != "end") return false;
    n.app = CleanLine(get("app"), 60);
    n.title = CleanLine(get("title"), 120);
    n.text = CleanLine(get("text"), 400);
    n.number = CleanLine(get("num"), 40);
    if (n.kind != "end" && n.title.empty() && n.text.empty() && n.number.empty()) return false;
    std::string id;
    for (char c : f["id"])
        if (id.size() < 120 && (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == ':' || c == '|'))
            id += c;
    n.key = n.kind + "|" +
            (id.empty() ? Narrow(n.app) + "|" + Narrow(n.title) + "|" + Narrow(n.text) + "|" + Narrow(n.number) : id);
    return true;
}

namespace {

bool Digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool StartsAt(const std::wstring& s, size_t at, const wchar_t* w) { return s.compare(at, wcslen(w), w) == 0; }

}  // namespace

std::wstring SmsCode(const std::wstring& text) {
    std::wstring low = Lower(text);
    struct Cand {
        size_t at;
        std::wstring code;
    };
    std::vector<Cand> found;
    for (size_t i = 0; i < low.size(); ++i) {
        if (!Digit(low[i]) || (i && Digit(low[i - 1]))) continue;
        size_t j = i;
        while (j < low.size() && Digit(low[j])) ++j;
        std::wstring code = low.substr(i, j - i);
        // «123-456», «123 456» - один код
        if (code.size() == 3 && j + 4 <= low.size() && (low[j] == L'-' || low[j] == L' ') && Digit(low[j + 1]) &&
            Digit(low[j + 2]) && Digit(low[j + 3]) && (j + 4 == low.size() || !Digit(low[j + 4]))) {
            code += low.substr(j + 1, 3);
            j += 4;
        }
        size_t at = i;
        i = j;
        if (code.size() < 4 || code.size() > 8) continue;
        wchar_t before = at ? low[at - 1] : L' ', after = j < low.size() ? low[j] : L' ';
        if (before == L'+' || IsCharAlphaW(before) || IsCharAlphaW(after)) continue;  // телефон, «ID12345»
        if (before == L'$' || before == L'€' || before == L'₽') continue;
        if ((before == L'.' || before == L',') && at > 1 && Digit(low[at - 2])) continue;  // дробная часть
        if ((after == L'.' || after == L',' || after == L':') && j + 1 < low.size() && Digit(low[j + 1]))
            continue;  // 1500.00, 12:30
        size_t k = j;
        while (k < low.size() && low[k] == L' ') ++k;
        if (k < low.size() && (low[k] == L'р' || low[k] == L'₽' || low[k] == L'%' || low[k] == L'$' || low[k] == L'€' ||
                               StartsAt(low, k, L"rub") || StartsAt(low, k, L"usd")))
            continue;  // деньги
        found.push_back({at, code});
    }
    if (found.empty()) return L"";
    std::vector<size_t> keys;
    for (const wchar_t* w : {L"код", L"code", L"парол", L"pin", L"password", L"kod"})
        for (size_t p = low.find(w); p != std::wstring::npos; p = low.find(w, p + 1)) keys.push_back(p);
    if (keys.empty()) return found.size() == 1 ? found[0].code : L"";
    // обычно «код 1234»: ближайший после слова; нет такого - ближайший перед ним («552913 - your code»)
    const Cand* best = nullptr;
    size_t bestD = ~(size_t)0;
    for (int after = 1; after >= 0 && !best; --after)
        for (const Cand& c : found)
            for (size_t k : keys) {
                if ((c.at > k) != (after == 1)) continue;
                size_t d = c.at > k ? c.at - k : k - c.at;
                if (d < bestD) bestD = d, best = &c;
            }
    return best ? best->code : L"";
}

std::string MacText(const unsigned char* b, size_t n) {
    if (n != 6) return "";
    bool zero = std::all_of(b, b + n, [](unsigned char c) { return c == 0; });
    if (zero) return "";
    char s[18];
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    return s;
}

uint32_t BroadcastOf(uint32_t ip, int prefix) {
    if (prefix <= 0 || prefix > 32) return 0xFFFFFFFFu;
    uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix);
    return (ip & mask) | ~mask;
}

bool NoticeGate::Pass(const std::string& key, unsigned long long now) {
    recent_.erase(
        std::remove_if(recent_.begin(), recent_.end(), [&](const auto& r) { return now - r.second >= 60000; }),
        recent_.end());
    for (const auto& r : recent_)
        if (r.first == key && now - r.second < 5000) return false;
    if (recent_.size() >= 20) return false;
    recent_.push_back({key, now});
    return true;
}

}  // namespace phone
