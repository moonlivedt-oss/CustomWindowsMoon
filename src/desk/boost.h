#pragma once
#include <string>
#include <vector>

#include <windows.h>

// Boost внутри RadialMenu: та же логика и тот же boost.config.json, что у PowerShell-скриптов
// D:\Desktop\components\Boost (они продолжают работать сами по себе).
// Перед игрой закрываются фоновые «пожиратели» памяти из белого списка (мягко, как крестиком,
// потом добиваются), во время игры - таймер 0.5 мс и пониженный приоритет процессов из deprioritize,
// после игры приоритеты возвращаются, память чистится ещё раз, закрытое можно открыть обратно.
// Очистке кэша памяти нужны права администратора: она идёт через задачу планировщика,
// которую один раз создаёт пользователь (RadialMenu.exe --purge с наивысшими правами).
namespace boost {

struct Settings {
    std::vector<std::wstring> whitelist, guarded, deprioritize, areas;
    int gracefulMs = 3000;
    int minFreePercent = 0;
    bool purge = false, trim = false, timer = false, gameDvr = false;
    bool ok = false;  // конфиг прочитан
};

// Папка Boost: из radial.cfg (boost_dir) или рядом с программой (..\Boost).
std::wstring Dir(const std::wstring& configured);
Settings Load(const std::wstring& dir);

// Что можно закрыть: программы из белого списка, по имени, с памятью.
struct Group {
    std::wstring name;   // как в списке процессов, без .exe
    std::wstring title;  // для людей: описание из файла («Microsoft Edge WebView2»), иначе name
    int count = 0;
    double mb = 0;
    std::vector<DWORD> pids;
    std::wstring path;    // путь, по которому программу потом можно открыть снова
    bool reopen = false;  // стоит ли предлагать вернуть (не апдейтеры и не служебные части)
};
std::vector<Group> Scan(const Settings& s);
double TotalMb(const std::vector<Group>& g);

struct Result {
    int closed = 0;
    double freedMb = 0;                // сколько стало свободнее
    std::vector<std::wstring> reopen;  // что можно вернуть после игры
};
// Закрыть (мягко, потом добить), записать boost.session.json, сжать рабочие наборы. Блокирует - звать из потока.
Result Free(const Settings& s, const std::wstring& dir, const std::vector<Group>& groups);
void ClearSession(const std::wstring& dir);
void Reopen(const std::vector<std::wstring>& paths);

// Пока идёт игра: таймер 0.5 мс, пониженный приоритет из deprioritize, один раз - Game DVR.
void GameTweaks(const Settings& s, bool on);

// Свободная память, МБ, и процент занятой.
double AvailMb(int* usedPercent = nullptr);

// Очистка памяти с правами администратора.
bool PurgeTaskExists();
bool SetupPurgeTask();             // создать задачу (Windows спросит разрешение)
bool RunPurge(const Settings& s);  // запустить задачу без запросов (области - из s); false - не готова
int PurgeMain(const Settings& s);  // RadialMenu.exe --purge: сама очистка, уже с правами

}  // namespace boost
