// Права расширений браузеров по manifest.json (И35): что расширение может - без полного разбора JSON, по массивам
// permissions / optional_permissions / host_permissions и matches у content_scripts. Под тестами.
#include <string>
#include <vector>

#include "tools/tools.h"

namespace tools {

namespace {

// все строки из массивов "key": [ ... ] (каждое вхождение ключа, вложенность - любая)
std::vector<std::string> JsonArrays(const std::string& js, const char* key) {
    std::vector<std::string> out;
    std::string k = std::string("\"") + key + "\"";
    for (size_t a = js.find(k); a != std::string::npos; a = js.find(k, a + 1)) {
        size_t c = js.find_first_not_of(" \t\r\n", a + k.size());
        if (c == std::string::npos || js[c] != ':') continue;
        size_t b = js.find_first_not_of(" \t\r\n", c + 1);
        if (b == std::string::npos || js[b] != '[') continue;
        size_t end = js.find(']', b);
        if (end == std::string::npos) break;
        for (size_t q = js.find('"', b); q != std::string::npos && q < end; q = js.find('"', q + 1)) {
            size_t e = js.find('"', q + 1);
            if (e == std::string::npos || e > end) break;
            out.push_back(js.substr(q + 1, e - q - 1));
            q = e;
        }
    }
    return out;
}

bool AllSitesPattern(const std::string& p) {
    return p == "<all_urls>" || p == "*://*/*" || p == "http://*/*" || p == "https://*/*" || p == "*://*/" ||
           p == "file:///*";
}

}  // namespace

int ExtRightsOf(const std::string& manifest) {
    int r = 0;
    std::vector<std::string> all;
    for (const char* k : {"permissions", "optional_permissions", "host_permissions", "matches"}) {
        std::vector<std::string> v = JsonArrays(manifest, k);
        all.insert(all.end(), v.begin(), v.end());
    }
    for (const std::string& p : all) {
        if (AllSitesPattern(p)) r |= EXT_ALL_SITES;
        if (p == "clipboardRead") r |= EXT_CLIPBOARD;
        if (p == "nativeMessaging") r |= EXT_NATIVE;
        if (p == "proxy") r |= EXT_PROXY;
        if (p == "debugger") r |= EXT_DEBUGGER;
        if (p == "cookies") r |= EXT_COOKIES;
        if (p == "history") r |= EXT_HISTORY;
        if (p == "management") r |= EXT_MANAGEMENT;
        if (p == "webRequestBlocking" || p == "declarativeNetRequestWithHostAccess") r |= EXT_REQUESTS;
    }
    return r;
}

std::wstring ExtRightsText(int r) {
    struct Name {
        int bit;
        const wchar_t* text;
    } names[] = {{EXT_ALL_SITES, L"читает и меняет все сайты"},
                 {EXT_CLIPBOARD, L"читает буфер обмена"},
                 {EXT_NATIVE, L"говорит с программами на компьютере"},
                 {EXT_PROXY, L"меняет прокси браузера"},
                 {EXT_DEBUGGER, L"подключается как отладчик к вкладкам"},
                 {EXT_COOKIES, L"читает куки (входы на сайты)"},
                 {EXT_HISTORY, L"видит историю"},
                 {EXT_MANAGEMENT, L"управляет другими расширениями"},
                 {EXT_REQUESTS, L"перехватывает запросы"}};
    std::wstring s;
    for (const Name& n : names)
        if (r & n.bit) s += (s.empty() ? L"" : L", ") + std::wstring(n.text);
    return s;
}

// Насколько много может: все сайты - основа; вместе с буфером, куками, отладчиком, прокси или связью с программами -
// уже доступ к паролям и входам.
int ExtRisk(int r) {
    // отладчик подключается к любой вкладке - это тот же доступ ко всем сайтам
    bool wide = r & (EXT_ALL_SITES | EXT_DEBUGGER);
    int k = wide ? 2 : 0;
    for (int bit : {EXT_CLIPBOARD, EXT_NATIVE, EXT_PROXY, EXT_COOKIES, EXT_MANAGEMENT})
        if (r & bit) k += wide ? 2 : 1;
    if (r & (EXT_HISTORY | EXT_REQUESTS)) k += 1;
    return k;
}

}  // namespace tools
