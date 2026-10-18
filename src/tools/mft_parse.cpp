// Разбор MFT без чтения тома: записи, куски атрибутов, таблица папок и её файл кэша (mft_detail.h).
// Сам том читает mft.cpp - в процессе с правами; здесь только байты в памяти, поэтому это покрыто тестами.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

#include "core/fuzzy.h"
#include "core/text.h"
#include "tools/mft_detail.h"

namespace tools::mft_detail {

bool Fixup(BYTE* rec, DWORD recSize, DWORD sector) {
    WORD usaOff = At<WORD>(rec, 4), usaCount = At<WORD>(rec, 6);
    if (usaCount < 2 || usaOff + usaCount * 2u > recSize || (DWORD)(usaCount - 1) * sector > recSize) return false;
    WORD usn = At<WORD>(rec, usaOff);
    for (WORD i = 1; i < usaCount; ++i) {
        BYTE* end = rec + (size_t)i * sector - 2;
        if (At<WORD>(end, 0) != usn) return false;
        memcpy(end, rec + usaOff + i * 2, 2);
    }
    return true;
}

std::vector<Extent> Extents(const BYTE* attr, DWORD attrLen) {
    std::vector<Extent> out;
    const BYTE* p = attr + At<WORD>(attr, 0x20);
    const BYTE* end = attr + attrLen;
    LONGLONG lcn = 0;
    while (p < end && *p) {
        int lenBytes = *p & 0x0F, offBytes = *p >> 4;
        ++p;
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8 || p + lenBytes + offBytes > end) break;
        ULONGLONG len = 0;
        for (int i = 0; i < lenBytes; ++i) len |= (ULONGLONG)p[i] << (8 * i);
        p += lenBytes;
        if (!offBytes) {
            out.push_back({-1, len});
            continue;
        }
        ULONGLONG raw = 0;
        for (int i = 0; i < offBytes; ++i) raw |= (ULONGLONG)p[i] << (8 * i);
        if (offBytes < 8 && (p[offBytes - 1] & 0x80)) raw |= ~0ull << (8 * offBytes);  // отрицательное смещение
        p += offBytes;
        lcn += (LONGLONG)raw;
        out.push_back({lcn, len});
    }
    return out;
}

void ParseRecord(BYTE* rec, ULONGLONG recNo, DWORD recSize, DWORD sector, std::vector<Node>& nodes, ULONGLONG target,
                 std::map<ULONGLONG, std::wstring>& names, std::vector<std::pair<ULONGLONG, ULONGLONG>>& links) {
    if (memcmp(rec, "FILE", 4) != 0 || !Fixup(rec, recSize, sector)) return;
    WORD flags = At<WORD>(rec, 0x16);
    if (!(flags & 1)) return;  // запись свободна
    ULONGLONG base = At<ULONGLONG>(rec, 0x20) & kRefMask;
    ULONGLONG owner = base ? base : recNo;  // запись-продолжение: её атрибуты принадлежат основной
    if (owner >= nodes.size()) return;
    Node& n = nodes[owner];
    if (!base) {
        n.used = true;
        n.dir = (flags & 2) != 0;
    }
    DWORD off = At<WORD>(rec, 0x14);
    while (off + 16 <= recSize) {
        const BYTE* a = rec + off;
        DWORD type = At<DWORD>(a, 0), len = At<DWORD>(a, 4);
        if (type == kAttrEnd || type == 0 || len < 16 || off + len > recSize) break;
        bool nonResident = a[8] != 0;
        BYTE nameLen = a[9];
        if (type == kAttrFileName && !nonResident) {
            DWORD vLen = At<DWORD>(a, 0x10);
            WORD vOff = At<WORD>(a, 0x14);
            if (vOff + vLen <= len && vLen >= 0x42) {
                const BYTE* fn = a + vOff;
                BYTE chars = fn[0x40], ns = fn[0x41];
                bool dos = ns == 2;  // короткое имя 8.3 - только если другого нет
                if (0x42u + chars * 2u <= vLen && n.named && !n.dosName && !dos) {
                    links.push_back({owner, At<ULONGLONG>(fn, 0) & kRefMask});  // ещё одна ссылка на тот же файл
                } else if (0x42u + chars * 2u <= vLen && (!n.named || (n.dosName && !dos))) {
                    n.parent = At<ULONGLONG>(fn, 0) & kRefMask;
                    n.named = true;
                    n.dosName = dos;
                    // все папки: основная запись папки уже сказала, что это папка; у продолжения ещё неизвестно -
                    // имя запоминаем, лишнее отсеет BuildDirs
                    if (target == kAllDirs ? n.dir || base : n.parent == target) {
                        std::wstring name(chars, L'\0');
                        memcpy(name.data(), fn + 0x42, chars * 2u);
                        names[owner] = name;
                    }
                }
            }
        } else if (type == kAttrData && nameLen == 0) {
            if (!nonResident)
                n.size = At<DWORD>(a, 0x10);
            else if (At<ULONGLONG>(a, 0x10) == 0)  // размер записан только в первом куске атрибута
                n.size = At<ULONGLONG>(a, 0x30);
        } else if (type == kAttrReparse) {
            n.reparse = true;
        }
        off += len;
    }
}

std::vector<DirRow> BuildDirs(const std::vector<Node>& nodes, const std::map<ULONGLONG, std::wstring>& names,
                              const std::vector<std::pair<ULONGLONG, ULONGLONG>>& links) {
    ULONGLONG count = nodes.size();
    std::vector<int> idx(count, -1);  // номер записи -> строка таблицы
    std::vector<DirRow> rows;
    for (ULONGLONG r = kRootRecord; r < count; ++r) {
        const Node& n = nodes[r];
        if (!n.used || !n.dir || (r != kRootRecord && (r < kFirstUserRecord || !n.named))) continue;
        idx[r] = (int)rows.size();
        DirRow d;
        d.rec = r;
        d.parent = r == kRootRecord ? r : n.parent;
        d.reparse = n.reparse;
        auto it = names.find(r);
        if (it != names.end()) d.name = it->second;
        rows.push_back(std::move(d));
    }
    auto addFile = [&](ULONGLONG r, ULONGLONG parent) {
        const Node& n = nodes[r];
        if (!n.used || n.dir || !n.named || r < kFirstUserRecord || parent >= count || idx[parent] < 0) return;
        DirRow& d = rows[idx[parent]];
        d.loose += n.size;
        ++d.looseFiles;
    };
    for (ULONGLONG r = kFirstUserRecord; r < count; ++r) addFile(r, nodes[r].parent);
    for (const auto& [r, parent] : links)
        if (r < count) addFile(r, parent);
    for (DirRow& d : rows) d.total = d.loose, d.totalFiles = d.looseFiles;

    // глубина от корня (память по пути вверх); оторванные от корня цепочки и петли - -1, их не складываем
    std::vector<int> depth(rows.size(), -2);
    std::vector<int> chain;
    for (size_t i = 0; i < rows.size(); ++i) {
        chain.clear();
        int cur = (int)i, at = -1;
        bool broken = false;
        while (true) {
            if (depth[cur] != -2) {
                at = depth[cur];
                broken = at < 0;
                break;
            }
            chain.push_back(cur);
            if (rows[cur].rec == kRootRecord) {
                at = -1;  // корень получит 0
                break;
            }
            ULONGLONG p = rows[cur].parent;
            if (chain.size() > 4096 || p == rows[cur].rec || p >= count || idx[p] < 0) {
                broken = true;
                break;
            }
            cur = idx[p];
        }
        for (size_t k = chain.size(); k-- > 0;) depth[chain[k]] = broken ? -1 : ++at;
    }
    std::vector<int> order;
    for (size_t i = 0; i < rows.size(); ++i)
        if (depth[i] > 0) order.push_back((int)i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return depth[a] > depth[b]; });
    for (int i : order) {
        DirRow& p = rows[idx[rows[i].parent]];
        p.total += rows[i].total;
        p.totalFiles += rows[i].totalFiles;
    }
    return rows;
}

std::vector<FolderSize> ListDir(const std::vector<DirRow>& rows, ULONGLONG rec, const std::wstring& dir) {
    std::wstring base = !dir.empty() && dir.back() == L'\\' ? dir.substr(0, dir.size() - 1) : dir;
    std::vector<FolderSize> out;
    FolderSize loose;
    loose.name = L"(файлы в корне)";
    loose.path = dir;
    for (const DirRow& d : rows) {
        if (d.rec == rec) {
            loose.bytes = d.loose;
            loose.files = d.looseFiles;
        } else if (d.parent == rec && !d.reparse && d.rec >= kFirstUserRecord && !d.name.empty()) {
            FolderSize f;  // ссылки не раскрываем, как ScanSizes
            f.name = d.name;
            f.path = base + L"\\" + d.name;
            f.bytes = d.total;
            f.files = d.totalFiles;
            out.push_back(f);
        }
    }
    if (loose.files) out.push_back(loose);
    // равные по размеру - по имени: порядок не зависит от порядка записей в таблице
    std::sort(out.begin(), out.end(), [](const FolderSize& a, const FolderSize& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

std::string FormatTable(const TableHead& head, const std::vector<DirRow>& rows) {
    std::string s;
    s.reserve(rows.size() * 64 + 64);
    char b[160];
    snprintf(b, sizeof(b), "#mft\t%08lx\t%llu\t%llu\t%llu\n", (unsigned long)head.serial, head.time, head.records,
             head.ms);
    s += b;
    for (const DirRow& d : rows) {
        snprintf(b, sizeof(b), "%llu\t%llu\t%llu\t%d\t%llu\t%d\t%c\t", d.rec, d.parent, d.total, d.totalFiles, d.loose,
                 d.looseFiles, d.reparse ? 'r' : '-');
        s += b;
        std::string name = Narrow(d.name);
        for (char& ch : name)
            if (ch == '\t' || ch == '\n' || ch == '\r') ch = '?';  // такая папка просто не пройдёт сверку
        s += name;
        s += '\n';
    }
    s += "#end\n";
    return s;
}

bool ParseTable(const std::string& text, ULONGLONG rec, TableHead& head, std::vector<DirRow>& rows) {
    rows.clear();
    if (text.compare(0, 5, "#mft\t") != 0 || text.size() < 10 || text.compare(text.size() - 5, 5, "#end\n") != 0)
        return false;
    const char* p = text.c_str() + 5;
    char* e = nullptr;
    head.serial = (DWORD)strtoul(p, &e, 16);
    head.time = strtoull(e, &e, 10);
    head.records = strtoull(e, &e, 10);
    head.ms = strtoull(e, &e, 10);
    size_t pos = text.find('\n');
    size_t stop = text.size() - 5;
    while (pos != std::string::npos && pos + 1 < stop) {
        size_t start = pos + 1;
        pos = text.find('\n', start);
        if (pos == std::string::npos || pos > stop) return false;
        const char* q = text.c_str() + start;
        ULONGLONG r = strtoull(q, &e, 10);
        ULONGLONG parent = strtoull(e, &e, 10);
        if (rec != kAllDirs && r != rec && parent != rec) continue;  // в файле весь том, нужна одна папка и её дети
        DirRow d;
        d.rec = r;
        d.parent = parent;
        d.total = strtoull(e, &e, 10);
        d.totalFiles = (int)strtol(e, &e, 10);
        d.loose = strtoull(e, &e, 10);
        d.looseFiles = (int)strtol(e, &e, 10);
        if (*e != '\t' || e[1] == 0 || e[2] != '\t') return false;
        d.reparse = e[1] == 'r';
        const char* nm = e + 3;
        if (nm > text.c_str() + pos) return false;
        d.name = Widen(std::string(nm, text.c_str() + pos));
        rows.push_back(std::move(d));
    }
    return true;
}

bool IndexTable(const std::string& text, TableIndex& ix) {
    ix.kids.clear();
    ix.byRec.clear();
    if (!ParseTable(text, kAllDirs, ix.head, ix.rows)) return false;
    ix.byRec.reserve(ix.rows.size());
    for (size_t i = 0; i < ix.rows.size(); ++i) {
        ix.byRec[ix.rows[i].rec] = i;
        ix.kids[ix.rows[i].parent].push_back(i);
    }
    return true;
}

std::vector<DirRow> Slice(const TableIndex& ix, ULONGLONG rec) {
    std::vector<DirRow> out;
    auto self = ix.byRec.find(rec);
    if (self != ix.byRec.end()) out.push_back(ix.rows[self->second]);
    auto k = ix.kids.find(rec);
    if (k != ix.kids.end())
        for (size_t i : k->second)
            if (ix.rows[i].rec != rec) out.push_back(ix.rows[i]);  // корень тома - сам себе родитель
    return out;
}

std::vector<std::wstring> LowerNames(const TableIndex& ix) {
    std::vector<std::wstring> out;
    out.reserve(ix.rows.size());
    for (const DirRow& r : ix.rows) { out.push_back(Lower(r.name)); }
    return out;
}

std::vector<size_t> FindDirs(const TableIndex& ix, const std::vector<std::wstring>& lower, const std::wstring& query,
                             size_t max) {
    std::wstring q = Lower(query);
    std::vector<size_t> out;
    if (q.empty() || lower.size() != ix.rows.size()) return out;
    std::wstring q2 = fuzzy::OtherLayout(q);
    auto score = [](const std::wstring& n, const std::wstring& s) {
        size_t at = n.find(s);
        if (at == std::wstring::npos) return -1;
        if (at == 0) return 0;
        for (; at != std::wstring::npos; at = n.find(s, at + 1)) {
            wchar_t p = n[at - 1];
            if (p == L' ' || p == L'-' || p == L'_' || p == L'.' || p == L'(') return 1;
        }
        return 2;
    };
    std::vector<std::pair<int, size_t>> hits;
    for (size_t i = 0; i < lower.size(); ++i) {
        if (ix.rows[i].rec == kRootRecord || ix.rows[i].reparse) continue;
        int a = score(lower[i], q), b = q2 == q ? -1 : score(lower[i], q2);
        int s = a < 0 ? b : (b < 0 ? a : std::min(a, b));
        if (s >= 0) hits.push_back({s, i});
    }
    std::sort(hits.begin(), hits.end(), [&](const auto& x, const auto& y) {
        if (x.first != y.first) return x.first < y.first;
        return ix.rows[x.second].total > ix.rows[y.second].total;
    });
    for (size_t i = 0; i < hits.size() && i < max; ++i) out.push_back(hits[i].second);
    return out;
}

std::wstring PathOf(const TableIndex& ix, size_t row, const std::wstring& root) {
    std::vector<const std::wstring*> parts;
    size_t cur = row;
    for (int guard = 0; guard < 512; ++guard) {
        const DirRow& r = ix.rows[cur];
        if (r.rec == kRootRecord) {
            std::wstring p = root;
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) p += L"\\" + **it;
            return parts.empty() ? root + L"\\" : p;
        }
        parts.push_back(&r.name);
        auto up = ix.byRec.find(r.parent);
        if (up == ix.byRec.end()) return L"";
        cur = up->second;
    }
    return L"";
}

bool LooseMatch(int mftFiles, ULONGLONG mftBytes, int listFiles, ULONGLONG listBytes) {
    if (mftFiles != listFiles) return false;
    if (mftBytes >= listBytes) return true;
    return listBytes - mftBytes <= listBytes / 100 + 1024 * 1024;
}

}  // namespace tools::mft_detail
