// Дубликаты больших файлов в личных папках: размер -> хэш начала и конца -> полный SHA-256.
// Оставляется копия не из Загрузок, среди них - самая старая; остальные уходят в Корзину через RadialTools.exe,
// который перед этим сам сверяет размер и SHA-256 с оставляемым файлом.
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <map>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

// папки сборки и служебные: копии там - норма (кэши, артефакты), их не трогаем
bool SkipDir(const std::wstring& name) {
    std::wstring n = Lower(name);
    static const wchar_t* kSkip[] = {L"node_modules",
                                     L"__pycache__",
                                     L"intermediate",
                                     L"saved",
                                     L"deriveddatacache",
                                     L"binaries",
                                     L".venv",
                                     L"venv",
                                     L"obj",
                                     L"$recycle.bin",
                                     L"target",
                                     L"dist",
                                     L"out",
                                     L"system volume information"};
    if (n.empty() || n[0] == L'.' || n.rfind(L"build", 0) == 0 || n.rfind(L"cmake-build", 0) == 0) return true;
    for (const wchar_t* s : kSkip)
        if (n == s) return true;
    return false;
}

struct File {
    std::wstring path;
    ULONGLONG size, created;
};

// Папка проекта или библиотеки ассетов: одинаковые файлы там нужны проекту (MetaHumans в Content из Megascans,
// текстуры в наборах моделей) - удалишь копию, сломаешь проект.
bool ProjectDir(const std::wstring& dir) {
    if (EndsWithI(dir, L"\\Megascans Library")) return true;
    for (const wchar_t* m : {L"\\.git", L"\\*.uproject", L"\\*.uplugin", L"\\package.json", L"\\CMakeLists.txt",
                             L"\\Cargo.toml", L"\\pyproject.toml", L"\\*.sln", L"\\project.godot"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + m).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            return true;
        }
    }
    return false;
}

// ассеты движков: копии между проектами - норма
bool EngineAsset(const std::wstring& name) {
    for (const wchar_t* e : {L".uasset", L".umap", L".uexp", L".ubulk", L".pak", L".utoc", L".ucas"})
        if (EndsWithI(name, e)) return true;
    return false;
}

void Walk(const std::wstring& dir, ULONGLONG minBytes, std::vector<File>& out, volatile bool* cancel,
          volatile long* progress, int depth) {
    if (depth > 24 || (cancel && *cancel)) return;
    if (ProjectDir(dir)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // ссылки и OneDrive-заглушки
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!SkipDir(n)) Walk(dir + L"\\" + n, minBytes, out, cancel, progress, depth + 1);
            continue;
        }
        if (progress) InterlockedIncrement(progress);
        ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (size < minBytes || (fd.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_OFFLINE)) ||
            EngineAsset(n))
            continue;
        out.push_back({dir + L"\\" + n, size,
                       ((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime});
    } while (FindNextFileW(h, &fd) && !(cancel && *cancel));
    FindClose(h);
}

// SHA-256 первых и последних 64 КБ: быстро отсеивает файлы одного размера с разным содержимым
std::wstring EdgeHash(const File& f) {
    HANDLE h = CreateFileW(f.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::vector<BYTE> buf(128 * 1024);
    DWORD got = 0, total = 0;
    ReadFile(h, buf.data(), 64 * 1024, &got, nullptr);
    total = got;
    if (f.size > 128 * 1024) {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)f.size - 64 * 1024;
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        ReadFile(h, buf.data() + total, 64 * 1024, &got, nullptr);
        total += got;
    }
    CloseHandle(h);
    BYTE hash[32];
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return L"";
    NTSTATUS st = BCryptHash(alg, nullptr, 0, buf.data(), total, hash, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) return L"";
    std::wstring out;
    wchar_t hx[3];
    for (BYTE b : hash) swprintf(hx, 3, L"%02x", b), out += hx;
    return out;
}

}  // namespace

std::vector<std::wstring> DupRoots() {
    std::vector<std::wstring> roots;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Downloads, &FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Videos,
                                    &FOLDERID_Pictures, &FOLDERID_Music}) {
        std::wstring p = Known(*id);
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) roots.push_back(p);
    }
    // вложенные корни не нужны (Рабочий стол бывает внутри Документов и наоборот)
    std::sort(roots.begin(), roots.end(),
              [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
    std::vector<std::wstring> out;
    for (const std::wstring& r : roots)
        if (out.empty() || Lower(r).rfind(Lower(out.back()) + L"\\", 0) != 0) {
            if (!out.empty() && Lower(out.back()) == Lower(r)) continue;
            out.push_back(r);
        }
    return out;
}

std::vector<DupGroup> FindDuplicates(const std::vector<std::wstring>& roots, ULONGLONG minBytes, volatile bool* cancel,
                                     volatile long* progress) {
    std::vector<File> files;
    for (const std::wstring& r : roots) Walk(r, minBytes, files, cancel, progress, 0);
    std::map<ULONGLONG, std::vector<File>> bySize;
    for (File& f : files) bySize[f.size].push_back(std::move(f));
    std::vector<DupGroup> out;
    std::wstring downloads = Lower(DownloadsDir());
    for (auto& [size, same] : bySize) {
        if (same.size() < 2 || (cancel && *cancel)) continue;
        std::map<std::wstring, std::vector<File>> byEdge;
        for (File& f : same) {
            std::wstring e = EdgeHash(f);
            if (!e.empty()) byEdge[e].push_back(f);
        }
        for (auto& [e, cand] : byEdge) {
            if (cand.size() < 2) continue;
            std::map<std::wstring, std::vector<File>> byFull;
            for (File& f : cand) {
                std::wstring h = FileSha256(f.path);
                if (!h.empty()) byFull[h].push_back(f);
            }
            for (auto& [h, group] : byFull) {
                if (group.size() < 2) continue;
                // остаётся: сначала не из Загрузок, среди равных - самый старый
                std::sort(group.begin(), group.end(), [&](const File& a, const File& b) {
                    bool da = Lower(a.path).rfind(downloads + L"\\", 0) == 0,
                         db = Lower(b.path).rfind(downloads + L"\\", 0) == 0;
                    if (da != db) return !da;
                    return a.created < b.created;
                });
                DupGroup g;
                g.size = size;
                g.hash = h;
                for (const File& f : group) g.paths.push_back(f.path);
                out.push_back(std::move(g));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const DupGroup& a, const DupGroup& b) {
        return a.size * (a.paths.size() - 1) > b.size * (b.paths.size() - 1);
    });
    return out;
}

}  // namespace tools
