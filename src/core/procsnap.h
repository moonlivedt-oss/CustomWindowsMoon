#pragma once
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

// Общий снимок списка процессов. Игровой режим (раз в 2 с), режим разработки (раз в 3 с) и Boost снимали
// CreateToolhelp32Snapshot каждый сам: теперь один снимок раздаётся всем, пока он не старше maxAgeMs.
namespace procsnap {

using List = std::vector<PROCESSENTRY32W>;
std::shared_ptr<const List> Get(DWORD maxAgeMs = 1500);

// Процессы своего сеанса со временем создания, процессором и частной памятью - одним вызовом
// NtQuerySystemInformation, без OpenProcess на каждый (сторожу раз в 5 с это стоило ~25 мс процессора). Пути здесь
// нет: за ним всё равно надо открывать процесс.
struct Info {
    DWORD pid = 0, parent = 0;
    std::wstring name;
    ULONGLONG created = 0;  // FILETIME одним числом
    ULONGLONG cpu = 0;      // ядро + пользователь, 100 нс
    ULONGLONG priv = 0;     // частные байты
};
std::vector<Info> Session();

}  // namespace procsnap
