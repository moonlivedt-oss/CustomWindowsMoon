#include "desk/lyrics.h"

#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>

#include "core/text.h"

namespace lyrics {

namespace {

// --- самый маленький JSON: только то, что отдаёт lrclib -------------------------------------

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;  // UTF-8
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* Field(const char* k) const {
        for (auto& f : fields)
            if (f.first == k) return &f.second;
        return nullptr;
    }
};

struct Parser {
    const std::string& t;
    size_t i = 0;
    void Ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    static void Utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += (char)cp;
        } else if (cp < 0x800) {
            o += (char)(0xC0 | cp >> 6);
            o += (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            o += (char)(0xE0 | cp >> 12);
            o += (char)(0x80 | (cp >> 6 & 63));
            o += (char)(0x80 | (cp & 63));
        } else {
            o += (char)(0xF0 | cp >> 18);
            o += (char)(0x80 | (cp >> 12 & 63));
            o += (char)(0x80 | (cp >> 6 & 63));
            o += (char)(0x80 | (cp & 63));
        }
    }
    std::string String() {
        std::string o;
        ++i;  // "
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') {
                o += c;
                continue;
            }
            if (i >= t.size()) break;
            char e = t[i++];
            switch (e) {
                case 'n':
                    o += '\n';
                    break;
                case 't':
                    o += '\t';
                    break;
                case 'r':
                    break;
                case 'b':
                case 'f':
                    break;
                case 'u': {
                    unsigned cp = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        unsigned lo = (unsigned)strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
                        i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    Utf8(o, cp);
                    break;
                }
                default:
                    o += e;
            }
        }
        ++i;
        return o;
    }
    Json Value(int depth = 0) {
        Json v;
        Ws();
        if (i >= t.size() || depth > 32) return v;
        char c = t[i];
        if (c == '{') {
            v.type = Json::Obj;
            ++i;
            for (;;) {
                Ws();
                if (i >= t.size() || t[i] == '}') {
                    ++i;
                    break;
                }
                if (t[i] == ',') {
                    ++i;
                    continue;
                }
                std::string k = String();
                Ws();
                if (i < t.size() && t[i] == ':') ++i;
                v.fields.push_back({k, Value(depth + 1)});
            }
        } else if (c == '[') {
            v.type = Json::Arr;
            ++i;
            for (;;) {
                Ws();
                if (i >= t.size() || t[i] == ']') {
                    ++i;
                    break;
                }
                if (t[i] == ',') {
                    ++i;
                    continue;
                }
                v.items.push_back(Value(depth + 1));
            }
        } else if (c == '"') {
            v.type = Json::Str;
            v.s = String();
        } else if (c == 't' || c == 'f') {
            v.type = Json::Bool;
            v.b = c == 't';
            i += v.b ? 4 : 5;
        } else if (c == 'n') {
            i += 4;
        } else {
            v.type = Json::Num;
            char* end = nullptr;
            v.n = strtod(t.c_str() + i, &end);
            i = end ? (size_t)(end - t.c_str()) : i + 1;
        }
        return v;
    }
};

// --- сеть ----------------------------------------------------------------------------------

std::wstring Encode(const std::wstring& s) {
    std::string u = Narrow(s);
    std::wstring o;
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (wchar_t)c;
        } else {
            wchar_t b[4];
            swprintf(b, 4, L"%%%02X", c);
            o += b;
        }
    }
    return o;
}

bool HttpGet(const std::wstring& path, std::string& body, bool& offline) {
    HINTERNET s = WinHttpOpen(L"RadialMenu/1.0 (desktop utility)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 4000, 4000, 6000, 8000);
    HINTERNET c = WinHttpConnect(s, L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    bool ok = r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr);
    if (!ok) offline = true;
    DWORD code = 0, sz = sizeof(code);
    if (ok)
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
    ok = ok && code == 200;
    while (ok) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
        body.append(chunk.data(), got);
        if (body.size() > 4u * 1024 * 1024) break;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// Название без «(Official Video)», «[Lyrics]», «feat. ...»: так lrclib находит чаще.
std::wstring Clean(std::wstring s) {
    for (wchar_t open : {L'(', L'['}) {
        wchar_t close = open == L'(' ? L')' : L']';
        size_t a;
        while ((a = s.find(open)) != std::wstring::npos) {
            size_t b = s.find(close, a);
            s.erase(a, b == std::wstring::npos ? std::wstring::npos : b - a + 1);
        }
    }
    std::wstring low = Lower(s);
    for (const wchar_t* f : {L" feat.", L" ft.", L" feat "}) {
        size_t p = low.find(f);
        if (p != std::wstring::npos) {
            s.resize(p);
            low.resize(p);
        }
    }
    return Trim(s);
}

std::vector<Line> ParseLrc(const std::wstring& text) {
    std::vector<Line> out;
    std::wstringstream ss(text);
    std::wstring row;
    while (std::getline(ss, row)) {
        std::vector<int64_t> times;
        size_t p = 0;
        while (p < row.size() && row[p] == L'[') {
            size_t e = row.find(L']', p);
            if (e == std::wstring::npos) break;
            int m = 0;
            double sec = 0;
            if (swscanf(row.substr(p + 1, e - p - 1).c_str(), L"%d:%lf", &m, &sec) == 2)
                times.push_back((int64_t)((m * 60 + sec) * 10000000));
            p = e + 1;
        }
        std::wstring t = Trim(row.substr(p));
        for (int64_t at : times) out.push_back({at, t});
    }
    std::stable_sort(out.begin(), out.end(), [](const Line& a, const Line& b) { return a.at < b.at; });
    return out;
}

Lyrics Pick(const Json& arr, int dur) {
    Lyrics best;
    best.state = kMissing;
    int bestScore = -1;
    for (const Json& o : arr.items) {
        if (o.type != Json::Obj) continue;
        const Json* d = o.Field("duration");
        const Json* syn = o.Field("syncedLyrics");
        const Json* pl = o.Field("plainLyrics");
        const Json* ins = o.Field("instrumental");
        bool close = !dur || (d && d->type == Json::Num && std::fabs(d->n - dur) <= 4);
        bool hasSyn = syn && syn->type == Json::Str && !syn->s.empty();
        bool hasPl = pl && pl->type == Json::Str && !pl->s.empty();
        bool isIns = ins && ins->type == Json::Bool && ins->b;
        int score = (hasSyn ? 4 : hasPl ? 2 : isIns ? 1 : 0) + (close ? 8 : 0);
        if (score <= bestScore || score == 0 || score == 8) continue;
        bestScore = score;
        best = Lyrics();
        if (hasSyn) {
            best.state = kSynced;
            best.lines = ParseLrc(Widen(syn->s));
        } else if (hasPl) {
            best.state = kPlain;
            std::wstringstream ss(Widen(pl->s));
            std::wstring row;
            while (std::getline(ss, row)) best.lines.push_back({-1, Trim(row)});
        } else {
            best.state = kInstrumental;
        }
    }
    return best;
}

std::wstring Key(const std::wstring& artist, const std::wstring& title) { return Lower(artist) + L"\n" + Lower(title); }

}  // namespace

int Lyrics::LineAt(int64_t pos) const {
    if (state != kSynced) return -1;
    int r = -1;
    for (int i = 0; i < (int)lines.size() && lines[i].at <= pos + 2000000; ++i) r = i;  // на 0.2 с раньше
    return r;
}

struct Fetcher::Job {
    Fetcher* self;
    std::wstring key, artist, title;
    int dur;
    HWND notify;
    UINT msg;
};

void Fetcher::Request(const std::wstring& artist, const std::wstring& title, int durSec, HWND notify, UINT msg) {
    if (title.empty()) return;
    std::wstring key = Key(artist, title);
    AcquireSRWLockExclusive(&mu_);
    bool have = cache_.count(key) && cache_[key].state != kOffline;
    if (!have) cache_[key].state = kLoading;
    ReleaseSRWLockExclusive(&mu_);
    if (have) return;
    auto* job = new Job{this, key, artist, title, durSec, notify, msg};
    HANDLE t = CreateThread(nullptr, 0, Thread, job, 0, nullptr);
    if (t) CloseHandle(t);
}

Lyrics Fetcher::Get(const std::wstring& artist, const std::wstring& title) {
    AcquireSRWLockExclusive(&mu_);
    auto it = cache_.find(Key(artist, title));
    Lyrics r = it == cache_.end() ? Lyrics() : it->second;
    ReleaseSRWLockExclusive(&mu_);
    return r;
}

DWORD WINAPI Fetcher::Thread(void* p) {
    std::unique_ptr<Job> j((Job*)p);
    std::wstring title = Clean(j->title), artist = Clean(j->artist);
    // попытки: как есть; «Исполнитель - Название» из заголовка видео; просто строкой поиска
    std::vector<std::wstring> tries;
    auto add = [&](const std::wstring& track, const std::wstring& art) {
        if (track.empty()) return;
        std::wstring p = L"/api/search?track_name=" + Encode(track);
        if (!art.empty()) p += L"&artist_name=" + Encode(art);
        if (std::find(tries.begin(), tries.end(), p) == tries.end()) tries.push_back(p);
    };
    add(title, artist);
    size_t dash = title.find(L" - ");
    std::wstring left = dash == std::wstring::npos ? title : Trim(title.substr(0, dash));
    if (dash != std::wstring::npos) {
        std::wstring right = Trim(title.substr(dash + 3));
        add(right, left);  // «Исполнитель - Название» из заголовка видео
        add(left, right);
    }
    // исполнитель бывает в скобках: «Control (Halsey) - Demon Slayer [AMV]»
    for (size_t a = j->title.find(L'('); a != std::wstring::npos; a = j->title.find(L'(', a + 1)) {
        size_t b = j->title.find(L')', a);
        if (b == std::wstring::npos) break;
        add(left, Trim(j->title.substr(a + 1, b - a - 1)));
    }
    add(left, L"");  // одно название: подходящее выберется по длине трека
    tries.push_back(L"/api/search?q=" + Encode(artist + L" " + title));
    Lyrics found;
    found.state = kMissing;
    bool offline = false;
    for (auto& path : tries) {
        std::string body;
        if (!HttpGet(path, body, offline)) {
            if (offline) break;
            continue;
        }
        Parser ps{body};
        Json arr = ps.Value();
        Lyrics l = Pick(arr, j->dur);
        if (l.state != kMissing) {
            found = std::move(l);
            break;
        }
    }
    if (offline && found.state == kMissing) found.state = kOffline;
    AcquireSRWLockExclusive(&j->self->mu_);
    j->self->cache_[j->key] = std::move(found);
    ReleaseSRWLockExclusive(&j->self->mu_);
    PostMessageW(j->notify, j->msg, 0, 0);
    return 0;
}

}  // namespace lyrics
