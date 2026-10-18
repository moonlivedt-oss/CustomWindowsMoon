#pragma once
// Разбор записей MFT без чтения тома (mft_parse.cpp): исправления секторов, куски атрибута, записи в узлы и таблица
// размеров всех папок тома. Отдельно от mft.cpp, чтобы тесты проверяли разбор на синтетических записях без прав.
#include <windows.h>

#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "tools/tools.h"

namespace tools::mft_detail {

constexpr ULONGLONG kRefMask = 0x0000FFFFFFFFFFFFull;  // номер записи - младшие 48 бит ссылки
constexpr ULONGLONG kRootRecord = 5;
constexpr ULONGLONG kFirstUserRecord = 24;  // 0-23 - служебные файлы NTFS ($MFT, $Bitmap...)
constexpr ULONGLONG kAllDirs = ~0ull;       // ParseRecord: запоминать имена всех папок, а не детей одной
constexpr DWORD kAttrFileName = 0x30, kAttrData = 0x80, kAttrReparse = 0xC0, kAttrEnd = 0xFFFFFFFF;

template <class T>
T At(const BYTE* p, size_t off) {
    T v;
    memcpy(&v, p + off, sizeof(T));
    return v;
}

// Последние 2 байта каждого сектора записи NTFS подменяет номером обновления (так видна «разорванная» запись),
// настоящие байты лежат в массиве исправлений. false - запись повреждена или не дописана.
bool Fixup(BYTE* rec, DWORD recSize, DWORD sector);

struct Extent {
    LONGLONG lcn;  // -1 - разреженный кусок, на диске его нет
    ULONGLONG clusters;
};
// Куски нерезидентного атрибута (data runs): длина и смещение от предыдущего куска, числа переменной ширины.
std::vector<Extent> Extents(const BYTE* attr, DWORD attrLen);

struct Node {
    ULONGLONG parent = 0, size = 0;
    bool used = false, dir = false, named = false, dosName = false, reparse = false;
};

// Разобрать одну запись в узлы: имя и родитель ($FILE_NAME), размер безымянного потока ($DATA), ссылка ли.
// names - имена записей, которые лежат прямо в папке target (или имена всех папок при target == kAllDirs).
// links - вторые и дальше жёсткие ссылки файла (запись, папка): Windows держит один файл под несколькими именами
// (WinSxS и System32), и обход, и Проводник считают его в каждой папке - считаем так же, иначе цифры не сойдутся с
// подсчётом внутри папки.
void ParseRecord(BYTE* rec, ULONGLONG recNo, DWORD recSize, DWORD sector, std::vector<Node>& nodes, ULONGLONG target,
                 std::map<ULONGLONG, std::wstring>& names, std::vector<std::pair<ULONGLONG, ULONGLONG>>& links);

// Строка таблицы папок тома: total - всё внутри (с подпапками), loose - файлы прямо в папке.
struct DirRow {
    ULONGLONG rec = 0, parent = 0, total = 0, loose = 0;
    int totalFiles = 0, looseFiles = 0;
    bool reparse = false;
    std::wstring name;
};
// Таблица всех папок тома из разобранных узлов (names - с kAllDirs). Файл считается в каждой папке, где лежит его
// имя (жёсткие ссылки), и во всех папках выше; содержимое папок-ссылок на кольцо не раскрывается - там пусто.
std::vector<DirRow> BuildDirs(const std::vector<Node>& nodes, const std::map<ULONGLONG, std::wstring>& names,
                              const std::vector<std::pair<ULONGLONG, ULONGLONG>>& links);
// Что лежит в папке rec: подпапки (без ссылок) и «(файлы в корне)», по убыванию размера; base - путь папки.
std::vector<FolderSize> ListDir(const std::vector<DirRow>& rows, ULONGLONG rec, const std::wstring& base);

// Файл кэша (UTF-8): «#mft serial время записей мс», строки «rec parent total files loose files флаги имя» через
// табуляцию, в конце «#end» - без него файл недописан.
struct TableHead {
    DWORD serial = 0;
    ULONGLONG time = 0;  // секунды с 1970 (UTC)
    ULONGLONG records = 0, ms = 0;
};
std::string FormatTable(const TableHead& head, const std::vector<DirRow>& rows);
// Из текста таблицы - только сама папка rec и её дети (kAllDirs - все строки). false - файл испорчен или недописан.
bool ParseTable(const std::string& text, ULONGLONG rec, TableHead& head, std::vector<DirRow>& rows);

// Вся таблица в памяти: строки и дети каждой папки. Студия ходит по папкам одного тома - переход внутрь берёт
// готовый срез за миллисекунды, а не разбирает 25 МБ текста заново.
struct TableIndex {
    TableHead head;
    std::vector<DirRow> rows;
    std::unordered_map<ULONGLONG, std::vector<size_t>> kids;  // родитель -> номера строк
    std::unordered_map<ULONGLONG, size_t> byRec;              // запись папки -> номер строки
};
bool IndexTable(const std::string& text, TableIndex& ix);
// Папка rec и её дети - то же, что ParseTable(text, rec), но из индекса.
std::vector<DirRow> Slice(const TableIndex& ix, ULONGLONG rec);

// Поиск папок по имени (И8): лучшие max строк - совпало начало имени, начало слова, где-то внутри (раскладка RU/EN
// не важна); при равенстве - что больше по размеру. lower - имена строк в нижнем регистре (LowerNames, один раз).
std::vector<std::wstring> LowerNames(const TableIndex& ix);
std::vector<size_t> FindDirs(const TableIndex& ix, const std::vector<std::wstring>& lower, const std::wstring& query,
                             size_t max);
// Полный путь строки: «C:\Users\...» (root - «C:»); пусто - цепочка родителей оборвалась.
std::wstring PathOf(const TableIndex& ix, size_t row, const std::wstring& root);
// Сверка «файлы прямо в папке»: MFT против списка папки. Число файлов должно совпасть. Байты: MFT видит настоящий
// размер открытых растущих файлов (журналы в System32\LogFiles\WMI), а список папки - отстающий, поэтому MFT больше
// списка - норма; меньше - только в пределах допуска (файл дописался уже после чтения таблицы): 1% и мегабайт.
bool LooseMatch(int mftFiles, ULONGLONG mftBytes, int listFiles, ULONGLONG listBytes);

}  // namespace tools::mft_detail
