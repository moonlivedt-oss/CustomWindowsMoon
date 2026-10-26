// «Телефон рядом»: файлы. Отдача с Range (докачка, просмотр видео без скачивания), ZIP на лету без сжатия (длина
// известна заранее), загрузка с телефона кусками с докачкой, папки ПК, которые открыты телефону (phone_share).
#include <winsock2.h>
#include <shlobj.h>

#include <algorithm>

#include "core/text.h"
#include "phone/phone_internal.h"

namespace phone::detail {

namespace {

std::string Q(const Request& r, const char* k) {
    auto it = r.query.find(k);
    return it == r.query.end() ? std::string() : it->second;
}

FILETIME FromUnixMs(long long ms) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)ms * 10000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    return ft;
}

std::wstring Unique(const std::wstring& dir, const std::wstring& name) {
    std::wstring p = dir + L"\\" + name;
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    for (int i = 2; i < 10000; ++i) {
        p = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    }
    return L"";
}

std::string Disposition(const std::wstring& name, bool inlineView) {
    std::string ascii;
    for (wchar_t ch : name) ascii += ch >= 32 && ch < 127 && ch != L'"' && ch != L'\\' ? (char)ch : '_';
    return std::string("Content-Disposition: ") + (inlineView ? "inline" : "attachment") + "; filename=\"" + ascii +
           "\"; filename*=UTF-8''" + UrlEncode(Narrow(name)) + "\r\n";
}

// ---------------- ZIP на лету ----------------

struct ZipItem {
    std::wstring path;
    std::string name;
    long long size = 0;
    FILETIME written{};
};

void DosTime(const FILETIME& ft, uint16_t& t, uint16_t& d) {
    FILETIME local;
    FileTimeToLocalFileTime(&ft, &local);
    WORD dd = 0, tt = 0;
    FileTimeToDosDateTime(&local, &dd, &tt);
    t = tt, d = dd;
}

void SendZip(Conn& cn, const std::vector<ZipItem>& items, const std::wstring& zipName) {
    std::vector<std::pair<std::string, long long>> sizes;
    for (const ZipItem& z : items) sizes.push_back({z.name, z.size});
    if (items.empty()) return Text(cn, 404, "нечего паковать");
    if (!ZipFits(sizes)) return Text(cn, 413, "больше 4 ГБ - скачайте по одному");
    long long total = ZipSize(sizes);
    std::string head = Head(cn, 200, "application/zip", total, Disposition(zipName, false));
    if (!SendAll(cn.c, head.data(), head.size())) return void(cn.close = true);
    g_sendLeft += total;
    std::vector<ZipEntry> dir;
    uint32_t offset = 0;
    long long sentData = 0;
    std::vector<char> buf(256 << 10);
    bool ok = true;
    for (const ZipItem& z : items) {
        ZipEntry e;
        e.name = z.name;
        e.size = (uint32_t)z.size;
        e.offset = offset;
        DosTime(z.written, e.dosTime, e.dosDate);
        std::string local = ZipLocal(e.name, e.size, e.dosTime, e.dosDate);
        ok = ok && SendAll(cn.c, local.data(), local.size());
        HANDLE h = CreateFileW(z.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        long long left = z.size;
        uint32_t crc = 0;
        while (ok && left > 0 && !Stopping()) {
            DWORD got = 0;
            DWORD want = (DWORD)std::min<long long>(left, (long long)buf.size());
            if (h == INVALID_HANDLE_VALUE || !ReadFile(h, buf.data(), want, &got, nullptr) || !got) {
                // файл пропал или стал короче: добиваем нулями, чтобы длина архива сошлась (такой файл будет битым)
                got = want;
                memset(buf.data(), 0, got);
            }
            crc = Crc32(crc, buf.data(), got);
            ok = SendAll(cn.c, buf.data(), got);
            left -= got;
            g_sendLeft -= got;
            sentData += got;
            Lock l;
            S().seen = GetTickCount64();
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        e.crc = crc;
        std::string desc = ZipDescriptor(crc, e.size);
        ok = ok && SendAll(cn.c, desc.data(), desc.size());
        offset += (uint32_t)(local.size() + z.size + desc.size());
        dir.push_back(e);
        if (!ok) break;
    }
    std::string central;
    for (const ZipEntry& e : dir) central += ZipCentral(e);
    central += ZipEnd(dir.size(), (uint32_t)(central.size()), offset);
    ok = ok && SendAll(cn.c, central.data(), central.size());
    g_sendLeft -= total - sentData;  // заголовки и недосланное - разом
    if (!ok) cn.close = true;
}

void Walk(const std::wstring& dir, const std::wstring& rel, std::vector<ZipItem>& out, size_t max) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // ссылки и точки соединения - мимо
        std::wstring r = rel.empty() ? n : rel + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Walk(dir + L"\\" + n, r, out, max);
        } else if (out.size() < max) {
            out.push_back({dir + L"\\" + n, ZipName(r), ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                           fd.ftLastWriteTime});
        }
    } while (out.size() < max && FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------- папки ПК ----------------

std::wstring FinalPath(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    wchar_t buf[2048];
    DWORD n = GetFinalPathNameByHandleW(h, buf, 2048, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    return n && n < 2048 ? Lower(buf) : L"";
}

// путь в папке ПК: r - номер папки, p - путь внутри; пусто - нельзя (вне папки, ссылкой наружу, нет такого)
std::wstring ShareFile(const Request& r, std::wstring* rootName = nullptr) {
    std::vector<std::wstring> roots = ShareRoots();
    size_t i = (size_t)atoi(Q(r, "r").c_str());
    if (i >= roots.size()) return L"";
    std::wstring rel = SafeRel(Q(r, "p"));
    if (rel == L"?") return L"";
    std::wstring root = roots[i], full = rel.empty() ? root : root + L"\\" + rel;
    std::wstring fr = FinalPath(root), ff = FinalPath(full);
    // точки соединения и ссылки, ведущие наружу, видны по настоящему пути
    if (fr.empty() || ff.empty() || ff.compare(0, fr.size(), fr) != 0 ||
        (ff.size() > fr.size() && ff[fr.size()] != L'\\'))
        return L"";
    if (rootName) *rootName = root.substr(root.find_last_of(L'\\') + 1);
    return full;
}

void ShareList(Conn& cn, const Request& r) {
    Seen();
    std::wstring rootName, dir = ShareFile(r, &rootName);
    DWORD a = dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return Text(cn, 403, "нет доступа");
    struct E {
        std::wstring n;
        long long s;
        bool d;
    };
    std::vector<E> list;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                continue;
            list.push_back({n, ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
        } while (list.size() < 3000 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(list.begin(), list.end(), [](const E& a, const E& b) {
        if (a.d != b.d) return a.d;
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.n.c_str(), -1,
                               b.n.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    std::string j = "{\"root\":" + phone::Json(rootName) + ",\"items\":[";
    for (size_t k = 0; k < list.size(); ++k)
        j += std::string(k ? "," : "") + "{\"n\":" + phone::Json(list[k].n) + ",\"s\":" + std::to_string(list[k].s) +
             ",\"d\":" + (list[k].d ? "true" : "false") + "}";
    JsonReply(cn, j + "]}");
}

// ---------------- загрузка с телефона ----------------

std::wstring TargetDir(const Request& r, const std::string& kind, long long mt) {
    if (kind == "to") {  // в открытую папку ПК, которую выбрали на телефоне
        std::wstring d = FilesAllowed() ? ShareFile(r) : L"";
        DWORD a = d.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(d.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? d : L"";
    }
    if (kind == "photo") return DatedDir(PhotosDir(), mt);
    if (kind == "scan") return DatedDir(PhotosDir() + L"\\Сканы", 0);
    return DownloadsDir();
}

void Finish(Conn& cn, Incoming in, const std::string& kind) {
    if (in.mt > 0) {  // дата снимка - дата файла
        HANDLE h = CreateFileW(in.tmp.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME ft = FromUnixMs(in.mt);
            SetFileTime(h, &ft, nullptr, &ft);
            CloseHandle(h);
        }
    }
    std::wstring final = Unique(in.dir, in.name);
    if (final.empty() || !MoveFileExW(in.tmp.c_str(), final.c_str(), 0)) {
        DeleteFileW(in.tmp.c_str());
        return Text(cn, 500, "не записать");
    }
    SetFileAttributesW(final.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!IsMedia(in.name)) MarkFromInternet(final);
    {
        Lock l;
        ++S().received;
        S().receivedBytes += in.total;
    }
    Post(kind == "scan" ? EV_SCAN : EV_RECEIVED, final);
    JsonReply(cn, "{\"have\":" + std::to_string(in.total) + ",\"done\":true}");
}

// PUT /up?id=&off=&total=&name=&kind=&mt=: кусок файла; GET /up?id= - сколько уже есть (докачка)
void Upload(Conn& cn, const Request& r) {
    Seen();
    std::string id = Q(r, "id"), kind = Q(r, "kind");
    if (!UploadId(id)) return Text(cn, 400, "нужен id");
    if (r.method == "GET") {
        Lock l;
        auto it = S().incoming.find(id);
        return JsonReply(cn, "{\"have\":" + std::to_string(it == S().incoming.end() ? 0 : it->second.have) + "}");
    }
    if (!r.headers.count("content-length")) return Text(cn, 411, "нужна длина");
    long long off = atoll(Q(r, "off").c_str()), total = atoll(Q(r, "total").c_str());
    if (total < 0 || off < 0 || off + r.length > total) return Text(cn, 400, "кусок не сходится");
    Incoming in;
    {
        Lock l;
        auto it = S().incoming.find(id);
        if (it == S().incoming.end()) {  // новый файл
            if (off != 0) return JsonReply(cn, "{\"have\":0}");
            Incoming n;
            n.name = SafeName(Widen(Q(r, "name")));
            n.mt = atoll(Q(r, "mt").c_str());
            n.total = total;
            n.dir = TargetDir(r, kind, n.mt);
            it = S().incoming.emplace(id, n).first;
        }
        if (it->second.busy) return Text(cn, 409, "кусок уже пишется");
        if (it->second.have != off || it->second.total != total) {  // докачка с другого места - скажем, откуда
            cn.close = cn.bodyLeft > 0;
            return Reply(cn, 409, "application/json", "{\"have\":" + std::to_string(it->second.have) + "}");
        }
        it->second.busy = true;
        in = it->second;
    }
    auto release = [&](long long have) {
        Lock l;
        auto it = S().incoming.find(id);
        if (it != S().incoming.end()) it->second.busy = false, it->second.have = have, it->second.tmp = in.tmp;
    };
    if (in.tmp.empty()) {
        if (!in.dir.empty()) SHCreateDirectoryExW(nullptr, in.dir.c_str(), nullptr);
        DWORD da = in.dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(in.dir.c_str());
        if (da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY)) {
            release(0);
            return Text(cn, 500, "не создать папку");
        }
        ULARGE_INTEGER freeB{};
        if (GetDiskFreeSpaceExW(in.dir.c_str(), &freeB, nullptr, nullptr) &&
            (long long)freeB.QuadPart < total + (256ll << 20)) {
            release(0);
            return Text(cn, 507, "на компьютере мало места");
        }
        in.tmp = in.dir + L"\\." + in.name + L"." + Widen(id) + L".part";
    }
    HANDLE h = CreateFileW(in.tmp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        release(in.have);
        return Text(cn, 500, "не записать");
    }
    LARGE_INTEGER at{};
    at.QuadPart = off;
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    SetEndOfFile(h);  // хвост прошлой оборванной попытки - прочь
    std::vector<char> buf(256 << 10);
    long long have = off;
    bool ok = true;
    while (cn.bodyLeft > 0 && !Stopping()) {
        int k = cn.Read(buf.data(), (int)buf.size());
        if (k <= 0) {
            ok = false;
            break;
        }
        DWORD w = 0;
        if (!WriteFile(h, buf.data(), (DWORD)k, &w, nullptr) || w != (DWORD)k) {
            ok = false;
            break;
        }
        have += k;
        if ((have & ((4 << 20) - 1)) < k) {  // примерно раз в 4 МБ: страница при обрыве спросит, сколько есть
            Lock l;
            S().seen = GetTickCount64();
            auto it = S().incoming.find(id);
            if (it != S().incoming.end()) it->second.have = have;
        }
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    release(have);
    if (!ok) return Text(cn, 400, "оборвалось");
    if (have < total) return JsonReply(cn, "{\"have\":" + std::to_string(have) + "}");
    {
        Lock l;
        S().incoming.erase(id);
    }
    in.have = have;
    Finish(cn, in, kind);
}

}  // namespace

std::wstring UniquePath(const std::wstring& dir, const std::wstring& name) { return Unique(dir, name); }

void MarkFromInternet(const std::wstring& path) {  // как браузер: файл пришёл извне - Windows спросит перед запуском
    HANDLE z = CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (z != INVALID_HANDLE_VALUE) {
        const char mark[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
        DWORD w = 0;
        WriteFile(z, mark, sizeof(mark) - 1, &w, nullptr);
        CloseHandle(z);
    }
}

std::wstring SharePathOf(const Request& r, const char* rk, const char* pk) {
    Request q = r;
    q.query["r"] = Q(r, rk);
    q.query["p"] = Q(r, pk);
    return ShareFile(q);
}

bool ShareRootIs(const std::wstring& path) {
    std::wstring f = FinalPath(path);
    for (const std::wstring& root : ShareRoots())
        if (!f.empty() && FinalPath(root) == f) return true;
    return false;
}

bool SendFile(Conn& cn, const Request& r, const std::wstring& path, const std::wstring& name, bool inlineView,
              bool* complete) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Text(cn, 404, "файл пропал с компьютера");
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    long long size = sz.QuadPart, from = 0, to = size - 1;
    bool partial = false;
    auto rg = r.headers.find("range");
    if (rg != r.headers.end() && size > 0) {
        if (!ParseRange(rg->second, size, from, to)) {
            CloseHandle(h);
            Reply(cn, 416, "", "", "Content-Range: bytes */" + std::to_string(size) + "\r\n");
            return false;
        }
        partial = true;
    }
    std::string extra = "Accept-Ranges: bytes\r\n" + Disposition(name, inlineView);
    if (partial)
        extra += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" +
                 std::to_string(size) + "\r\n";
    long long len = size > 0 ? to - from + 1 : 0;
    std::string head = Head(cn, partial ? 206 : 200, ContentType(name), len, extra);
    bool ok = SendAll(cn.c, head.data(), head.size());
    if (!ok || r.method == "HEAD") {
        CloseHandle(h);
        return false;
    }
    LARGE_INTEGER at{};
    at.QuadPart = from;
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    std::vector<char> buf(256 << 10);
    long long left = len;
    g_sendLeft += len;
    while (ok && left > 0 && !Stopping()) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)std::min<long long>(left, (long long)buf.size()), &got, nullptr) || !got)
            break;
        ok = SendAll(cn.c, buf.data(), got);
        left -= got;
        g_sendLeft -= got;
        Lock l;
        S().seen = GetTickCount64();
    }
    g_sendLeft -= left;
    CloseHandle(h);
    if (left > 0) cn.close = true;  // не дослали - длина в заголовке врёт, соединение больше не годится
    if (complete) *complete = ok && left == 0 && to == size - 1;
    return ok && left == 0;
}

bool HandleFiles(Conn& cn, const Request& r, const std::string& rest) {
    bool get = r.method == "GET" || r.method == "HEAD";
    if (rest.compare(0, 3, "/f/") == 0 && get) {  // брошенный на телефон файл
        Seen();
        size_t n = (size_t)atoi(rest.c_str() + 3);
        Offered f;
        {
            Lock l;
            if (n >= S().files.size()) {
                Text(cn, 404, "нет такого файла");
                return true;
            }
            f = S().files[n];
        }
        bool complete = false;
        SendFile(cn, r, f.path, f.name.substr(f.name.find_last_of(L'\\') + 1), false, &complete);
        if (complete) {
            bool first = false, all = true;
            {
                Lock l;
                if (n < S().files.size()) {
                    first = !S().files[n].done;
                    S().files[n].done = true;
                    ++S().seq;
                }
                for (const Offered& o : S().files) all = all && o.done;
                if (all && !S().allDone) S().allDone = GetTickCount64();
            }
            if (first) Post(EV_FILE_DONE, f.name);
        }
        return true;
    }
    if (rest == "/zip" && get) {  // всё брошенное одним архивом
        Seen();
        std::vector<ZipItem> items;
        {
            Lock l;
            for (const Offered& f : S().files) {
                WIN32_FILE_ATTRIBUTE_DATA a{};
                GetFileAttributesExW(f.path.c_str(), GetFileExInfoStandard, &a);
                items.push_back({f.path, ZipName(f.name), f.size, a.ftLastWriteTime});
            }
        }
        SendZip(cn, items, L"С компьютера.zip");
        {
            Lock l;
            for (Offered& f : S().files) f.done = true;
            ++S().seq;
            if (!S().allDone) S().allDone = GetTickCount64();
        }
        Post(EV_FILE_DONE, L"архив");
        return true;
    }
    if (rest == "/up") {
        Upload(cn, r);
        return true;
    }
    if (rest == "/ls" && get) {
        ShareList(cn, r);
        return true;
    }
    if (rest == "/get" && get) {  // файл из папки ПК: смотреть (inline) или скачать (dl=1)
        Seen();
        std::wstring p = ShareFile(r);
        DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
            Text(cn, 403, "нет доступа");
            return true;
        }
        SendFile(cn, r, p, p.substr(p.find_last_of(L'\\') + 1), Q(r, "dl") != "1");
        return true;
    }
    if (rest == "/zipdir" && get) {  // папка ПК целиком
        Seen();
        std::wstring p = ShareFile(r);
        DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            Text(cn, 403, "нет доступа");
            return true;
        }
        std::wstring leaf = p.substr(p.find_last_of(L'\\') + 1);
        std::vector<ZipItem> items;
        Walk(p, leaf, items, 20000);
        SendZip(cn, items, leaf + L".zip");
        return true;
    }
    return false;
}

}  // namespace phone::detail
