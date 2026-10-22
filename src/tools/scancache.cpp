// Итог обхода кэшей и мусора проектов на диске (О5): после перезапуска в пределах срока - он, а не новый обход.
#include <cwchar>
#include <string>

#include "tools/tools.h"

namespace tools {

namespace {

std::wstring Clean(std::wstring s) {
    for (wchar_t& ch : s)
        if (ch == L'\t' || ch == L'\n' || ch == L'\r') ch = L' ';
    return s;
}

std::vector<std::wstring> Cols(const std::wstring& line) {
    std::vector<std::wstring> p;
    size_t a = 0;
    for (size_t t; (t = line.find(L'\t', a)) != std::wstring::npos; a = t + 1) p.push_back(line.substr(a, t - a));
    p.push_back(line.substr(a));
    return p;
}

std::wstring File() { return DataDir() + L"\\scan-cache.tsv"; }

}  // namespace

std::wstring ScanCacheRows(const ScanCache& c) {
    std::wstring s = L"when\t" + std::to_wstring(c.when) + L"\nddc\t" + std::to_wstring(c.ddcOld) + L"\n";
    for (const Junk& j : c.junk)
        s += L"junk\t" + Clean(j.name) + L"\t" + Clean(j.path) + L"\t" + std::to_wstring(j.bytes) + L"\t" +
             std::to_wstring(j.fresh) + L"\t" + (j.admin ? L"1" : L"0") + L"\t" + (j.slow ? L"1" : L"0") + L"\n";
    for (const ProjectJunk& p : c.projects)
        s += L"proj\t" + Clean(p.project) + L"\t" + Clean(p.name) + L"\t" + Clean(p.path) + L"\t" + Clean(p.kind) +
             L"\t" + std::to_wstring(p.bytes) + L"\t" + std::to_wstring(p.idleDays) + L"\n";
    return s;
}

bool ParseScanCache(const std::wstring& text, ScanCache& c) {
    c = ScanCache();
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find(L'\n', a);
        std::wstring line = text.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
        a = e == std::wstring::npos ? text.size() : e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> p = Cols(line);
        if (p[0] == L"when" && p.size() == 2) {
            c.when = _wtoi64(p[1].c_str());
        } else if (p[0] == L"ddc" && p.size() == 2) {
            c.ddcOld = _wcstoui64(p[1].c_str(), nullptr, 10);
        } else if (p[0] == L"junk" && p.size() == 7) {
            Junk j;
            j.name = p[1], j.path = p[2];
            j.bytes = _wcstoui64(p[3].c_str(), nullptr, 10), j.fresh = _wcstoui64(p[4].c_str(), nullptr, 10);
            j.admin = p[5] == L"1", j.slow = p[6] == L"1";
            c.junk.push_back(j);
        } else if (p[0] == L"proj" && p.size() == 7) {
            ProjectJunk pj;
            pj.project = p[1], pj.name = p[2], pj.path = p[3], pj.kind = p[4];
            pj.bytes = _wcstoui64(p[5].c_str(), nullptr, 10), pj.idleDays = _wtoi(p[6].c_str());
            c.projects.push_back(pj);
        }
    }
    return c.when > 0;
}

void SaveScanCache(const ScanCache& c) {
    std::wstring text = ScanCacheRows(c);
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string raw(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), raw.data(), n, nullptr, nullptr);
    std::wstring path = File(), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    bool ok = WriteFile(f, raw.data(), (DWORD)raw.size(), &put, nullptr) && put == raw.size();
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void DropScanCache() { DeleteFileW(File().c_str()); }

bool LoadScanCache(ScanCache& c) {
    HANDLE f = CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string raw(4 * 1024 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, raw.data(), (DWORD)raw.size(), &got, nullptr);
    CloseHandle(f);
    raw.resize(got);
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), text.data(), n);
    return ParseScanCache(text, c);
}

}  // namespace tools
