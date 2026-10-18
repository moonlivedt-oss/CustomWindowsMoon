// Тесты без окон: правило TEMP, разбор хода чистки, обход папок, разбор MFT и индекс таблицы, сверка MFT, журнал
// автоматики, находки автозапуска, дампы падений, фоновые задачи.
// Сборка и запуск: cmake --build build-check --target radial_tests && ctest --test-dir build-check
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/bg.h"
#include "core/crash.h"
#include "tools/journal.h"
#include "tools/mft_detail.h"
#include "tools/selfwatch.h"
#include "tools/tools.h"

namespace {

int g_failed = 0, g_passed = 0;

void Check(bool ok, const char* what, int line) {
    if (ok) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("FAIL line %d: %s\n", line, what);
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr ULONGLONG kHour = 3600ull * 10000000;

ULONGLONG Now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// Временная папка теста: создаётся пустой, удаляется в конце.
struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        path = std::wstring(base) + L"radial_tests_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~TempDir() { Remove(path); }
    static void Remove(const std::wstring& d) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                std::wstring p = d + L"\\" + n;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    Remove(p);
                else
                    DeleteFileW(p.c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(d.c_str());
    }
};

// Файл заданного размера с временем создания и записи «age назад».
std::wstring MakeFile(const std::wstring& dir, const wchar_t* name, DWORD bytes, ULONGLONG age) {
    std::wstring p = dir + L"\\" + name;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    std::string data(bytes, 'x');
    DWORD put = 0;
    WriteFile(f, data.data(), bytes, &put, nullptr);
    ULONGLONG t = Now() - age;
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
    SetFileTime(f, &ft, &ft, &ft);
    CloseHandle(f);
    return p;
}

tools::WalkFile Probe(const std::wstring& dir, const wchar_t* name, ULONGLONG age) {
    static std::wstring keep;  // WalkFile хранит указатель на папку
    keep = dir;
    tools::WalkFile f;
    f.dir = &keep;
    f.name = name;
    f.nameLen = wcslen(name);
    f.write = f.create = Now() - age;
    return f;
}

void TestParseCleanLine() {
    using tools::CleanLine;
    CleanLine a = tools::ParseCleanLine(L"#start\t2");
    CHECK(a.kind == CleanLine::START && a.index == 2);
    CleanLine b = tools::ParseCleanLine(L"#freed\t1\t1229175546");
    CHECK(b.kind == CleanLine::FREED && b.index == 1 && b.bytes == 1229175546ull);
    CleanLine c = tools::ParseCleanLine(L"#busy\tC:\\Temp\\a\tb.pch");
    CHECK(c.kind == CleanLine::BUSY && c.text == L"C:\\Temp\\a\tb.pch");
    CHECK(tools::ParseCleanLine(L"Чищу: Кэш npm...").kind == CleanLine::LOG);
    CHECK(tools::ParseCleanLine(L"#start\tx").index == (size_t)-1);     // мусор вместо номера - не номер 0
    CHECK(tools::ParseCleanLine(L"#freed\t1").kind == CleanLine::LOG);  // неполная строка - просто журнал
    CHECK(tools::ParseCleanLine(L"").kind == CleanLine::LOG);
}

void TestTempRemovable() {
    TempDir t;
    ULONGLONG now = Now();
    CHECK(!tools::TempRemovable(Probe(t.path, L"fresh.tmp", kHour), now));  // час - свежий
    MakeFile(t.path, L"old.tmp", 10, 25 * kHour);
    CHECK(tools::TempRemovable(Probe(t.path, L"old.tmp", 25 * kHour), now));  // старше суток, никто не держит
    MakeFile(t.path, L"idle.tmp", 10, 7 * kHour);
    CHECK(tools::TempRemovable(Probe(t.path, L"idle.tmp", 7 * kHour), now));  // 7 часов, никто не держит
    std::wstring held = MakeFile(t.path, L"held.tmp", 10, 7 * kHour);
    HANDLE h = CreateFileW(held.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(!tools::TempRemovable(Probe(t.path, L"held.tmp", 7 * kHour), now));  // держат - не трогаем
    CHECK(tools::FileInUse(held));
    CloseHandle(h);
    CHECK(!tools::FileInUse(held));
    MakeFile(t.path, L"preamble-1a2b3c.pch", 10, 2 * kHour);
    CHECK(tools::TempRemovable(Probe(t.path, L"preamble-1a2b3c.pch", 2 * kHour), now));  // кэш clangd - через час
    MakeFile(t.path, L"other.pch", 10, 2 * kHour);
    CHECK(!tools::TempRemovable(Probe(t.path, L"other.pch", 2 * kHour), now));  // обычный .pch - через 6 часов
    CHECK(!tools::TempRemovable(Probe(t.path, L"preamble-x.pch", kHour / 2), now));
}

void TestWalk() {
    TempDir t;
    std::wstring sub = t.path + L"\\a", deep = sub + L"\\b";
    CreateDirectoryW(sub.c_str(), nullptr);
    CreateDirectoryW(deep.c_str(), nullptr);
    MakeFile(t.path, L"1.bin", 1000, 0);
    MakeFile(sub, L"2.bin", 2000, 0);
    MakeFile(deep, L"3.bin", 3000, 0);
    for (int i = 0; i < 300; ++i) MakeFile(deep, (L"m" + std::to_wstring(i)).c_str(), 7, 0);  // больше одного буфера
    int files = 0;
    std::atomic<long> progress{0};
    CHECK(tools::DirBytes(t.path, &files, nullptr, &progress) == 6000 + 300 * 7);
    CHECK(files == 303);
    CHECK(progress == 303);
    std::atomic<bool> cancel{true};
    CHECK(tools::DirBytes(t.path, nullptr, &cancel) == 0);  // отменено до начала
    bool sawName = false;
    tools::WalkFiles(t.path, [&](const tools::WalkFile& f) {
        if (std::wstring(f.name, f.nameLen) == L"3.bin") sawName = f.Path() == deep + L"\\3.bin" && f.size == 3000;
    });
    CHECK(sawName);
    CHECK(tools::DirBytes(t.path + L"\\нет такой") == 0);
    // Н2: папка-ссылка не раскрывается (иначе 3.bin посчитался бы дважды), файл-ссылка идёт в fn
    std::wstring cmd = L"cmd.exe /d /c mklink /J \"" + t.path + L"\\j\" \"" + sub + L"\" >nul";
    _wsystem(cmd.c_str());
    if (tools::LinkDir(GetFileAttributesW((t.path + L"\\j").c_str()))) CHECK(tools::DirBytes(t.path) == 6000 + 300 * 7);
    if (CreateSymbolicLinkW((t.path + L"\\s.lnk").c_str(), (t.path + L"\\1.bin").c_str(),
                            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {  // без режима разработчика - не выйдет
        bool sawLink = false;
        tools::WalkFiles(t.path, [&](const tools::WalkFile& f) {
            if (std::wstring(f.name, f.nameLen) == L"s.lnk") sawLink = (f.attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        });
        CHECK(sawLink);
    }
    CHECK(tools::CloudOnly(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
    CHECK(!tools::CloudOnly(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_ARCHIVE));
    CHECK(tools::LinkDir(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    CHECK(!tools::LinkDir(FILE_ATTRIBUTE_REPARSE_POINT));
}

void TestLiveSize() {
    TempDir t;
    MakeFile(t.path, L"a.bin", 4000, 0);
    MakeFile(t.path, L"b.bin", 6000, 0);
    tools::LiveStart({{L"тест", t.path}});  // первый замер - через 2 с тишины после запуска
    bool ready = false;
    tools::LiveSize got;
    for (int i = 0; i < 50 && !ready; ++i) {
        Sleep(100);
        for (const tools::LiveSize& l : tools::LiveSizes())
            if (l.path == t.path && l.ready) ready = true, got = l;
    }
    tools::LiveStop();
    CHECK(ready);
    CHECK(got.now == 10000);
    CHECK(got.today == 0 && got.hour == 0);  // первый замер - точка отсчёта
}

// ---- MFT: синтетические записи (tools/mft_detail.h), без прав и без тома ----

namespace md = tools::mft_detail;

// Запись MFT 1024 байта, сектор 512: заголовок, массив исправлений на 0x30, атрибуты с 0x38.
struct FakeRec {
    std::vector<BYTE> b = std::vector<BYTE>(1024);
    DWORD off = 0x38;
    explicit FakeRec(WORD flags, ULONGLONG base = 0) {
        memcpy(b.data(), "FILE", 4);
        Put<WORD>(4, 0x30);
        Put<WORD>(6, 3);  // номер обновления + по слову на каждый из двух секторов
        Put<WORD>(0x14, 0x38);
        Put<WORD>(0x16, flags);  // 1 - занята, 2 - папка
        Put<ULONGLONG>(0x20, base);
    }
    template <class T>
    void Put(size_t at, T v) {
        memcpy(b.data() + at, &v, sizeof(T));
    }
    BYTE* Attr(DWORD type, DWORD len, bool nonResident) {
        len = (len + 7) & ~7u;
        BYTE* a = b.data() + off;
        Put<DWORD>(off, type);
        Put<DWORD>(off + 4, len);
        a[8] = nonResident;
        off += len;
        return a;
    }
    void Name(ULONGLONG parent, const std::wstring& name, BYTE ns) {
        DWORD vLen = 0x42 + (DWORD)name.size() * 2;
        BYTE* a = Attr(md::kAttrFileName, 0x18 + vLen, false);
        memcpy(a + 0x10, &vLen, 4);
        WORD vOff = 0x18;
        memcpy(a + 0x14, &vOff, 2);
        BYTE* fn = a + 0x18;
        memcpy(fn, &parent, 8);
        fn[0x40] = (BYTE)name.size();
        fn[0x41] = ns;  // 1 - Win32, 2 - DOS 8.3, 3 - оба сразу
        memcpy(fn + 0x42, name.data(), name.size() * 2);
    }
    void Data(DWORD size) {
        BYTE* a = Attr(md::kAttrData, 0x18, false);
        memcpy(a + 0x10, &size, 4);
    }
    void DataBig(ULONGLONG size, ULONGLONG startVcn = 0) {
        BYTE* a = Attr(md::kAttrData, 0x48, true);
        memcpy(a + 0x10, &startVcn, 8);
        WORD runs = 0x40;
        memcpy(a + 0x20, &runs, 2);
        memcpy(a + 0x30, &size, 8);
    }
    void Reparse() { Attr(md::kAttrReparse, 0x18, false); }
    // конец атрибутов и защита секторов: последние 2 байта сектора - номер обновления, настоящие - в массиве
    std::vector<BYTE> Done(WORD usn = 0x0BAD) {
        Put<DWORD>(off, md::kAttrEnd);
        Put<WORD>(0x30, usn);
        for (int i = 1; i <= 2; ++i) {
            memcpy(b.data() + 0x30 + i * 2, b.data() + i * 512 - 2, 2);
            Put<WORD>(i * 512 - 2, usn);
        }
        return b;
    }
};

void TestMftFixup() {
    FakeRec r(1);
    r.Put<WORD>(510, 0xCDAB);  // настоящие байты на месте конца первого сектора
    r.Put<WORD>(1022, 0x3412);
    std::vector<BYTE> rec = r.Done();
    CHECK(md::At<WORD>(rec.data(), 510) == 0x0BAD);
    CHECK(md::Fixup(rec.data(), 1024, 512));
    CHECK(md::At<WORD>(rec.data(), 510) == 0xCDAB);
    CHECK(md::At<WORD>(rec.data(), 1022) == 0x3412);
    // «разорванная» запись: второй сектор записан не до конца
    std::vector<BYTE> torn = FakeRec(1).Done();
    torn[1022] ^= 0xFF;
    CHECK(!md::Fixup(torn.data(), 1024, 512));
    // массив исправлений за пределами записи
    std::vector<BYTE> bad = FakeRec(1).Done();
    bad[4] = 0xF0, bad[5] = 0x03;
    CHECK(!md::Fixup(bad.data(), 1024, 512));
}

void TestMftExtents() {
    std::vector<BYTE> a(0x60);
    a[0x20] = 0x40;                                     // куски с 0x40
    const BYTE runs[] = {0x21, 0x10, 0x00, 0x10,        // 16 кластеров с 0x1000
                         0x11, 0x08, 0xF0,              // 8 кластеров, смещение -16: с 0x0FF0
                         0x01, 0x04,                    // 4 кластера разреженных
                         0x31, 0x02, 0x10, 0x00, 0x00,  // 2 кластера, +0x10: с 0x1000
                         0x00};
    memcpy(a.data() + 0x40, runs, sizeof(runs));
    std::vector<md::Extent> e = md::Extents(a.data(), (DWORD)a.size());
    CHECK(e.size() == 4);
    if (e.size() != 4) return;
    CHECK(e[0].lcn == 0x1000 && e[0].clusters == 16);
    CHECK(e[1].lcn == 0x0FF0 && e[1].clusters == 8);
    CHECK(e[2].lcn == -1 && e[2].clusters == 4);
    CHECK(e[3].lcn == 0x1000 && e[3].clusters == 2);  // разреженный кусок не сдвигает отсчёт
    // кусок, вылезающий за атрибут, обрывает разбор
    a[0x40] = 0x88;
    CHECK(md::Extents(a.data(), 0x44).empty());
}

void TestMftParseRecord() {
    std::vector<md::Node> nodes(64);
    std::map<ULONGLONG, std::wstring> names;
    std::vector<std::pair<ULONGLONG, ULONGLONG>> links;
    auto parse = [&](FakeRec& r, ULONGLONG no, ULONGLONG target) {
        std::vector<BYTE> rec = r.Done();
        md::ParseRecord(rec.data(), no, 1024, 512, nodes, target, names, links);
    };
    // DOS-имя первым, Win32 - вторым: остаётся длинное, ссылкой не считается
    FakeRec a(1);
    a.Name(5, L"PROGRA~1.TXT", 2);
    a.Name(5, L"Program notes.txt", 1);
    a.Data(300);
    parse(a, 30, 5);
    CHECK(nodes[30].used && !nodes[30].dir && nodes[30].named && !nodes[30].dosName);
    CHECK(names[30] == L"Program notes.txt");
    CHECK(nodes[30].size == 300);
    CHECK(links.empty());
    // наоборот: Win32 первым, DOS после - тоже без ссылки
    FakeRec b(1);
    b.Name(5, L"Long name.txt", 1);
    b.Name(5, L"LONGNA~1.TXT", 2);
    parse(b, 31, 5);
    CHECK(names[31] == L"Long name.txt");
    CHECK(links.empty());
    // жёсткая ссылка: второе Win32-имя в другой папке
    FakeRec c(1);
    c.Name(40, L"a.dll", 1);
    c.Name(41, L"a.dll", 1);
    c.Data(1000);
    parse(c, 32, md::kAllDirs);
    CHECK(nodes[32].parent == 40);
    CHECK(links.size() == 1 && links[0].first == 32 && links[0].second == 41);
    CHECK(!names.count(32));  // файл, а нужны только папки
    // запись-продолжение: размер большого файла в записи 34, основная - 33
    FakeRec base(1);
    base.Name(5, L"big.bin", 1);
    parse(base, 33, 5);
    FakeRec ext(1, 33 | (2ull << 48));  // в ссылке старшие 16 бит - номер повтора
    ext.DataBig(5ull << 30);
    parse(ext, 34, 5);
    CHECK(nodes[33].size == (5ull << 30));
    CHECK(!nodes[34].used);
    // второй кусок того же атрибута размер не перетирает
    FakeRec ext2(1, 33);
    ext2.DataBig(7, 100);
    parse(ext2, 35, 5);
    CHECK(nodes[33].size == (5ull << 30));
    // папка-ссылка: имя есть, отметка reparse
    FakeRec j(3);
    j.Name(5, L"Junction", 1);
    j.Reparse();
    parse(j, 36, md::kAllDirs);
    CHECK(nodes[36].dir && nodes[36].reparse);
    CHECK(names[36] == L"Junction");
    // свободная и испорченная записи не трогают узлы
    FakeRec freeRec(0);
    freeRec.Name(5, L"gone", 1);
    parse(freeRec, 37, 5);
    CHECK(!nodes[37].used && !names.count(37));
    std::vector<BYTE> torn = FakeRec(1).Done();
    torn[510] ^= 1;
    md::ParseRecord(torn.data(), 38, 1024, 512, nodes, 5, names, links);
    CHECK(!nodes[38].used);
}

void TestMftTable() {
    // корень 5; A(24) в корне, B(25) в A, J(26) - ссылка в корне; файлы: 27 в B, 28 в корне, 29 в A и B (ссылка)
    std::vector<md::Node> n(32);
    auto dir = [&](ULONGLONG r, ULONGLONG parent) { n[r].used = n[r].dir = n[r].named = true, n[r].parent = parent; };
    auto file = [&](ULONGLONG r, ULONGLONG parent, ULONGLONG size) {
        n[r].used = n[r].named = true, n[r].parent = parent, n[r].size = size;
    };
    n[5].used = n[5].dir = n[5].named = true, n[5].parent = 5;
    dir(24, 5);
    dir(25, 24);
    dir(26, 5);
    n[26].reparse = true;
    dir(30, 31);  // оторванная от корня петля: 30 и 31 - родители друг друга
    dir(31, 30);
    file(27, 25, 100);
    file(28, 5, 7);
    file(29, 24, 50);
    std::map<ULONGLONG, std::wstring> names = {{24, L"A"}, {25, L"B"}, {26, L"J"}, {30, L"X"}, {31, L"Y"}};
    std::vector<std::pair<ULONGLONG, ULONGLONG>> links = {{29, 25}};
    std::vector<md::DirRow> rows = md::BuildDirs(n, names, links);
    auto row = [&](ULONGLONG r) {
        for (const md::DirRow& d : rows)
            if (d.rec == r) return d;
        return md::DirRow{};
    };
    CHECK(row(24).total == 200 && row(24).totalFiles == 3);  // ссылка считается и в A, и в B
    CHECK(row(25).total == 150 && row(25).looseFiles == 2);
    CHECK(row(5).total == 207 && row(5).loose == 7);
    CHECK(row(30).total == 0);  // петля не уходит в бесконечность
    std::vector<tools::FolderSize> top = md::ListDir(rows, 5, L"C:\\");
    CHECK(top.size() == 2);  // A и файлы в корне; ссылка J не раскрывается
    if (top.size() == 2) {
        CHECK(top[0].name == L"A" && top[0].path == L"C:\\A" && top[0].bytes == 200);
        CHECK(top[1].name == L"(файлы в корне)" && top[1].path == L"C:\\" && top[1].bytes == 7);
    }
    // файл кэша: туда и обратно, только нужная папка и её дети
    md::TableHead head{0xABCD1234, 1760000000, 32, 4300};
    std::string text = md::FormatTable(head, rows);
    md::TableHead h2;
    std::vector<md::DirRow> part;
    CHECK(md::ParseTable(text, 24, h2, part));
    CHECK(h2.serial == 0xABCD1234 && h2.time == 1760000000 && h2.records == 32 && h2.ms == 4300);
    CHECK(part.size() == 2);  // A и B
    std::vector<tools::FolderSize> inA = md::ListDir(part, 24, L"C:\\A");
    CHECK(inA.size() == 2 && inA[0].name == L"B" && inA[0].path == L"C:\\A\\B" && inA[0].bytes == 150);
    CHECK(inA.size() == 2 && inA[1].bytes == 50 && inA[1].files == 1);
    CHECK(md::ParseTable(text, 5, h2, part) && md::ListDir(part, 5, L"C:\\").size() == 2);
    // недописанный файл не принимается
    CHECK(!md::ParseTable(text.substr(0, text.size() - 3), 24, h2, part));
    CHECK(!md::ParseTable("", 24, h2, part));
    // имя не по-английски переживает UTF-8
    for (md::DirRow& d : rows)
        if (d.rec == 25) d.name = L"Загрузки";
    CHECK(md::ParseTable(md::FormatTable(head, rows), 24, h2, part));
    CHECK(md::ListDir(part, 24, L"C:\\A").size() == 2 && md::ListDir(part, 24, L"C:\\A")[0].name == L"Загрузки");
}

// ---- П1: индекс таблицы в памяти даёт те же срезы, что разбор текста ----

void TestMftIndex() {
    std::vector<md::DirRow> rows;
    auto add = [&](ULONGLONG r, ULONGLONG parent, ULONGLONG total, const wchar_t* name, bool reparse = false) {
        md::DirRow d;
        d.rec = r, d.parent = parent, d.total = total, d.totalFiles = (int)(total / 10), d.name = name;
        d.loose = total / 4, d.looseFiles = 1, d.reparse = reparse;
        rows.push_back(d);
    };
    add(5, 5, 1000, L"");
    add(24, 5, 600, L"Users");
    add(25, 24, 400, L"Moon");
    add(26, 25, 100, L"Загрузки");
    add(27, 5, 300, L"Windows");
    add(28, 5, 0, L"Ссылка", true);
    add(29, 27, 120, L"System32");
    std::string text = md::FormatTable({0x1234, 1760000000, 64, 900}, rows);
    md::TableIndex ix;
    CHECK(md::IndexTable(text, ix));
    CHECK(ix.rows.size() == rows.size() && ix.head.serial == 0x1234);
    bool same = true;
    for (ULONGLONG rec : {5ull, 24ull, 25ull, 26ull, 27ull, 29ull, 999ull}) {
        md::TableHead h;
        std::vector<md::DirRow> part;
        md::ParseTable(text, rec, h, part);
        std::wstring base = L"C:\\x";
        std::vector<tools::FolderSize> a = md::ListDir(part, rec, base), b = md::ListDir(md::Slice(ix, rec), rec, base);
        if (a.size() != b.size()) same = false;
        for (size_t i = 0; same && i < a.size(); ++i)
            if (a[i].name != b[i].name || a[i].bytes != b[i].bytes || a[i].files != b[i].files) same = false;
    }
    CHECK(same);
    CHECK(md::Slice(ix, 5).size() == 4);  // корень (сам себе родитель - один раз), Users, Windows, Ссылка
    CHECK(md::Slice(ix, 999).empty());
    CHECK(!md::IndexTable(text.substr(0, text.size() - 2), ix));
}

// ---- Н1: сверка файлов в корне на растущих журналах ----

void TestLooseMatch() {
    const ULONGLONG MB = 1024 * 1024;
    CHECK(md::LooseMatch(10, 500 * MB, 10, 500 * MB));
    CHECK(md::LooseMatch(10, 900 * MB, 10, 20 * MB));    // журнал растёт: MFT видит настоящий размер, список отстаёт
    CHECK(!md::LooseMatch(11, 900 * MB, 10, 900 * MB));  // число файлов - строго
    CHECK(md::LooseMatch(3, 100 * MB, 3, 101 * MB));     // дописался после чтения таблицы: в пределах допуска
    CHECK(!md::LooseMatch(3, 100 * MB, 3, 150 * MB));    // MFT заметно меньше - таблица не та или устарела
    CHECK(md::LooseMatch(0, 0, 0, 0));
}

// ---- вывод DISM /AnalyzeComponentStore ----

void TestStoreSize() {
    const wchar_t* en =
        L"Component Store (WinSxS) information:\r\n\r\n"
        L"Windows Explorer Reported Size of Component Store : 8.12 GB\r\n\r\n"
        L"Actual Size of Component Store : 7.50 GB\r\n\r\n"
        L"    Shared with Windows : 5.10 GB\r\n"
        L"    Backups and Disabled Features : 2.30 GB\r\n";
    CHECK(tools::ParseStoreSize(en) == (ULONGLONG)(7.5 * 1024 * 1024 * 1024));
    const wchar_t* ru =
        L"Размер хранилища компонентов по данным проводника : 8,12 ГБ\r\n"
        L"Фактический размер хранилища компонентов : 512,5 МБ\r\n";
    CHECK(tools::ParseStoreSize(ru) == (ULONGLONG)(512.5 * 1024 * 1024));
    CHECK(tools::ParseStoreSize(L"Ошибка: 740\r\nТребуются повышенные права.") == 0);
    CHECK(tools::ParseStoreSize(L"") == 0);
}

// ---- У2: ход DISM и SFC по процентам в выводе ----

void TestLastPercent() {
    CHECK(tools::LastPercent("Deployment Image Servicing\r\n[==========                 20.0%                  ]\r"
                             "[====================       45.3%                  ]\r") == 45);
    CHECK(tools::LastPercent("[==========================100.0%==========================]\r\n") == 100);
    // SFC пишет в UTF-16: между буквами нули
    std::string sfc;
    for (char ch : std::string("Verification 37% complete.")) sfc += ch, sfc += '\0';
    CHECK(tools::LastPercent(sfc) == 37);
    CHECK(tools::LastPercent("Проверка 8,5% завершена") == 8);
    CHECK(tools::LastPercent("The operation completed successfully.") == -1);
    CHECK(tools::LastPercent("100% is %d and 250%") == 100);  // 250 - не процент хода, берём предыдущий
}

// ---- журнал автоматики: ротация в journal.old.tsv ----

void TestJournal() {
    TempDir t;
    for (int i = 0; i < 40; ++i)
        tools::JournalAdd(t.path, L"18.10 12:" + std::to_wstring(10 + i), L"запись " + std::to_wstring(i), 400);
    WIN32_FILE_ATTRIBUTE_DATA a{}, o{};
    CHECK(GetFileAttributesExW((t.path + L"\\journal.old.tsv").c_str(), GetFileExInfoStandard, &o));
    bool cur = GetFileAttributesExW((t.path + L"\\journal.tsv").c_str(), GetFileExInfoStandard, &a);
    CHECK(!cur || a.nFileSizeLow <= 400 + 64);  // свежий файл после ротации снова мал
    std::vector<tools::JournalEntry> e = tools::JournalRead(t.path, 5);
    CHECK(e.size() == 5);
    if (!e.empty()) CHECK(e[0].text == L"запись 39" && e[0].when == L"18.10 12:49");
    // берём и из старого файла, если в свежем мало
    std::vector<tools::JournalEntry> all = tools::JournalRead(t.path, 100);
    CHECK(all.size() > 5 && all.size() < 40);  // одна старая копия: самое давнее ушло
    bool ordered = true;
    for (size_t i = 1; i < all.size(); ++i)
        if (_wtoi(all[i - 1].text.c_str() + 7) <= _wtoi(all[i].text.c_str() + 7)) ordered = false;
    CHECK(ordered);
}

// ---- «Защита»: находка автозапуска несёт саму запись, перед выключением - сверка со свежим списком ----

void TestStartFind() {
    tools::StartItem a, b, c;
    a.name = L"Updater", a.kind = 0, a.key = L"Updater", a.command = L"C:\\U\\up.exe";
    b.name = L"Telegram", b.kind = 0, b.key = L"Telegram", b.command = L"C:\\T\\tg.exe -autostart";
    c.name = L"Steam", c.kind = 1, c.key = L"Steam.lnk", c.command = L"C:\\S\\steam.exe";
    tools::Suspect s;
    s.isStart = true, s.startItem = b;  // находка взята, когда b стояла второй
    // список обновили: порядок другой, на месте номера 1 теперь чужая запись
    std::vector<tools::StartItem> fresh = {c, a, b};
    const tools::StartItem* f = tools::FindStart(fresh, s.startItem);
    CHECK(f && f->name == L"Telegram");
    CHECK(fresh[1].name != s.startItem.name);  // старый номер указал бы на Updater
    // команду подменили - это уже не та запись
    fresh[2].command = L"C:\\Temp\\evil.exe";
    CHECK(tools::FindStart(fresh, s.startItem) == nullptr);
    // запись убрали
    CHECK(tools::FindStart({a, c}, s.startItem) == nullptr);
    // то же имя из другого места - другая запись
    tools::StartItem d = b;
    d.kind = 1;
    CHECK(!tools::SameStart(d, b));
}

// ---- С3: дампы падений - о каждом говорим один раз, храним не больше kKeep ----

void TestCrashReports() {
    TempDir t;
    auto dump = [&](const wchar_t* stem, const char* what) {
        MakeFile(t.path, (std::wstring(stem) + L".dmp").c_str(), 16, 0);
        if (what) {
            HANDLE f = CreateFileW((t.path + L"\\" + stem + L".txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   0, nullptr);
            DWORD put = 0;
            WriteFile(f, what, (DWORD)strlen(what), &put, nullptr);
            CloseHandle(f);
        }
    };
    dump(L"crash-20261017-093000-11", "C0000005 (обращение к памяти) в RadialMenu.exe+0x10");
    dump(L"crash-20261018-140501-22", nullptr);
    std::vector<crash::Report> r = crash::Unreported(t.path, true);
    CHECK(r.size() == 2);
    if (r.size() == 2) {
        CHECK(r[0].when == L"17.10 09:30" && r[0].what.find(L"RadialMenu.exe+0x10") != std::wstring::npos);
        CHECK(r[1].when == L"18.10 14:05" && r[1].what.empty());
    }
    CHECK(crash::Unreported(t.path, true).empty());  // сказали - больше не говорим
    dump(L"crash-20261019-080000-33", "E0000A11");
    r = crash::Unreported(t.path, false);
    CHECK(r.size() == 1 && r[0].when == L"19.10 08:00");
    CHECK(crash::Unreported(t.path, false).size() == 1);  // без mark отметка не сдвинулась
    for (int i = 0; i < 6; ++i) dump((L"crash-2026102" + std::to_wstring(i) + L"-120000-1").c_str(), "x");
    crash::Prune(t.path, crash::kKeep);
    int dmp = 0, txt = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((t.path + L"\\crash-*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            dmp += n.find(L".dmp") != std::wstring::npos;
            txt += n.find(L".txt") != std::wstring::npos;
            if (n.find(L"crash-20261017") == 0) CHECK(!"самый старый дамп должен уйти");
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    CHECK(dmp == crash::kKeep && txt == crash::kKeep);  // .txt уходят вместе со своими дампами
}

// ---- П3: рост своей памяти и дескрипторов за сутки ----

void TestSelfLeak() {
    const ULONGLONG H = 3600ull * 1000, MB = 1024 * 1024;
    std::vector<tools::SelfSample> s;
    auto at = [&](ULONGLONG hour, ULONGLONG privMb, DWORD handles, DWORD gdi) {
        tools::SelfSample x;
        x.at = 1000 + hour * H, x.priv = privMb * MB, x.handles = handles, x.gdi = gdi, x.user = 100;
        s.push_back(x);
    };
    std::wstring what;
    for (ULONGLONG h = 0; h < 10; ++h) at(h, 100 + h * 50, 900, 300);
    CHECK(!tools::SelfLeak(s, what));  // 10 часов - рано судить, хоть и растёт
    s.clear();
    for (ULONGLONG h = 0; h <= 24; ++h) at(h, 100 + (h % 3) * 20, 900 + (DWORD)(h % 2) * 50, 300);
    CHECK(!tools::SelfLeak(s, what));  // сутки колебаний без роста
    s.clear();
    for (ULONGLONG h = 0; h <= 24; ++h) at(h, 100 + h * 20, 900 + (DWORD)h * 200, 300);
    CHECK(tools::SelfLeak(s, what));  // +480 МБ и +4800 дескрипторов
    CHECK(what.find(L"частные") != std::wstring::npos && what.find(L"дескрипторы +4800") != std::wstring::npos);
    CHECK(what.find(L"GDI") == std::wstring::npos);
    tools::SelfSample me = tools::SampleSelf();
    CHECK(me.priv > 0 && me.handles > 0);
}

void TestBackground() {
    std::atomic<int> hits{0};
    for (int i = 0; i < 8; ++i) CHECK(bg::Run([&] { Sleep(30), ++hits; }));
    bg::Quit(3000);
    CHECK(hits == 8);
    CHECK(bg::Running() == 0);
    CHECK(bg::Quitting());
}

}  // namespace

int wmain() {
    TestParseCleanLine();
    TestTempRemovable();
    TestWalk();
    TestLiveSize();
    TestMftFixup();
    TestMftExtents();
    TestMftParseRecord();
    TestMftTable();
    TestMftIndex();
    TestLooseMatch();
    TestStoreSize();
    TestLastPercent();
    TestJournal();
    TestStartFind();
    TestCrashReports();
    TestSelfLeak();
    TestBackground();  // последним: после Quit задачи видят Quitting()
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
