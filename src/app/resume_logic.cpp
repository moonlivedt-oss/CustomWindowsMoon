// «Продолжить, где остановился»: снимок строками, разбор VS Code, разница и текст карточки (без окон, под тестами).
#include <cwctype>
#include <ctime>

#include "app/resume.h"

namespace resume {

namespace {

std::wstring Low(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring LeafOf(const std::wstring& p) {
    std::wstring s = p;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    size_t cut = s.find_last_of(L"\\/");
    return cut == std::wstring::npos ? s : s.substr(cut + 1);
}

const wchar_t* KindName(Thing::Kind k) {
    switch (k) {
        case Thing::CODE:
            return L"code";
        case Thing::FOLDER:
            return L"folder";
        case Thing::UNREAL:
            return L"unreal";
        default:
            return L"app";
    }
}

// все строковые значения ключа "key": "..." после позиции from (простые строки без вложенных кавычек)
std::vector<std::wstring> Values(const std::wstring& json, const std::wstring& key, size_t from, size_t to) {
    std::vector<std::wstring> out;
    std::wstring k = L"\"" + key + L"\"";
    for (size_t at = json.find(k, from); at != std::wstring::npos && at < to; at = json.find(k, at + 1)) {
        size_t q1 = json.find(L'"', json.find(L':', at + k.size()) + 1);
        size_t q2 = q1 == std::wstring::npos ? std::wstring::npos : json.find(L'"', q1 + 1);
        if (q2 == std::wstring::npos || q2 > to) break;
        out.push_back(json.substr(q1 + 1, q2 - q1 - 1));
    }
    return out;
}

// конец объекта, начинающегося с «{» в позиции open (строки с фигурными скобками внутри не учитываем: в путях их нет)
size_t Close(const std::wstring& json, size_t open) {
    int depth = 0;
    for (size_t i = open; i < json.size(); ++i) {
        if (json[i] == L'{' || json[i] == L'[') ++depth;
        if (json[i] == L'}' || json[i] == L']') {
            if (--depth == 0) return i;
        }
    }
    return json.size();
}

}  // namespace

std::wstring Save(const Snapshot& s) {
    std::wstring o = L"when\t" + std::to_wstring(s.when) + L"\n";
    for (const Thing& t : s.things) o += std::wstring(KindName(t.kind)) + L"\t" + t.path + L"\t" + t.label + L"\n";
    return o;
}

bool Parse(const std::wstring& text, Snapshot& s) {
    s = {};
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t t1 = line.find(L'\t');
        if (t1 == std::wstring::npos) continue;
        std::wstring kind = line.substr(0, t1);
        if (kind == L"when") {
            s.when = _wtoi64(line.c_str() + t1 + 1);
            continue;
        }
        size_t t2 = line.find(L'\t', t1 + 1);
        Thing t;
        t.path = line.substr(t1 + 1, t2 == std::wstring::npos ? std::wstring::npos : t2 - t1 - 1);
        t.label = t2 == std::wstring::npos ? LeafOf(t.path) : line.substr(t2 + 1);
        t.kind = kind == L"code"     ? Thing::CODE
                 : kind == L"folder" ? Thing::FOLDER
                 : kind == L"unreal" ? Thing::UNREAL
                                     : Thing::APP;
        if (!t.path.empty()) s.things.push_back(t);
    }
    return s.when > 0;
}

std::wstring FromFileUri(const std::wstring& uri) {
    std::wstring u = uri;
    if (u.rfind(L"file:///", 0) == 0)
        u = u.substr(8);
    else if (u.rfind(L"file://", 0) == 0)
        u = L"\\\\" + u.substr(7);  // сетевой путь
    else
        return L"";
    std::wstring out;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == L'%' && i + 2 < u.size() && iswxdigit(u[i + 1]) && iswxdigit(u[i + 2])) {
            out += (wchar_t)wcstol(u.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += u[i] == L'/' ? L'\\' : u[i];
        }
    }
    // байты UTF-8 после раскодировки - собрать в символы (кириллица в пути)
    std::string bytes;
    bool utf = false;
    for (wchar_t c : out) {
        if (c > 0x7F && c < 0x100) utf = true;
        bytes += (char)(c & 0xFF);
    }
    if (utf) {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
        if (n > 0) {
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), w.data(), n);
            out = w;
        }
    }
    if (out.size() >= 2 && out[1] == L':') out[0] = (wchar_t)towupper(out[0]);
    return out;
}

std::vector<std::wstring> CodeFolders(const std::wstring& json, const std::vector<std::wstring>& titles) {
    std::vector<std::wstring> out;
    auto add = [&](const std::wstring& p) {
        if (p.empty()) return;
        for (const std::wstring& o : out)
            if (Low(o) == Low(p)) return;
        out.push_back(p);
    };
    size_t bw = json.find(L"\"backupWorkspaces\"");
    if (bw != std::wstring::npos) {
        size_t open = json.find(L'{', bw);
        size_t end = open == std::wstring::npos ? json.size() : Close(json, open);
        size_t folders = json.find(L"\"folders\"", bw);
        if (folders != std::wstring::npos && folders < end)
            for (const std::wstring& u : Values(json, L"folderUri", folders, end)) add(FromFileUri(u));
    }
    // папки из заголовков окон: «App.tsx - shop - Visual Studio Code» -> «shop» -> полный путь из известных VS Code
    std::vector<std::wstring> known;
    size_t pa = json.find(L"\"profileAssociations\"");
    if (pa != std::wstring::npos) {
        size_t ws = json.find(L"\"workspaces\"", pa);
        size_t open = ws == std::wstring::npos ? std::wstring::npos : json.find(L'{', ws);
        if (open != std::wstring::npos) {
            size_t end = Close(json, open);
            for (size_t q = json.find(L'"', open); q != std::wstring::npos && q < end;) {
                size_t q2 = json.find(L'"', q + 1);
                if (q2 == std::wstring::npos || q2 > end) break;
                std::wstring key = json.substr(q + 1, q2 - q - 1);
                if (key.rfind(L"file:", 0) == 0) known.push_back(FromFileUri(key));
                size_t next = json.find(L',', q2);  // за значением - следующая пара
                q = next == std::wstring::npos || next > end ? std::wstring::npos : json.find(L'"', next);
            }
        }
    }
    const std::wstring tail = L" - Visual Studio Code";
    for (const std::wstring& t : titles) {
        if (t.size() <= tail.size() || t.compare(t.size() - tail.size(), tail.size(), tail) != 0) continue;
        std::wstring head = t.substr(0, t.size() - tail.size());
        size_t dash = head.rfind(L" - ");
        std::wstring name = dash == std::wstring::npos ? head : head.substr(dash + 3);
        if (name.size() > 2 && name.front() == L'[') continue;  // «[Extension Development Host]»
        std::wstring hit;
        int hits = 0;
        for (const std::wstring& k : known)
            if (Low(LeafOf(k)) == Low(name)) hit = k, ++hits;
        if (hits == 1) add(hit);  // одно имя у двух папок - не угадываем
    }
    return out;
}

std::vector<Thing> Missing(const Snapshot& was, const std::vector<Thing>& now) {
    std::vector<Thing> out;
    for (const Thing& t : was.things) {
        bool open = false;
        for (const Thing& n : now) open = open || (n.kind == t.kind && Low(n.path) == Low(t.path));
        if (!open) out.push_back(t);
    }
    return out;
}

std::wstring Describe(const std::vector<Thing>& things, size_t maxLen) {
    std::wstring s;
    for (Thing::Kind k : {Thing::CODE, Thing::UNREAL, Thing::FOLDER, Thing::APP}) {
        std::wstring part;
        for (const Thing& t : things)
            if (t.kind == k) part += (part.empty() ? L"" : L", ") + t.label;
        if (part.empty()) continue;
        const wchar_t* head = k == Thing::CODE     ? L"VS Code: "
                              : k == Thing::UNREAL ? L"Unreal: "
                              : k == Thing::FOLDER ? L"папки: "
                                                   : L"программы: ";
        s += (s.empty() ? L"" : L"; ") + std::wstring(head) + part;
    }
    if (s.size() > maxLen) s = s.substr(0, maxLen - 3) + L"...";
    return s;
}

std::wstring When(LONGLONG then, LONGLONG now) {
    time_t a = (time_t)then, b = (time_t)now;
    tm ta{}, tb{};
    localtime_s(&ta, &a);
    localtime_s(&tb, &b);
    wchar_t hm[16];
    swprintf(hm, 16, L"%02d:%02d", ta.tm_hour, ta.tm_min);
    tm yb = tb;
    yb.tm_mday -= 1;
    mktime(&yb);
    if (ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday) return std::wstring(L"сегодня в ") + hm;
    if (ta.tm_year == yb.tm_year && ta.tm_yday == yb.tm_yday) return std::wstring(L"вчера в ") + hm;
    wchar_t d[32];
    swprintf(d, 32, L"%02d.%02d в ", ta.tm_mday, ta.tm_mon + 1);
    return d + std::wstring(hm);
}

}  // namespace resume
