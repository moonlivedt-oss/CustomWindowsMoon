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

// Здоровье самой программы (Л27): работает ли то, на чём держится остальное. Снимок собирает студия, здесь -
// только вывод, под тестами.
struct AppHealthIn {
    bool mouseWanted = false;  // перехват мыши кому-то нужен (углы, двойной клик, Alt + колесо)
    bool mouseHooked = false;  // и стоит
    bool mouseSilent = false;  // стоит, курсор двигался, а событий нет - Windows сняла его молча
    bool gameMode = false;     // в игре перехват снят нарочно
    int autostartWanted = -1;  // AutostartWanted: -1 - не задавали
    bool autostartOn = false;
    bool autostartUserOff = false;                                       // выключен в Диспетчере задач
    bool toolsExe = true;                                                // RadialTools.exe рядом
    bool adminExists = false, adminReady = false, adminBlocked = false;  // задача с правами «Сети»
    unsigned long long watchAgeMs = 0;  // сколько слежка молчит; 0 - ещё не прошла ни разу
    unsigned long long watchPeriodMs = 60000;
    unsigned long long uptimeMs = 0;  // сколько живёт программа
};

struct AppPart {
    enum State { OK, OFF, BAD };  // OFF - выключено нарочно или не нужно, не поломка
    std::wstring name;
    State state = OK;
    std::wstring why;
    int fix = 0;  // чем чинить: 0 - ничем, 1 - перехват заново, 2 - автозапуск, 3 - задача с правами
};

std::vector<AppPart> AppHealth(const AppHealthIn& in);
// Одной строкой: «Всё работает: перехват мыши, автозапуск, ...» или «Отвалилось: слежка (молчит 7 мин)»
std::wstring AppHealthLine(const std::vector<AppPart>& parts);

}  // namespace tools
