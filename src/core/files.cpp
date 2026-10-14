#include "core/files.h"

#include <algorithm>

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "core/config.h"
#include "core/perf.h"
#include "core/text.h"

namespace {

// "Blender.url" -> "Blender", "X.exe — ярлык.lnk" -> "X"
std::wstring DisplayName(std::wstring n, bool isDir) {
    if (isDir) return n;
    for (const wchar_t* ext : {L".lnk", L".url"}) {
        if (EndsWithI(n, ext)) {
            n.resize(n.size() - wcslen(ext));
            break;
        }
    }
    size_t dash = n.find(L" — ярлык");
    if (dash == std::wstring::npos) dash = n.find(L" - ярлык");
    if (dash != std::wstring::npos) n.resize(dash);
    if (EndsWithI(n, L".exe")) n.resize(n.size() - 4);
    return n;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

}  // namespace

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

std::wstring ExpandPath(const std::wstring& p) {
    if (p.empty()) return p;
    std::wstring s = p;
    static const struct {
        const wchar_t* tok;
        const KNOWNFOLDERID* id;
    } kTokens[] = {{L"{desktop}", &FOLDERID_Desktop},     {L"{downloads}", &FOLDERID_Downloads},
                   {L"{documents}", &FOLDERID_Documents}, {L"{music}", &FOLDERID_Music},
                   {L"{pictures}", &FOLDERID_Pictures},   {L"{videos}", &FOLDERID_Videos},
                   {L"{home}", &FOLDERID_Profile}};
    for (const auto& t : kTokens) {
        size_t at = s.find(t.tok);
        if (at != std::wstring::npos) s.replace(at, wcslen(t.tok), KnownFolder(*t.id));
    }
    if (s.find(L'%') != std::wstring::npos) {
        wchar_t buf[2048];
        if (ExpandEnvironmentStringsW(s.c_str(), buf, 2048)) s = buf;
    }
    return s;
}

std::wstring ResolveIcon(const std::wstring& p) {
    if (p.empty()) return {};
    std::wstring s = ExpandPath(p);
    if (PathIsRelativeW(s.c_str())) return ExeDir() + L"\\" + s;
    return s;
}

bool IsFolder(const std::wstring& full) {
    if (full.rfind(L"shell:", 0) == 0) return false;
    DWORD a = GetFileAttributesW(full.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsImageFile(const std::wstring& n) {
    for (const wchar_t* e : {L".png", L".webp", L".jpg", L".jpeg", L".bmp", L".ico"})
        if (EndsWithI(n, e)) return true;
    return false;
}

std::wstring ParentDir(const std::wstring& p) {
    std::wstring s = p;
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    size_t at = s.find_last_of(L'\\');
    return at == std::wstring::npos ? s : s.substr(0, at);
}

// Путь значка: внутри папки программы храним относительным.
std::wstring ShortPath(const std::wstring& full) {
    std::wstring base = ExeDir() + L"\\";
    if (full.size() > base.size() && _wcsnicmp(full.c_str(), base.c_str(), base.size()) == 0)
        return full.substr(base.size());
    return full;
}

std::vector<Item> ListFolder(const std::wstring& full, bool files) {
    PERF_SCOPE("config.ListFolder");
    std::vector<Item> dirs, links, rest;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((full + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring base = full;
    if (!base.empty() && base.back() == L'\\') base.pop_back();
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || n[0] == L'.' || n.rfind(L"~$", 0) == 0) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (_wcsicmp(n.c_str(), L"desktop.ini") == 0) continue;
        bool isDir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        Item it;
        it.name = DisplayName(n, isDir);
        it.path = it.full = base + L"\\" + n;
        it.dir = isDir;
        if (isDir)
            dirs.push_back(std::move(it));
        else if (EndsWithI(n, L".lnk") || EndsWithI(n, L".url") || EndsWithI(n, L".exe"))
            links.push_back(std::move(it));
        else if (files)
            rest.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    auto byName = [](const Item& a, const Item& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; };
    std::vector<Item> all;
    for (auto* v : {&dirs, &links, &rest}) {
        std::sort(v->begin(), v->end(), byName);
        for (auto& it : *v) all.push_back(std::move(it));
    }
    return all;
}
