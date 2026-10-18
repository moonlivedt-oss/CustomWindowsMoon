// Тесты без окон: правило TEMP, разбор хода чистки, обход папок, разбор MFT, журнал автоматики, фоновые задачи.
// Сборка и запуск: cmake --build build-check --target radial_tests && ctest --test-dir build-check
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/bg.h"
#include "tools/journal.h"
#include "tools/mft_detail.h"
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
    TestStoreSize();
    TestJournal();
    TestBackground();  // последним: после Quit задачи видят Quitting()
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
