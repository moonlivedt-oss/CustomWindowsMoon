#pragma once
// Сторожа процессов без окон (под тестами): тихий баланс процессора (И12, как ProBalance), «не спать, пока
// работает» (И13) и долгие команды (И14). Здесь только решения по снимкам; снимки, приоритеты, запрет сна и
// карточки - app/toolsui_guard.cpp.
#include <windows.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace guard {

// имя exe подходит под одну из масок через | (Code.exe|*Editor*.exe), без учёта регистра
bool Match(const std::wstring& name, const std::wstring& masks);

struct Proc {
    DWORD pid = 0, parent = 0;
    std::wstring name;
    ULONGLONG created = 0;  // FILETIME: номер процесса мог достаться другому
    ULONGLONG cpu = 0;      // процессорное время, 100 нс, с начала жизни
};

// Тихий баланс: фоновый процесс съедает больше kHog всех ядер kHogMs подряд, пока пользователь работает за другим
// окном, - временно ниже приоритет. Вернуть - когда он kCalmMs ест меньше kCalm, пользователь ушёл (простой) или
// стал этим окном пользоваться. protect - кого не трогать (система, свои, игра, то, что впереди).
class Balancer {
public:
    static constexpr double kHog = 0.25, kCalm = 0.05;
    static constexpr ULONGLONG kHogMs = 90 * 1000, kCalmMs = 60 * 1000;
    struct Act {
        DWORD pid;
        std::wstring name;
        double share;  // какую долю процессора ел (для журнала)
    };
    // now - мс; cores - число логических процессоров; active - ввод был в последние 2 минуты
    void Feed(ULONGLONG now, const std::vector<Proc>& procs, int cores, bool active, const std::set<DWORD>& protect,
              std::vector<Act>& lower, std::vector<Act>& restore);
    std::vector<Act> Lowered() const;  // кто сейчас понижен

private:
    struct St {
        std::wstring name;
        ULONGLONG created = 0, cpu = 0, at = 0;  // последний замер
        ULONGLONG hogSince = 0, calmSince = 0;
        double share = 0;
        bool lowered = false;
    };
    std::map<DWORD, St> st_;
};

// «Не спать»: идёт ли что-то из списка (сборка, копирование, загрузка Steam). Возвращает, что именно (пусто - нет).
std::wstring KeepAwake(const std::vector<Proc>& procs, const std::wstring& masks);

// Долгие команды: процесс из списка отработал не меньше minMs - событие «закончилось». Вложенные (ninja под cmake,
// cl под MSBuild) не считаются отдельно: сообщает самый внешний.
class LongWatch {
public:
    struct Done {
        DWORD pid;
        std::wstring name;
        ULONGLONG ms;
        std::vector<DWORD> ancestors;  // родитель, его родитель... на момент запуска (окно терминала - среди них)
    };
    // now - FILETIME сейчас (как Proc::created)
    std::vector<Done> Feed(ULONGLONG now, const std::vector<Proc>& procs, const std::wstring& masks, ULONGLONG minMs);
    size_t Tracking() const { return runs_.size(); }

private:
    struct Run {
        std::wstring name;
        ULONGLONG created = 0, seen = 0;
        bool nested = false;
        std::vector<DWORD> ancestors;
    };
    std::map<DWORD, Run> runs_;
};

}  // namespace guard
