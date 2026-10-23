#pragma once
// Обновить программы (Л12): что можно обновить через winget и обновление выбранного одной кнопкой. Список -
// «winget upgrade» (вывод разбирается здесь, под тестами); обновление - видимое окно «winget upgrade --id ... -e»:
// соглашения пакета спросит сам winget, права администратора - сама Windows. Программа ничего не принимает за вас.
#include <string>
#include <vector>

namespace tools {

struct Upgrade {
    std::wstring name, id, have, avail;
};

std::vector<Upgrade> ParseWingetUpgrade(const std::wstring& out);
std::wstring WingetExe();  // путь к winget.exe (псевдоним из WindowsApps); пусто - нет
// Список (5-30 с, сеть); false - winget нет или не ответил
bool WingetUpgrades(std::vector<Upgrade>& out);
bool WingetUpgradeWindow(const std::wstring& id);  // окно cmd с обновлением одного пакета

}  // namespace tools
