#pragma once
#include <memory>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

// Общий снимок списка процессов. Игровой режим (раз в 2 с), режим разработки (раз в 3 с) и Boost снимали
// CreateToolhelp32Snapshot каждый сам: теперь один снимок раздаётся всем, пока он не старше maxAgeMs.
namespace procsnap {

using List = std::vector<PROCESSENTRY32W>;
std::shared_ptr<const List> Get(DWORD maxAgeMs = 1500);

}  // namespace procsnap
