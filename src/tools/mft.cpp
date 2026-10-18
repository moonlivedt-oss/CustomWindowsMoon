// Размеры папок прямым чтением таблицы MFT (только NTFS и только с правами администратора: том \\.\C: открывает
// лишь администратор). Вместо обхода сотен тысяч файлов по одному - один последовательный проход по MFT большими
// кусками: весь диск за секунды. Читаем только: том открыт на чтение, на диск ничего не пишется.
// Задача с правами за один проход считает все папки тома и кладёт таблицу в elevate::DataDir() (её может читать
// обычный пользователь, писать - только администратор); студия несколько минут берёт ответы для любой папки этого
// тома оттуда. Разбор записей - в mft_parse.cpp.
#include <windows.h>

#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>

#include "core/elevate.h"
#include "core/text.h"
#include "net/net.h"
#include "tools/mft_detail.h"
#include "tools/tools.h"

namespace tools {

using namespace mft_detail;

namespace {

constexpr ULONGLONG kFreshSec = 5 * 60;  // столько таблица в кэше считается свежей

struct MftVolume {
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD sector = 512, cluster = 4096, recSize = 1024;
    ULONGLONG mftLcn = 0;
    ~MftVolume() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool Read(ULONGLONG offset, BYTE* buf, DWORD bytes) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)offset;
        DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, buf, bytes, &got, nullptr) && got == bytes;
    }
};

ULONGLONG UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000000ull;
}

// «C:» -> «C:\», лишние косые в конце - прочь
std::wstring NormDir(const std::wstring& dirIn) {
    std::wstring dir = dirIn;
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    if (dir.size() == 2) dir += L"\\";
    return dir;
}

bool IsNtfs(const std::wstring& root, DWORD* serial = nullptr) {
    wchar_t fs[MAX_PATH] = {};
    DWORD sn = 0;
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, &sn, nullptr, nullptr, fs, MAX_PATH)) return false;
    if (serial) *serial = sn;
    return _wcsicmp(fs, L"NTFS") == 0;
}

// Номер записи MFT папки и серийный номер её тома (папка-ссылка открывается по месту, куда ведёт)
bool DirRecord(const std::wstring& dir, ULONGLONG& rec, DWORD& serial) {
    HANDLE dh = CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dh == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION bi{};
    BOOL got = GetFileInformationByHandle(dh, &bi);
    CloseHandle(dh);
    if (!got) return false;
    rec = (((ULONGLONG)bi.nFileIndexHigh << 32) | bi.nFileIndexLow) & kRefMask;
    serial = bi.dwVolumeSerialNumber;
    return true;
}

// Весь том за один проход: таблица всех папок. root - «C:\».
bool ReadVolume(const std::wstring& root, std::vector<DirRow>& rows, TableHead& head, std::wstring& err) {
    auto t0 = std::chrono::steady_clock::now();
    if (!IsNtfs(root, &head.serial)) return err = L"не NTFS", false;
    MftVolume v;
    v.h = CreateFileW((L"\\\\.\\" + root.substr(0, 2)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr, OPEN_EXISTING, 0, nullptr);
    if (v.h == INVALID_HANDLE_VALUE) return err = L"том не открылся (нужны права администратора)", false;
    std::vector<BYTE> boot(4096);
    if (!v.Read(0, boot.data(), (DWORD)boot.size()) || memcmp(boot.data() + 3, "NTFS    ", 8) != 0)
        return err = L"загрузочный сектор не NTFS", false;
    v.sector = At<WORD>(boot.data(), 0x0B);
    v.cluster = v.sector * boot[0x0D];
    v.mftLcn = At<ULONGLONG>(boot.data(), 0x30);
    signed char cpr = (signed char)boot[0x40];
    v.recSize = cpr > 0 ? (DWORD)cpr * v.cluster : 1u << (-cpr);
    if (v.sector < 512 || v.sector > 4096 || !v.cluster || v.recSize < 1024 || v.recSize > 65536)
        return err = L"странная геометрия тома", false;

    // запись 0 - сам $MFT: где на диске лежат его куски и сколько в нём записей
    DWORD first = std::max(v.recSize, v.sector);
    std::vector<BYTE> rec0(first);
    if (!v.Read(v.mftLcn * v.cluster, rec0.data(), first) || memcmp(rec0.data(), "FILE", 4) != 0 ||
        !Fixup(rec0.data(), v.recSize, v.sector))
        return err = L"запись $MFT не прочиталась", false;
    std::vector<Extent> extents;
    ULONGLONG mftBytes = 0;
    for (DWORD off = At<WORD>(rec0.data(), 0x14); off + 16 <= v.recSize;) {
        const BYTE* a = rec0.data() + off;
        DWORD type = At<DWORD>(a, 0), len = At<DWORD>(a, 4);
        if (type == kAttrEnd || len < 16 || off + len > v.recSize) break;
        if (type == kAttrData && a[8] && a[9] == 0) {
            extents = Extents(a, len);
            mftBytes = At<ULONGLONG>(a, 0x30);
            break;
        }
        off += len;
    }
    if (extents.empty() || !mftBytes) return err = L"куски $MFT не найдены", false;
    ULONGLONG count = mftBytes / v.recSize;
    std::vector<Node> nodes(count);
    std::map<ULONGLONG, std::wstring> names;  // имена папок
    std::vector<std::pair<ULONGLONG, ULONGLONG>> links;

    // проход по MFT кусками по 4 МБ
    constexpr DWORD kChunk = 4 << 20;
    std::unique_ptr<BYTE, decltype(&::_aligned_free)> buf((BYTE*)_aligned_malloc(kChunk, 4096), &::_aligned_free);
    if (!buf) return err = L"нет памяти", false;
    ULONGLONG recNo = 0;
    for (const Extent& e : extents) {
        ULONGLONG bytes = e.clusters * v.cluster;
        if (e.lcn < 0) {  // разреженный кусок: записей там нет
            recNo += bytes / v.recSize;
            continue;
        }
        for (ULONGLONG done = 0; done < bytes && recNo < count;) {
            DWORD part = (DWORD)std::min<ULONGLONG>(kChunk, bytes - done);
            if (!v.Read((ULONGLONG)e.lcn * v.cluster + done, buf.get(), part)) return err = L"ошибка чтения MFT", false;
            for (DWORD o = 0; o + v.recSize <= part && recNo < count; o += v.recSize, ++recNo)
                ParseRecord(buf.get() + o, recNo, v.recSize, v.sector, nodes, kAllDirs, names, links);
            done += part;
        }
    }
    rows = BuildDirs(nodes, names, links);
    head.time = UnixNow();
    head.records = count;
    head.ms =
        (ULONGLONG)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    err = L"записей MFT: " + std::to_wstring(count) + L", папок: " + std::to_wstring(rows.size()) + L", " +
          std::to_wstring(head.ms) + L" мс";
    return true;
}

std::wstring CacheFile(wchar_t letter) {
    return elevate::DataDir() + L"\\mft-" + std::wstring(1, (wchar_t)towupper(letter)) + L".tsv";
}

std::string ReadFileAll(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    std::string s;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (256ll << 20)) {
        s.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr) || got != s.size()) s.clear();
    }
    CloseHandle(f);
    return s;
}

// Папка из таблицы в кэше, если таблица того же тома и не старше maxAge секунд. age - сколько ей секунд.
bool FromCache(const std::wstring& dir, ULONGLONG rec, DWORD serial, ULONGLONG maxAge, std::vector<FolderSize>& out,
               ULONGLONG* age) {
    std::string text = ReadFileAll(CacheFile(dir[0]));
    TableHead head;
    std::vector<DirRow> rows;
    if (text.empty() || !ParseTable(text, rec, head, rows) || head.serial != serial) return false;
    ULONGLONG now = UnixNow(), old = now > head.time ? now - head.time : 0;
    if (old > maxAge || !std::any_of(rows.begin(), rows.end(), [&](const DirRow& d) { return d.rec == rec; }))
        return false;
    if (age) *age = old;
    out = ListDir(rows, rec, dir);
    return true;
}

// Сверка: каждая папка, которую видит Проводник, должна быть в ответе, а файлы прямо в папке - совпасть по числу и
// байтам (так проверен и разбор размеров). Не сошлось - таблица прочиталась не так или устарела. Заодно отмечаются
// папки, куда обходу нет входа (System Volume Information): их размер виден только по MFT.
bool Verify(const std::wstring& dir, std::vector<FolderSize>& got, std::wstring& note) {
    std::wstring base = dir.back() == L'\\' ? dir.substr(0, dir.size() - 1) : dir;
    ULONGLONG looseBytes = 0;
    int looseFiles = 0;
    std::vector<std::wstring> seen;  // папки, которые видны в списке
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((base + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                looseBytes += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                ++looseFiles;
                continue;
            }
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            seen.push_back(n);
            bool found = std::any_of(got.begin(), got.end(),
                                     [&](const FolderSize& f) { return _wcsicmp(f.name.c_str(), n.c_str()) == 0; });
            if (!found) {
                FindClose(h);
                note = L"сверка не сошлась: нет папки " + n;
                return false;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    ULONGLONG mftBytes = 0;
    int mftFiles = 0;
    for (const FolderSize& f : got)
        if (f.path == dir) mftBytes = f.bytes, mftFiles = f.files;
    // пока считали, файл мог дописаться: допуск 1% и мегабайт
    ULONGLONG diff = mftBytes > looseBytes ? mftBytes - looseBytes : looseBytes - mftBytes;
    if (mftFiles != looseFiles || diff > looseBytes / 100 + 1024 * 1024) {
        note = L"сверка не сошлась: файлы в корне " + std::to_wstring(mftFiles) + L" против " +
               std::to_wstring(looseFiles);
        return false;
    }
    for (FolderSize& f : got) {
        if (f.path == dir || !f.files) continue;
        HANDLE k = FindFirstFileExW((f.path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (k != INVALID_HANDLE_VALUE) {
            FindClose(k);
            continue;
        }
        // видна в списке, а сама не открывается - закрыта. Ни код ошибки, ни атрибуты не годятся: на
        // LogFiles\WMI\RtBackup (только для SYSTEM) Windows не отдаёт даже атрибуты папки
        f.closed = std::any_of(seen.begin(), seen.end(),
                               [&](const std::wstring& n) { return _wcsicmp(n.c_str(), f.name.c_str()) == 0; });
    }
    return true;
}

}  // namespace

bool MftSizes(const std::wstring& dirIn, std::vector<FolderSize>& out, std::wstring& err) {
    std::wstring dir = NormDir(dirIn);
    if (dir.size() < 3 || dir[1] != L':') return err = L"нужен путь с буквой диска", false;
    if (!IsNtfs(dir.substr(0, 3))) return err = L"не NTFS", false;
    ULONGLONG target = 0;
    DWORD serial = 0;
    if (!DirRecord(dir, target, serial)) return err = L"папка не открылась", false;
    std::vector<DirRow> rows;
    TableHead head;
    if (!ReadVolume(dir.substr(0, 3), rows, head, err)) return false;
    if (head.serial != serial) return err = L"папка на другом томе (ссылка)", false;
    out = ListDir(rows, target, dir);
    return true;
}

std::wstring MftAdminMain(const std::wstring& dirIn) {
    // том присылает обычный процесс: только буква существующего диска, и только чтение; таблица пишется в папку
    // помощника, куда пишут лишь администраторы
    std::wstring dir = NormDir(dirIn);
    if (dir.size() < 3 || dir[1] != L':' || dir[2] != L'\\' || !iswalpha(dir[0]))
        return L"@mftfail\tнет такого диска\n";
    std::wstring root = dir.substr(0, 3);
    if (GetDriveTypeW(root.c_str()) == DRIVE_NO_ROOT_DIR) return L"@mftfail\tнет такого диска\n";
    std::vector<DirRow> rows;
    TableHead head;
    std::wstring note;
    if (!ReadVolume(root, rows, head, note)) return L"@mftfail\t" + note + L"\n";
    std::string text = FormatTable(head, rows);
    CreateDirectoryW(elevate::DataDir().c_str(), nullptr);
    std::wstring path = CacheFile(root[0]), tmp = path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"@mftfail\tтаблица не записалась\n";
    DWORD w = 0;
    BOOL ok = WriteFile(f, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
    CloseHandle(f);
    // целиком или никак: читающий видит прежнюю таблицу, пока новая не дописана
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return L"@mftfail\tтаблица не записалась\n";
    }
    return L"@mftok\t" + note + L"\n";
}

MftState MftReady() {
    if (elevate::Blocked(elevate::TASK_NET)) return MFT_BLOCKED;
    if (net::AdminReady()) return MFT_READY;
    return elevate::TaskExists(elevate::TASK_NET) ? MFT_OLD : MFT_NONE;
}

bool MftCached(const std::wstring& dirIn, std::vector<FolderSize>& out, bool verify) {
    std::wstring dir = NormDir(dirIn);
    ULONGLONG rec = 0;
    DWORD serial = 0;
    if (dir.size() < 3 || dir[1] != L':' || !DirRecord(dir, rec, serial)) return false;
    std::vector<FolderSize> got;
    std::wstring note;
    if (!FromCache(dir, rec, serial, kFreshSec, got, nullptr) || (verify && !Verify(dir, got, note))) return false;
    out = std::move(got);
    return true;
}

bool ScanSizesMft(const std::wstring& dirIn, std::vector<FolderSize>& out, std::wstring& note,
                  std::atomic<bool>* reading, MftState* state) {
    std::wstring dir = NormDir(dirIn);
    if (dir.size() < 3 || dir[1] != L':') return false;
    if (!IsNtfs(dir.substr(0, 3))) return note = L"не NTFS", false;
    ULONGLONG rec = 0;
    DWORD serial = 0;
    if (!DirRecord(dir, rec, serial)) return false;
    // одна задача за раз: вторая папка подождёт и возьмёт свежую таблицу из кэша
    static std::mutex busy;
    std::lock_guard<std::mutex> hold(busy);
    std::vector<FolderSize> got;
    ULONGLONG age = 0;
    if (FromCache(dir, rec, serial, kFreshSec, got, &age) && Verify(dir, got, note)) {
        out = std::move(got);
        note = L"таблица MFT из кэша, " + std::to_wstring(age / 60) + L" мин назад";
        return true;
    }
    // MFT читается целиком (секунды) при любой папке, а обычный обход небольшой папки - миллисекунды: без свежего
    // кэша задачу зовём только для диска и папок первого уровня (C:\Windows, C:\Users), где обход идёт минутами
    if (dir.size() > 3 && dir.find(L'\\', 3) != std::wstring::npos) return note.clear(), false;
    // без вопросов UAC: только если задача с правами уже есть и её пускают
    MftState st = MftReady();
    if (state) *state = st;
    switch (st) {
        case MFT_BLOCKED:
            return note = L"задачу с правами не пускает антивирус", false;
        case MFT_OLD:
            return note = L"задача с правами устарела после пересборки", false;
        case MFT_NONE:
            return note = L"задача с правами не настроена", false;
        default:
            break;
    }
    if (reading) *reading = true;
    elevate::Put(L"MftDir", dir.substr(0, 3));
    bool answered = false;
    net::RunAdmin(
        net::JOB_MFT_SIZES,
        [&](const std::wstring& s) {
            std::vector<std::wstring> p = Split(s, L'\t');
            if (p.size() >= 2 && (p[0] == L"@mftok" || p[0] == L"@mftfail")) {
                answered = p[0] == L"@mftok";
                note = p[1];
            }
        },
        120000);
    if (reading) *reading = false;
    if (!answered) return false;
    std::wstring summary = note;
    if (!FromCache(dir, rec, serial, kFreshSec, got, nullptr)) return note = L"таблица MFT не прочиталась", false;
    if (!Verify(dir, got, note)) return false;
    out = std::move(got);
    note = summary;
    return true;
}

}  // namespace tools
