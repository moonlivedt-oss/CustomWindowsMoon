#include "helpers/translate_logic.h"

#include "core/secrets.h"

namespace translate {

std::wstring Escape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'\\':
                out += L"\\\\";
                break;
            case L'\n':
                out += L"\\n";
                break;
            case L'\r':
                out += L"\\r";
                break;
            case L'\t':
                out += L"\\t";
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        wchar_t c = s[++i];
        out += c == L'n' ? L'\n' : c == L'r' ? L'\r' : c == L't' ? L'\t' : c;
    }
    return out;
}

HelperLine ParseHelperLine(const std::wstring& raw) {
    HelperLine r;
    std::wstring line = raw;
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
    size_t a = line.find(L'\t');
    std::wstring head = line.substr(0, a);
    std::wstring rest = a == std::wstring::npos ? L"" : line.substr(a + 1);
    if (head == L"READY") {
        r.kind = HelperLine::Ready;
        r.text = rest;
    } else if (head == L"FAIL") {
        r.kind = HelperLine::Fail;
        r.text = Unescape(rest);
    } else if (head == L"DONE") {
        r.kind = HelperLine::Done;
    } else if (head == L"OK" || head == L"ERR") {
        size_t b = rest.find(L'\t');
        if (b == std::wstring::npos) return r;
        r.kind = head == L"OK" ? HelperLine::Ok : HelperLine::Err;
        r.id = rest.substr(0, b);
        r.text = Unescape(rest.substr(b + 1));
    }
    return r;
}

std::wstring SourceOf(const std::wstring& text) {
    int cyr = 0, lat = 0;
    for (wchar_t c : text) {
        if (c >= 0x400 && c <= 0x52F) ++cyr;
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= 0xC0 && c <= 0x24F)) ++lat;
    }
    return cyr > lat ? L"ru" : L"en";
}

std::wstring TargetFor(const std::wstring& source) { return source == L"ru" ? L"en" : L"ru"; }

bool HasPair(const std::wstring& pairs, const std::wstring& source, const std::wstring& target) {
    std::wstring want = L"," + source + L">" + target + L",";
    return (L"," + pairs + L",").find(want) != std::wstring::npos;
}

bool ContainsSecret(const std::wstring& text) {
    if (!secrets::Kind(text).empty()) return true;
    // Kind смотрит текст целиком; внутри фразы токен и номер карты ищем по строкам и словам
    auto check = [&](wchar_t stop) {
        bool found = false;
        for (size_t at = 0; at < text.size() && !found;) {
            size_t end = stop == L'\n' ? text.find(L'\n', at) : text.find_first_of(L" \t\r\n", at);
            if (end == std::wstring::npos) end = text.size();
            if (end > at) found = !secrets::Kind(text.substr(at, end - at)).empty();
            at = end + 1;
        }
        return found;
    };
    return check(L'\n') || check(L' ');
}

namespace {

// Строка JSON с позиции открывающей кавычки; позиция сдвигается за закрывающую
bool ReadJsonString(const std::wstring& j, size_t& i, std::wstring& out) {
    if (i >= j.size() || j[i] != L'"') return false;
    out.clear();
    for (++i; i < j.size(); ++i) {
        wchar_t c = j[i];
        if (c == L'"') {
            ++i;
            return true;
        }
        if (c != L'\\') {
            out += c;
            continue;
        }
        if (++i >= j.size()) return false;
        c = j[i];
        switch (c) {
            case L'n':
                out += L'\n';
                break;
            case L'r':
                out += L'\r';
                break;
            case L't':
                out += L'\t';
                break;
            case L'b':
                out += L'\b';
                break;
            case L'f':
                out += L'\f';
                break;
            case L'u': {
                if (i + 4 >= j.size()) return false;
                unsigned v = 0;
                for (int k = 1; k <= 4; ++k) {
                    wchar_t h = j[i + k];
                    v = v * 16 + (h >= L'0' && h <= L'9'   ? h - L'0'
                                  : h >= L'a' && h <= L'f' ? h - L'a' + 10
                                  : h >= L'A' && h <= L'F' ? h - L'A' + 10
                                                           : 0);
                }
                out += (wchar_t)v;
                i += 4;
                break;
            }
            default:
                out += c;
        }
    }
    return false;
}

}  // namespace

bool ParseGtx(const std::wstring& j, std::wstring& out) {
    out.clear();
    size_t i = j.find_first_not_of(L" \r\n\t");
    if (i == std::wstring::npos || j[i] != L'[') return false;
    i = j.find_first_not_of(L" \r\n\t", i + 1);
    if (i == std::wstring::npos || j[i] != L'[') return false;  // null - перевода нет
    ++i;
    // первый массив: куски перевода; первая строка каждого куска - сам перевод, остальное пропускаем
    while (i < j.size()) {
        i = j.find_first_not_of(L" \r\n\t,", i);
        if (i == std::wstring::npos) return false;
        if (j[i] == L']') return true;
        if (j[i] != L'[') return false;
        size_t k = j.find_first_not_of(L" \r\n\t", i + 1);
        if (k == std::wstring::npos) return false;
        std::wstring part;
        if (j[k] == L'"') {
            if (!ReadJsonString(j, k, part)) return false;
            out += part;
        }
        // до конца этого куска: скобки считаем, строки пропускаем целиком
        int depth = 1;
        while (k < j.size() && depth > 0) {
            if (j[k] == L'"') {
                if (!ReadJsonString(j, k, part)) return false;
                continue;
            }
            if (j[k] == L'[') ++depth;
            if (j[k] == L']') --depth;
            ++k;
        }
        if (depth != 0) return false;
        i = k;
    }
    return false;
}

std::vector<std::wstring> SplitChunks(const std::wstring& text, size_t limit) {
    if (text.size() <= limit || limit < 2) return {text};
    std::vector<std::wstring> pieces;
    for (size_t at = 0; at <= text.size();) {
        size_t nl = text.find(L'\n', at);
        std::wstring para = text.substr(at, nl == std::wstring::npos ? nl : nl - at);
        while (para.size() > limit) {
            size_t cut = para.rfind(L". ", limit - 1);
            if (cut == std::wstring::npos || cut < limit / 2) cut = para.rfind(L' ', limit - 1);
            cut = cut == std::wstring::npos || cut < limit / 2 ? limit : cut + 1;
            pieces.push_back(para.substr(0, cut));
            para.erase(0, cut);
        }
        pieces.push_back(para);
        if (nl == std::wstring::npos) break;
        at = nl + 1;
    }
    std::vector<std::wstring> chunks;
    std::wstring cur;
    bool started = false;
    for (const std::wstring& p : pieces) {
        if (started && cur.size() + 1 + p.size() > limit) {
            chunks.push_back(cur);
            cur = p;
        } else {
            cur = started ? cur + L"\n" + p : p;
            started = true;
        }
    }
    if (started) chunks.push_back(cur);
    return chunks;
}

bool SplitBatch(const std::wstring& joined, size_t count, std::vector<std::wstring>& out) {
    out.clear();
    for (size_t at = 0;;) {
        size_t nl = joined.find(L'\n', at);
        out.push_back(joined.substr(at, nl == std::wstring::npos ? nl : nl - at));
        if (nl == std::wstring::npos) break;
        at = nl + 1;
    }
    return out.size() == count;
}

std::wstring HttpError(int status) {
    if (status == 429) return L"Google временно ограничил запросы, подождите минуту";
    if (status == 0) return L"Нет связи с Google";
    return L"Google ответил ошибкой " + std::to_wstring(status);
}

bool SameText(const std::wstring& a, const std::wstring& b) {
    auto norm = [](const std::wstring& s) {
        std::wstring out;
        for (wchar_t c : s) {
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
            if (c >= 0x410 && c <= 0x42F) c = (wchar_t)(c + 32);
            if (c == 0x401) c = 0x451;
            bool letter = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c >= 0xC0;
            if (letter) out += c;
        }
        return out;
    };
    return norm(a) == norm(b);
}

}  // namespace translate
