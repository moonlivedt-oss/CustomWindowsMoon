#pragma once
// Оценка здоровья ПК 0-100 (И31) из того, что программа уже знает: место и прогноз, память, диски (SMART), находки
// «Защиты», износ батареи, ожидание перезагрузки. Штрафы с причинами - видно, за что снято.
#include <string>
#include <vector>

namespace tools {

struct HealthIn {
    struct Disk {
        std::wstring root;
        int freePct = 100;
        int days = -1;  // прогноз «хватит на», -1 - неизвестно
    };
    std::vector<Disk> disks;
    int memLoad = 0;   // %
    int smartBad = 0;  // дисков с предупреждением или хуже
    int smartWarn = 0;
    int danger = 0, odd = 0;  // находки «Защиты»: опасные (6+) и странные (3+)
    int batteryWear = -1;     // % потери ёмкости, -1 - нет батареи
    bool reboot = false;      // Windows ждёт перезагрузки
};

struct HealthOut {
    int score = 100;
    struct Minus {
        int points;
        std::wstring why;
    };
    std::vector<Minus> minus;  // крупные первыми
};

HealthOut HealthScore(const HealthIn& in);

}  // namespace tools
