#pragma once
// Сон после работы (Л6): «когда всё закончится - усыпить» или «выключить». Взводится вручную (трей, студия -
// Помощники), дальше раз в 5 с: идёт ли ещё работа - сборка (cmake, ninja, UBT, шейдеры, MSBuild...), упаковка и
// копирование, рендер Blender (ест процессор), загрузка браузера (свежий .crdownload/.part в Загрузках), загрузка
// Steam (папка steamapps\downloading не пуста), игра; вы за компьютером (мышь или клавиатура за 2 минуты) - тоже ждём.
// Всё затихло на 2 минуты - карточка «Усыплю через 60 с» с «Сейчас» и «Отмена», потом сон или выключение.
// afterwork_logic.cpp - что считается работой и отсчёт (под тестами); afterwork.cpp - снимки и сам сон.
#include <windows.h>

#include <string>
#include <vector>

struct Config;

namespace afterwork {

enum Mode { OFF, SLEEP, SHUTDOWN };

// --- без окон (под тестами) ---
struct ProcNow {
    std::wstring name;  // exe
    double cpu = 0;     // доля всех ядер за прошлый промежуток, 0..1
};
// Что ещё работает (пусто - ничего): процессы по маскам, Blender, который ест процессор (рендер), свежие
// недокачанные файлы, загрузка Steam
std::wstring BusyReason(const std::vector<ProcNow>& procs, const std::wstring& masks, int freshPartials,
                        bool steamDownloading);
// Отсчёт: работа идёт - ждём; затихло на quietMs - отсчёт countMs; дошёл - пора
class Countdown {
public:
    enum Out { WAIT, COUNT, ACT };
    Out Feed(bool busy, ULONGLONG now, ULONGLONG quietMs = 120000, ULONGLONG countMs = 60000);
    int SecondsLeft(ULONGLONG now) const { return actAt_ > now ? (int)((actAt_ - now + 999) / 1000) : 0; }
    void Reset() { idleSince_ = actAt_ = 0; }

private:
    ULONGLONG idleSince_ = 0, actAt_ = 0;
};

// --- в программе ---
int FreshPartials();      // недокачанных файлов в Загрузках, которые писались за 2 минуты (и для Л21)
bool SteamDownloading();  // в steamapps\downloading есть папки игр
void Init(HINSTANCE inst, const Config* cfg);
void Arm(Mode m);  // OFF - отменить
Mode Armed();
std::wstring Status();  // для трея и студии: «жду: идёт сборка UnrealBuildTool», «усыплю через 40 с»
void Tick();            // из общего опроса
void Shutdown();

}  // namespace afterwork
