#pragma once
// Недельный отчёт картинкой (П12): место, рост, что поставилось и пропало, сеть и скорость, падения и батарея за
// неделю - одной PNG в %APPDATA%\MoonDesk\reports, с разницей к прошлому отчёту (его числа - reports\numbers.tsv).
// Данные - из того, что программа уже знает (без долгих обходов). weekreport_logic.cpp - числа и сравнение, под
// тестами; weekreport.cpp - сбор и рисование.
#include <windows.h>

#include <string>
#include <vector>

struct UiColors;

namespace weekreport {

struct Disk {
    std::wstring root;  // корень тома, как у tools::Volume
    ULONGLONG free = 0, total = 0;
};
struct Data {
    LONGLONG when = 0;                                      // секунды unix
    std::wstring from, to;                                  // «16.10», «23.10»
    std::vector<Disk> disks;                                // постоянные диски
    std::vector<std::pair<std::wstring, LONGLONG>> growth;  // что выросло сильнее всего (до 4)
    int added = 0, removed = 0, updated = 0;                // что поставилось / пропало / обновилось за неделю
    std::vector<std::wstring> addedNames;                   // первые из появившегося
    int drops = 0;                                          // обрывов интернета за неделю
    double tunnelDown = 0, directDown = 0;                  // медианы скорости за неделю, Мбит/с; 0 - не мерили
    std::wstring bestExit;                                  // самый быстрый выход туннеля
    int crashes = 0;                                        // падений программы за неделю
    int batteryWear = -1;                                   // износ батареи, %; -1 - нет батареи
};

// --- числа и сравнение (без окон и файлов, под тестами) ---
std::wstring NumbersRow(const Data& d);  // одна строка numbers.tsv
bool ParseNumbers(const std::wstring& line, Data& d);
// Что изменилось к прошлому отчёту, по-человечески: «C: свободно -12 ГБ», «обрывов 5 (было 1)», «через туннель
// быстрее на 20%»; пусто - сравнивать не с чем или всё то же
std::vector<std::wstring> Deltas(const Data& now, const Data& prev);

// --- сбор и картинка ---
Data Collect();  // из фоновых подсчётов, журнала сети, истории скорости, падений; из рабочего потока
Data DemoData(bool previous = false);
bool Render(const Data& now, const Data* prev, const UiColors& c, const std::wstring& png);
// Собрать, сравнить с прошлым отчётом, записать PNG и числа; путь к картинке (пусто - не получилось)
// deltas - сюда же разница к прошлому отчёту (для дайджеста)
std::wstring Make(const UiColors& c, std::vector<std::wstring>* deltas = nullptr);

}  // namespace weekreport
