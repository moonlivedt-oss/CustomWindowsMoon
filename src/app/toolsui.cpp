#include "app/toolsui.h"

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

#include <shellapi.h>
#include <shlobj.h>

#include "core/config.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "desk/scenecard.h"
#include "gfx/scene.h"
#include "menu/menu.h"
#include "net/net.h"
#include "tools/tools.h"

namespace toolsui {

namespace {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
BoostCard g_card;
std::function<void(int, const std::wstring&)> g_openStudio;
volatile bool g_busy = false;
ULONGLONG g_diskCheck = 0, g_weeklyCheck = 0;
std::set<std::wstring> g_warned;  // о каком диске уже сказали (пока место не вернётся)

// на момент открытия кольца: номер в ключе сектора -> что именно
std::vector<tools::StartItem> g_starts;
std::vector<tools::Junk> g_junk;
std::vector<tools::HeavyApp> g_apps;
std::vector<tools::Listener> g_ports;
std::vector<tools::Which> g_which;
std::vector<tools::ProjectJunk> g_projects;
std::vector<tools::Conflict> g_conflicts;
std::vector<tools::Listener> g_oldPorts;  // серверы разработки старше 6 часов
std::vector<tools::Leftover> g_tails;     // хвосты сборки
std::vector<tools::ProcHistory::Leak> g_leakList;
std::vector<tools::ProcHistory::Hog> g_hogs;
std::vector<std::wstring> g_wake;
std::vector<tools::ToolVersion> g_vers;
std::vector<tools::DupGroup> g_dups;  // под g_scanLock: заполняет поток поиска
ULONGLONG g_dupsAt = 0;
std::vector<tools::DupGroup> g_dupList;     // что показано в кольце (номера dup:i)
std::vector<tools::Suspect> g_finds;        // «Защита»: что показано в кольце
std::vector<tools::StartItem> g_secStarts;  // автозагрузка на момент проверки (номера из Suspect::start)
std::vector<tools::SecurityApp> g_secApps;

// «Защита»: проверка подписей - секунды при первом разе, дальше из кэша; в фоне раз в полчаса.
SRWLOCK g_secLock = SRWLOCK_INIT;
tools::SecurityScan g_sec;
volatile ULONGLONG g_secAt = 0;  // 0 - ещё не проверяли
volatile bool g_secRunning = false;
std::set<std::wstring> g_secWarned;  // о чём уже сказали карточкой (путь или запись)
constexpr int kDangerScore = 6;
constexpr UINT kSecTag = 0x5E;  // wParam у WM_TOOLS_JOB: проверка защиты закончилась
bool g_secAsked = false;        // проверку просили из кольца: итог показать, даже если всё чисто

// Тяжёлое (обход тысяч файлов, версии программ) считается в фоне раз в полчаса: кольцо открывается сразу
// и показывает последнее известное.
struct Scan {
    std::vector<tools::Junk> junk;
    std::vector<tools::ProjectJunk> projects;
    std::vector<tools::Conflict> conflicts;
    std::vector<tools::Growth> growth;
    ULONGLONG ddcOld = 0;  // кэш Unreal, не открывавшийся 30 дней
    tools::Driver driver;
    std::vector<tools::ToolVersion> versions;  // раз в неделю отдельно, как драйвер
};
SRWLOCK g_scanLock = SRWLOCK_INIT;
Scan g_scan;
volatile ULONGLONG g_scanAt = 0;  // 0 - не считалось или нужно пересчитать
ULONGLONG g_started = 0;
volatile bool g_scanRunning = false, g_weeklyRunning = false;
std::vector<std::wstring> g_roots;  // где искать проекты (из колец), обновляется при открытии кольца

constexpr int kDdcDays = 30, kIdleDays = 60;

struct JobMsg {
    std::wstring title, text;
    bool done = false, ok = false;
    bool quiet = false;  // ход и итог показывает сцена, не текстовая карточка
};

ThemeInfo Theme() { return CurrentTheme(*g_cfg); }

Item Act(const std::wstring& name, const wchar_t* icon, const std::wstring& key, const std::wstring& note) {
    Item it;
    it.name = name;
    it.iconFull = ExeDir() + L"\\icons\\" + icon;
    it.special = 3;
    it.path = key;
    it.note = note;
    return it;
}

void RunJob(const std::wstring& title, std::function<bool(const tools::Log&, std::wstring&)> work,
            bool quiet = false) {
    if (g_busy) {
        g_card.Show(BoostCard::DONE, L"Подождите", L"Предыдущее действие ещё идёт.", Theme());
        return;
    }
    g_busy = true;
    if (!quiet) g_card.Show(BoostCard::WORKING, title, L"Начинаю...", Theme());
    struct Box {
        std::wstring title;
        std::function<bool(const tools::Log&, std::wstring&)> work;
        bool quiet;
    };
    auto* box = new Box{title, std::move(work), quiet};
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<Box> b((Box*)p);
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::wstring last;
            tools::Log log = [&](const std::wstring& s) {
                last = s;
                auto* m = new JobMsg{b->title, s, false, false, b->quiet};
                if (!PostMessageW(g_app, WM_TOOLS_JOB, 0, (LPARAM)m)) delete m;
            };
            std::wstring result;
            bool ok = b->work(log, result);
            auto* m = new JobMsg{b->title, result.empty() ? last : result, true, ok, b->quiet};
            if (!PostMessageW(g_app, WM_TOOLS_JOB, 0, (LPARAM)m)) delete m;
            CoUninitialize();
            return 0;
        },
        box, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete box;
        g_busy = false;
    }
}

std::wstring Short(std::wstring s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + L"…" : s; }

// Очистка с живой сценой: места по одному, лента scene::Clean ведёт вихрь в карточке (и в студии, если открыта).
void RunClean(const std::wstring& title, const std::vector<tools::Junk>& list) {
    if (g_busy) return RunJob(title, nullptr);  // скажет «подождите»
    std::vector<scene::CleanItem> items;
    for (const tools::Junk& j : list) items.push_back({j.name, j.bytes});
    scene::CleanBegin(items);
    SceneCard::Get().Show(SceneCard::CLEAN, title, Theme());
    RunJob(
        title,
        [list](const tools::Log& log, std::wstring& res) {
            ULONGLONG freed = 0;
            for (size_t i = 0; i < list.size(); ++i) {
                scene::CleanMark(i, 1);
                ULONGLONG f = tools::CleanJunk(list[i], log);
                scene::CleanMark(i, 2, f);
                freed += f;
            }
            scene::CleanEnd();
            res = L"Освобождено " + tools::Bytes(freed) + L".";
            g_scanAt = 0;  // пересчитать размеры
            return true;
        },
        true);
}

std::wstring NoExe(const std::wstring& s) { return EndsWithI(s, L".exe") ? s.substr(0, s.size() - 4) : s; }

std::wstring Leaf(const std::wstring& p) { return p.substr(p.find_last_of(L'\\') + 1); }

std::wstring Days(int d) { return Plural(d, L"день", L"дня", L"дней"); }

// короткое имя кэша для сектора: «Кэш npm» -> «npm»
std::wstring JunkShort(const std::wstring& n) {
    if (n == L"Временные файлы") return L"Временные";
    if (n == L"Аварийные дампы") return L"Дампы";
    if (n == L"Отчёты об ошибках") return L"Отчёты";
    if (n == L"Архивы Cargo") return L"Cargo";
    if (n == L"Кэш шейдеров DirectX") return L"DirectX";
    if (n == L"Кэш шейдеров NVIDIA") return L"NVIDIA";
    if (n == L"Кэш Unreal Engine") return L"Unreal DDC";
    return n.rfind(L"Кэш ", 0) == 0 ? n.substr(4) : n;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

// Фоновый подсчёт: кэши, кэш Unreal, мусор проектов, конфликты PATH; заодно - снимок размеров для «роста».
void RefreshScan() {
    if (g_scanRunning || !g_cfg) return;  // студия в режиме снимка: утилиты не запущены
    g_scanRunning = true;
    auto* roots = new std::vector<std::wstring>(g_roots.empty() ? ProjectRoots(*g_cfg) : g_roots);
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<std::vector<std::wstring>> r((std::vector<std::wstring>*)p);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);  // фоновый обход диска не мешает работе
            Scan s;
            s.junk = tools::FindJunk(true);
            s.ddcOld = tools::OldBytes(tools::DdcDir(), kDdcDays);
            s.projects = tools::FindProjectJunk(*r);
            s.conflicts = tools::PathConflicts();
            // снимок размеров на сегодня: кэши, мусор проектов по проектам, обычные папки
            std::vector<std::pair<std::wstring, std::wstring>> names;
            std::vector<ULONGLONG> bytes;
            for (const tools::Junk& j : s.junk) names.push_back({j.name, j.path}), bytes.push_back(j.bytes);
            std::vector<std::pair<std::wstring, ULONGLONG>> perProject;
            for (const tools::ProjectJunk& pj : s.projects) {
                auto it = std::find_if(perProject.begin(), perProject.end(),
                                       [&](const auto& x) { return x.first == pj.project; });
                if (it == perProject.end())
                    perProject.push_back({pj.project, pj.bytes});
                else
                    it->second += pj.bytes;
            }
            for (const auto& [proj, b] : perProject)
                names.push_back({L"Мусор " + Leaf(proj), proj + L"\\*сборка"}), bytes.push_back(b);
            for (const KNOWNFOLDERID* id : {&FOLDERID_Downloads, &FOLDERID_Desktop, &FOLDERID_Videos}) {
                std::wstring f = KnownFolder(*id);
                if (f.empty()) continue;
                names.push_back({Leaf(f), f});
                bytes.push_back(tools::DirBytes(f));
            }
            tools::RecordSizes(names, bytes);
            s.growth = tools::WeekGrowth(7);
            AcquireSRWLockExclusive(&g_scanLock);
            s.driver = g_scan.driver;  // драйвер и версии проверяются отдельно
            s.versions = g_scan.versions;
            g_scan = std::move(s);
            g_scanAt = GetTickCount64();
            ReleaseSRWLockExclusive(&g_scanLock);
            g_scanRunning = false;
            return 0;
        },
        roots, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete roots;
        g_scanRunning = false;
    }
}

// Проверка «Защиты» в фоне: подписи, места запуска, автозапуск, перехваты в реестре. Итог - сообщением в окно,
// чтобы карточка об опасном показалась из основного потока.
void RefreshSecurity() {
    if (g_secRunning || !g_app) return;
    g_secRunning = true;
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void*) -> DWORD {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            tools::SecurityScan s = tools::ScanSecurity();
            CoUninitialize();
            AcquireSRWLockExclusive(&g_secLock);
            g_sec = std::move(s);
            g_secAt = GetTickCount64();
            ReleaseSRWLockExclusive(&g_secLock);
            g_secRunning = false;
            PostMessageW(g_app, WM_TOOLS_JOB, kSecTag, 0);
            return 0;
        },
        nullptr, 0, nullptr);
    if (th)
        CloseHandle(th);
    else
        g_secRunning = false;
}

std::wstring FindKey(const tools::Suspect& s) {
    return s.pid ? Lower(s.path) : s.where + L"|" + s.name + L"|" + s.path;
}

// После фоновой проверки: об опасном (счёт 6+) - карточка, про каждую находку один раз.
void OnSecurity() {
    if (g_busy) return;
    AcquireSRWLockShared(&g_secLock);
    std::vector<tools::Suspect> all = g_sec.hijacks;
    all.insert(all.end(), g_sec.procs.begin(), g_sec.procs.end());
    all.insert(all.end(), g_sec.autos.begin(), g_sec.autos.end());
    for (const auto* v : {&g_sec.tasks, &g_sec.exts, &g_sec.nets}) all.insert(all.end(), v->begin(), v->end());
    ReleaseSRWLockShared(&g_secLock);
    for (const tools::Suspect& s : all) {
        if (s.score < kDangerScore || !g_secWarned.insert(FindKey(s)).second) continue;
        g_card.Show(BoostCard::DONE, L"Защита: " + s.name, s.why + L". Подробнее - кольцо «Утилиты», «Защита».",
                    Theme(), 30000, Toast::G_WARN);
        return;
    }
}

// Карточка-вопрос с двумя действиями; подписи кнопок после ответа возвращаются к обычным.
void Ask(const std::wstring& title, const std::wstring& sub, const std::wstring& yes, const std::wstring& no,
         std::function<void()> onYes, std::function<void()> onNo = nullptr, int hold = 20000,
         Toast::Glyph glyph = Toast::G_NONE) {
    auto reset = [] {
        g_card.yesLabel.clear();
        g_card.noLabel.clear();
    };
    g_card.yesLabel = yes;
    g_card.noLabel = no;
    g_card.onYes = [reset, onYes] {
        reset();
        if (onYes) onYes();
    };
    g_card.onNo = [reset, onNo] {
        reset();
        if (onNo) onNo();
    };
    g_card.Show(BoostCard::OFFER, title, sub, Theme(), hold, glyph);
}

// ---------------- слежка раз в минуту ----------------
// Свой поток: снимок процессов копится в истории (утечки, фоновый процессор), трафик физических адаптеров,
// выделенная память, раз в 5 минут - новый автозапуск. Карточки показывает основной поток, по одной и не в игре.

SRWLOCK g_watchLock = SRWLOCK_INIT;
tools::ProcHistory g_hist;
std::vector<tools::ProcInfo> g_procs;  // последний снимок
std::vector<tools::ProcHistory::Leak> g_leaks;
std::vector<tools::StartItem> g_newStarts;  // ждут вопроса
std::vector<std::wstring> g_wakeCache;      // кто будит: powercfg раз в полчаса, не при открытии кольца
tools::CommitInfo g_commit;
double g_netDown = 0, g_netUp = 0;  // байт/с за последнюю минуту
HANDLE g_watchStop = nullptr, g_watchThread = nullptr;
constexpr UINT kWatchTag = 0x5F;  // wParam у WM_TOOLS_JOB: новый снимок слежки

bool g_game = false, g_dev = false;
ULONGLONG g_devEnded = 0;  // когда закрылся Unreal: через 2 минуты - смотреть хвосты
std::set<std::wstring> g_leakWarned;
ULONGLONG g_commitWarned = 0, g_proactiveAt = 0;

DWORD WINAPI WatchLoop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ULONGLONG lastIn = 0, lastOut = 0, lastAt = 0;
    int n = 0;
    do {
        std::vector<tools::ProcInfo> ps = tools::Processes();
        ULONGLONG in = 0, out = 0, now = GetTickCount64();
        tools::NetCounters(in, out);
        tools::CommitInfo c = tools::Commit();
        std::vector<tools::StartItem> fresh;
        if (n % 5 == 0) fresh = tools::NewStartupItems();
        std::vector<std::wstring> wake;
        bool wakeNew = n % 30 == 0;
        if (wakeNew) wake = tools::WakeDevices();
        AcquireSRWLockExclusive(&g_watchLock);
        g_hist.Feed(ps, now);
        g_procs = std::move(ps);
        g_leaks = g_hist.Leaks();
        g_commit = c;
        if (lastAt && in >= lastIn && out >= lastOut && now > lastAt) {
            double dt = (now - lastAt) / 1000.0;
            g_netDown = (in - lastIn) / dt;
            g_netUp = (out - lastOut) / dt;
        }
        g_newStarts.insert(g_newStarts.end(), fresh.begin(), fresh.end());
        if (wakeNew) g_wakeCache = std::move(wake);
        ReleaseSRWLockExclusive(&g_watchLock);
        lastIn = in, lastOut = out, lastAt = now;
        PostMessageW(g_app, WM_TOOLS_JOB, kWatchTag, 0);
        ++n;
    } while (WaitForSingleObject(g_watchStop, 60000) == WAIT_TIMEOUT);
    CoUninitialize();
    return 0;
}

// программа целиком (все процессы с этим именем) - для мягкого закрытия, как в секторе «Процессы»
tools::HeavyApp AppByName(const std::wstring& name) {
    tools::HeavyApp a;
    a.name = name;
    AcquireSRWLockShared(&g_watchLock);
    for (const tools::ProcInfo& p : g_procs)
        if (_wcsicmp(p.name.c_str(), name.c_str()) == 0) a.pids.push_back(p.pid), a.bytes += p.mem;
    ReleaseSRWLockShared(&g_watchLock);
    return a;
}

void KillLeftovers(const std::vector<tools::Leftover>& tails) {
    RunJob(L"Хвосты сборки", [tails](const tools::Log&, std::wstring& res) {
        int ok = 0;
        ULONGLONG freed = 0;
        for (const tools::Leftover& t : tails)
            if (tools::KillPid(t.pid, t.name)) ++ok, freed += t.mem;
        res = L"Завершено " + std::to_wstring(ok) + L" из " + std::to_wstring(tails.size()) + L", освободилось " +
              tools::Bytes(freed) + L" памяти.";
        return ok > 0;
    });
}

void OpenPagefile() {
    ShellExecuteW(nullptr, L"open", L"SystemPropertiesPerformance.exe", nullptr, nullptr, SW_SHOWNORMAL);
    g_card.Show(BoostCard::DONE, L"Файл подкачки",
                L"Вкладка «Дополнительно», «Виртуальная память» - «Изменить». Для 16 ГБ памяти и Unreal: размер "
                L"по выбору системы или вручную 16-24 ГБ на SSD. Нужны права, применится после перезагрузки.",
                Theme(), 30000, Toast::G_INFO);
}

// По одной карточке: новый автозапуск, утечка, память на пределе, хвосты Unreal. Не в игре и не поверх другой.
void TryProactive() {
    ULONGLONG now = GetTickCount64();
    if (g_game || g_busy || g_card.Visible() || now - g_proactiveAt < 5000) return;
    AcquireSRWLockExclusive(&g_watchLock);
    bool haveStart = !g_newStarts.empty();
    tools::StartItem st = haveStart ? g_newStarts.front() : tools::StartItem{};
    if (haveStart) g_newStarts.erase(g_newStarts.begin());
    std::vector<tools::ProcHistory::Leak> leaks = g_leaks;
    tools::CommitInfo c = g_commit;
    std::vector<tools::ProcInfo> procs = g_dev || !g_devEnded ? std::vector<tools::ProcInfo>{} : g_procs;
    ReleaseSRWLockExclusive(&g_watchLock);
    g_proactiveAt = now;
    if (haveStart) {
        Ask(
            L"Новое в автозапуске: " + st.name,
            st.where + L": " + Short(st.command, 120) + L". Будет запускаться при каждом входе.", L"Оставить",
            st.canToggle ? L"Выключить" : L"Понятно", nullptr,
            [st] {
                if (!st.canToggle) return;
                std::wstring msg;
                bool ok = tools::SetStartup(st, false, [&](const std::wstring& t) { msg = t; });
                g_card.Show(BoostCard::DONE, ok ? L"Автозагрузка" : L"Не получилось", msg, Theme());
            },
            40000, Toast::G_INFO);
        return;
    }
    for (const tools::ProcHistory::Leak& l : leaks) {
        std::wstring key = std::to_wstring(l.pid) + L"|" + Lower(l.name);
        if (!g_leakWarned.insert(key).second) continue;
        std::wstring name = l.name;
        Ask(
            L"Растёт память: " + NoExe(l.name),
            L"Было " + tools::Bytes(l.was) + L" " + std::to_wstring(l.minutes) + L" мин назад, сейчас " +
                tools::Bytes(l.now) + L", и рост не останавливается. Похоже на утечку - помогает перезапуск.",
            L"Закрыть", L"Не сейчас", [name] { CloseHeavy(AppByName(name)); }, nullptr, 40000, Toast::G_WARN);
        return;
    }
    if (c.limit && c.total * 100 >= c.limit * 90 && (!g_commitWarned || now - g_commitWarned > 6 * 3600 * 1000ull)) {
        g_commitWarned = now;
        Ask(
            L"Память на пределе",
            L"Занято " + tools::Bytes(c.total) + L" из " + tools::Bytes(c.limit) + L" (память и подкачка " +
                tools::Bytes(tools::PagefileBytes()) +
                L"). Дальше программы начнут падать, Unreal - при сборке шейдеров.",
            L"Что закрыть", L"Подкачка", [] { MemoryOffer(L"память на пределе", {}, true); }, [] { OpenPagefile(); },
            40000, Toast::G_WARN);
        return;
    }
    if (g_devEnded && now - g_devEnded > 2 * 60 * 1000) {
        g_devEnded = 0;
        std::vector<tools::Leftover> tails;
        ULONGLONG mem = 0;
        for (const tools::Leftover& t : tools::BuildLeftovers(procs))
            if (t.why.find(L"Unreal") != std::wstring::npos) tails.push_back(t), mem += t.mem;
        if (tails.empty()) return;
        Ask(
            L"Unreal закрыт, а его процессы остались",
            Plural((int)tails.size(), L"процесс", L"процесса", L"процессов") + L" (" + tails[0].name +
                (tails.size() > 1 ? L" и другие" : L"") + L") держат " + tools::Bytes(mem) + L" памяти.",
            L"Завершить", L"Оставить", [tails] { KillLeftovers(tails); }, nullptr, 30000, Toast::G_INFO);
    }
}

// ---------------- раз в неделю: рост, здоровье SSD, драйвер ----------------
// weekly.tsv: что<TAB>день последней проверки

int Today() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / (24ull * 3600 * 10000000));
}

int LastDay(const std::wstring& what) {
    wchar_t v[32] = L"";
    GetPrivateProfileStringW(L"weekly", what.c_str(), L"0", v, 32, (tools::DataDir() + L"\\weekly.ini").c_str());
    return _wtoi(v);
}

void SetDay(const std::wstring& what, int day) {
    WritePrivateProfileStringW(L"weekly", what.c_str(), std::to_wstring(day).c_str(),
                               (tools::DataDir() + L"\\weekly.ini").c_str());
}

struct WeeklyMsg {
    std::wstring growth, health;
    tools::Driver driver;
    std::vector<tools::ToolVersion> versions;
    bool versionsChecked = false;
};
constexpr UINT kWeeklyTag = 0x57;  // wParam у WM_TOOLS_JOB: итог недельной проверки

void Weekly() {
    if (g_weeklyRunning) return;
    int today = Today();
    bool growth = today - LastDay(L"growth") >= 7, health = today - LastDay(L"health") >= 7,
         driver = today - LastDay(L"driver") >= 3, versions = today - LastDay(L"versions") >= 7;
    if (!growth && !health && !driver && !versions) return;
    g_weeklyRunning = true;
    struct Want {
        bool growth, health, driver, versions;
    };
    auto* w = new Want{growth, health, driver, versions};
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<Want> want((Want*)p);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto* m = new WeeklyMsg;
            int today = Today();
            if (want->growth) {
                // за неделю выросло больше гигабайта - сказать, что именно
                std::vector<tools::Growth> g = tools::WeekGrowth(7);
                for (size_t i = 0; i < g.size() && i < 3; ++i)
                    if (g[i].delta >= 1024ll * 1024 * 1024)
                        m->growth +=
                            (m->growth.empty() ? L"" : L", ") + g[i].name + L" +" + tools::Bytes((ULONGLONG)g[i].delta);
                // нужны замеры хотя бы за несколько дней, иначе неделя ещё не прошла
                if (!g.empty()) SetDay(L"growth", today);
            }
            if (want->health) {
                std::vector<tools::Physical> d = tools::PhysicalDisks();
                if (net::AdminReady()) tools::DiskHealth(d, [](const std::wstring&) {});
                m->health = tools::HealthWeekly(d);
                SetDay(L"health", today);
            }
            if (want->driver) {
                m->driver = tools::NvidiaDriver();
                if (m->driver.ok) SetDay(L"driver", today);
            }
            if (want->versions) {
                m->versions = tools::ToolVersions();
                m->versionsChecked = true;
                if (!m->versions.empty()) SetDay(L"versions", today);
            }
            CoUninitialize();
            if (!PostMessageW(g_app, WM_TOOLS_JOB, kWeeklyTag, (LPARAM)m)) delete m;
            g_weeklyRunning = false;
            return 0;
        },
        w, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete w;
        g_weeklyRunning = false;
    }
}

void OnWeekly(WeeklyMsg* m) {
    if (m->driver.ok) {
        AcquireSRWLockExclusive(&g_scanLock);
        g_scan.driver = m->driver;
        ReleaseSRWLockExclusive(&g_scanLock);
    }
    std::wstring fresh, freshKey;  // что вышло новее; про один и тот же набор - один раз
    if (m->versionsChecked) {
        AcquireSRWLockExclusive(&g_scanLock);
        g_scan.versions = m->versions;
        ReleaseSRWLockExclusive(&g_scanLock);
        for (const tools::ToolVersion& v : m->versions)
            if (v.newer) {
                fresh += (fresh.empty() ? L"" : L"; ") + v.name + L": " + v.note;
                freshKey += v.name + v.latest;
            }
    }
    if (g_busy) return;
    // по одной карточке за раз: важнее здоровье диска, потом драйвер, потом рост
    if (!m->health.empty()) {
        g_card.Show(BoostCard::DONE, L"Диск: стоит посмотреть", m->health, Theme(), 20000, Toast::G_WARN);
    } else if (m->driver.newer && LastDay(L"driver-said-" + m->driver.latest) == 0) {
        SetDay(L"driver-said-" + m->driver.latest, Today());  // про одну версию - один раз
        tools::Driver d = m->driver;
        Ask(
            L"Драйвер NVIDIA " + d.latest,
            L"Установлен " + d.installed + (d.date.empty() ? L"" : L", новый вышел " + d.date) +
                L". Обновить можно в NVIDIA App - сам ничего не ставлю.",
            L"Открыть NVIDIA App", L"Позже",
            [] {
                std::wstring app = L"C:\\Program Files\\NVIDIA Corporation\\NVIDIA App\\CEF\\NVIDIA App.exe";
                if (GetFileAttributesW(app.c_str()) != INVALID_FILE_ATTRIBUTES)
                    ShellExecuteW(nullptr, L"open", app.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                else
                    ShellExecuteW(nullptr, L"open", L"https://www.nvidia.com/drivers/", nullptr, nullptr,
                                  SW_SHOWNORMAL);
            },
            nullptr, 30000, Toast::G_INFO);
    } else if (!fresh.empty() &&
               LastDay(L"versions-said-" + std::to_wstring(std::hash<std::wstring>{}(freshKey))) == 0) {
        SetDay(L"versions-said-" + std::to_wstring(std::hash<std::wstring>{}(freshKey)), Today());
        g_card.Show(BoostCard::DONE, L"Вышли обновления",
                    fresh + L". Ничего не ставлю сам; ссылки - в кольце «Утилиты», «PATH».", Theme(), 25000,
                    Toast::G_INFO);
    } else if (!m->growth.empty()) {
        g_card.Show(BoostCard::DONE, L"За неделю выросло", m->growth + L". Почистить - кольцо «Утилиты».", Theme(),
                    14000, Toast::G_INFO);
    }
}

}  // namespace

// ---------------- корни проектов ----------------

std::vector<std::wstring> ProjectRoots(const Config& cfg) {
    std::vector<std::wstring> roots;
    std::wstring profile = Lower(KnownFolder(FOLDERID_Profile)), downloads = Lower(KnownFolder(FOLDERID_Downloads));
    auto ok = [&](const std::wstring& p) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
        std::wstring l = Lower(p);
        while (l.size() > 3 && l.back() == L'\\') l.pop_back();
        // корни дисков, профиль целиком, загрузки и системное - слишком широко
        return l.size() > 3 && l != profile && l != downloads && l.find(L"\\windows") == std::wstring::npos &&
               l.find(L"\\program files") == std::wstring::npos && l.find(L"\\appdata") == std::wstring::npos;
    };
    std::function<void(const std::vector<Item>&)> walk = [&](const std::vector<Item>& items) {
        for (const Item& it : items) {
            if (it.dir && ok(it.full)) roots.push_back(it.full);
            walk(it.children);
        }
    };
    walk(cfg.left);
    walk(cfg.right);
    for (const std::wstring& p : {profile + L"\\source\\repos", KnownFolder(FOLDERID_Documents) + L"\\Unreal Projects",
                                  std::wstring(L"D:\\Unreal Projects")})
        if (ok(p)) roots.push_back(p);
    // вложенные корни не нужны: обход и так зайдёт внутрь
    for (std::wstring& r : roots) {
        while (r.size() > 3 && r.back() == L'\\') r.pop_back();
    }
    std::sort(roots.begin(), roots.end(),
              [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
    std::vector<std::wstring> out;
    for (const std::wstring& r : roots)
        if (out.empty() || Lower(r).rfind(Lower(out.back()) + L"\\", 0) != 0) {
            if (!out.empty() && Lower(out.back()) == Lower(r)) continue;
            out.push_back(r);
        }
    return out;
}

void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below,
          std::function<void(int, const std::wstring&)> openStudio) {
    g_app = app;
    g_cfg = cfg;
    g_openStudio = std::move(openStudio);
    g_card.below = below;
    g_card.Create(inst);
    g_diskCheck = GetTickCount64() - 9 * 60 * 1000;  // первая проверка места - через минуту после старта
    g_started = GetTickCount64();
    g_weeklyCheck = g_started;
    g_watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_watchThread = CreateThread(nullptr, 0, WatchLoop, nullptr, 0, nullptr);
}

void OnModes(bool game, bool dev) {
    if (g_dev && !dev) g_devEnded = GetTickCount64();  // Unreal закрыт: через пару минут - не остались ли воркеры
    if (dev) g_devEnded = 0;
    g_game = game;
    g_dev = dev;
}

namespace {

std::wstring Level(int score) { return score >= kDangerScore ? L"Опасно: " : L"Странно: "; }

void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_app)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

// Отчёт VirusTotal по хэшу: сам файл никуда не уходит, открывается только страница.
void OpenVirusTotal(const std::wstring& path) {
    RunJob(L"VirusTotal", [path](const tools::Log& log, std::wstring& res) {
        log(L"Считаю SHA-256 файла...");
        std::wstring h = tools::FileSha256(path);
        if (h.empty()) {
            res = L"Файл не прочитать: " + path;
            return false;
        }
        std::wstring url = L"https://www.virustotal.com/gui/file/" + h;
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        res =
            L"Открыл отчёт по хэшу, сам файл никуда не отправлен. Если VirusTotal его не знает, загрузить можно "
            L"вручную на той же странице.";
        return true;
    });
}

void SecurityAction(const tools::Suspect& s) {
    bool file = !s.path.empty() && GetFileAttributesW(s.path.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring facts = s.why + L". " + s.path + (s.signer.empty() ? L"" : L", подпись «" + s.signer + L"»") +
                         (s.parent.empty() ? L"" : L", запустил " + s.parent);
    if (s.pid) {
        tools::Suspect c = s;
        Ask(
            Level(s.score) + NoExe(s.name) + L" (PID " + std::to_wstring(s.pid) + L")", facts,
            file ? L"VirusTotal" : L"Понятно", L"Завершить",
            [c, file] {
                if (file) OpenVirusTotal(c.path);
            },
            [c] {
                Ask(L"Завершить " + c.name + L"?",
                    L"PID " + std::to_wstring(c.pid) + L", " + c.path +
                        L". Несохранённое в нём пропадёт. Если это вирус, он может запуститься снова - проверь "
                        L"автозапуск и Kaspersky.",
                    L"Завершить", L"Не надо", [c] {
                        RunJob(L"Завершаю " + NoExe(c.name), [c](const tools::Log&, std::wstring& res) {
                            bool ok = tools::KillPid(c.pid, c.name);
                            res = ok ? c.name + L" завершён."
                                     : L"Не далось: процесс уже закрылся, запущен с правами или защищён.";
                            return ok;
                        });
                    });
            },
            30000, Toast::G_WARN);
        return;
    }
    if (s.start >= 0 && s.start < (int)g_secStarts.size()) {
        tools::StartItem it = g_secStarts[s.start];
        std::wstring path = s.path;
        Ask(
            Level(s.score) + it.name + L" при входе", it.where + L": " + Short(it.command, 120) + L". " + s.why,
            file ? L"VirusTotal" : L"Понятно", it.canToggle ? L"Выключить" : L"Закрыть",
            [path, file] {
                if (file) OpenVirusTotal(path);
            },
            [it] {
                if (!it.canToggle) return;
                std::wstring msg;
                bool ok = tools::SetStartup(it, false, [&](const std::wstring& t) { msg = t; });
                g_card.Show(BoostCard::DONE, ok ? L"Автозагрузка" : L"Не получилось",
                            msg + (ok ? L" Включить обратно - в студии или Диспетчере задач." : L""), Theme());
            },
            30000, Toast::G_WARN);
        return;
    }
    if (s.kind == tools::FIND_TASK) {
        std::wstring task = s.where, path = s.path;
        Ask(
            Level(s.score) + L"задача " + s.name, s.why + L". Запускает: " + Short(s.parent, 140),
            file ? L"VirusTotal" : L"Понятно", L"Выключить",
            [path, file] {
                if (file) OpenVirusTotal(path);
            },
            [task] {
                std::wstring msg;
                bool ok = tools::SetTaskEnabled(task, false, [&](const std::wstring& t) { msg = t; });
                g_card.Show(BoostCard::DONE, ok ? L"Задача выключена" : L"Не получилось", msg, Theme());
            },
            30000, Toast::G_WARN);
        return;
    }
    if (s.kind == tools::FIND_EXT) {
        std::wstring dir = s.path;
        Ask(
            Level(s.score) + L"расширение " + s.name,
            s.where + L": " + s.why + L". Убрать - на странице расширений браузера (меню - Расширения).",
            L"Показать папку", L"Понятно",
            [dir] { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }, nullptr, 30000,
            Toast::G_WARN);
        return;
    }
    if (s.kind == tools::FIND_NET) {
        bool hosts = s.name.rfind(L"hosts", 0) == 0, dns = s.name.rfind(L"DNS", 0) == 0;
        std::wstring where = s.where;
        Ask(
            Level(s.score) + s.name,
            s.why + (hosts ? L". Править hosts можно только с правами (Блокнот от администратора)."
                     : dns ? L". Вернуть автоматический DNS: свойства адаптера - IPv4."
                           : L". Выключить: Параметры - Сеть - Прокси."),
            hosts ? L"Папка hosts"
            : dns ? L"Адаптеры"
                  : L"Прокси",
            L"Понятно",
            [hosts, dns, where] {
                if (hosts)
                    ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + where + L"\"").c_str(), nullptr,
                                  SW_SHOWNORMAL);
                else
                    ShellExecuteW(nullptr, L"open", dns ? L"ncpa.cpl" : L"ms-settings:network-proxy", nullptr, nullptr,
                                  SW_SHOWNORMAL);
            },
            nullptr, 30000, Toast::G_WARN);
        return;
    }
    // находка в реестре: исправляется с правами, сам не трогаю
    std::wstring where = s.where;
    Ask(
        Level(s.score) + s.name,
        s.why + L". Значение: " + s.path +
            L". Править - с правами администратора: скопирую путь к ключу, вставь его в адресную строку regedit.",
        L"Открыть regedit", L"Понятно",
        [where] {
            CopyText(where);  // адресная строка regedit понимает и HKLM\..., и HKCU\...
            ShellExecuteW(nullptr, L"open", L"regedit.exe", nullptr, nullptr, SW_SHOWNORMAL);
        },
        nullptr, 30000, Toast::G_WARN);
}

// «Защита»: находки последней проверки, антивирус, починка Windows и свои программы из папки security_dir.
Item SecurityItem() {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_secLock);
    bool checked = g_secAt != 0;
    tools::SecurityScan sec = g_sec;
    ReleaseSRWLockShared(&g_secLock);
    if (!checked || now - g_secAt > 10 * 60 * 1000) RefreshSecurity();

    g_finds.clear();
    g_finds.insert(g_finds.end(), sec.hijacks.begin(), sec.hijacks.end());
    g_finds.insert(g_finds.end(), sec.procs.begin(), sec.procs.end());
    g_finds.insert(g_finds.end(), sec.autos.begin(), sec.autos.end());
    for (const auto* v : {&sec.tasks, &sec.nets, &sec.exts}) g_finds.insert(g_finds.end(), v->begin(), v->end());
    g_secStarts = sec.starts;
    int danger = 0;
    for (const tools::Suspect& s : g_finds) danger += s.score >= kDangerScore;

    std::wstring checkedNote = L"Проверено " + Plural(sec.processes, L"программа", L"программы", L"программ") + L" и " +
                               Plural(sec.autoruns, L"запись", L"записи", L"записей") + L" автозапуска";
    Item item = Act(L"Защита", danger ? L"tools-bug.png" : L"tools-shield.png", L"",
                    !checked          ? L"Подписи, места запуска, автозапуск и перехваты в реестре; проверяю..."
                    : g_finds.empty() ? checkedNote + L": ничего странного"
                    : danger          ? L"Опасного: " + std::to_wstring(danger) + L", всего находок " +
                                   std::to_wstring(g_finds.size()) + L". " + checkedNote
                             : L"Странного: " + std::to_wstring(g_finds.size()) + L". " + checkedNote);
    for (size_t i = 0; i < g_finds.size() && i < 12; ++i) {
        const tools::Suspect& s = g_finds[i];
        static const wchar_t* kWhat[] = {
            L"процесс", L"автозапуск", L"реестр", L"задача планировщика", L"расширение браузера", L"сеть"};
        std::wstring what = kWhat[s.kind];
        item.children.push_back(
            Act(Short(NoExe(s.name), 14), L"tools-bug.png", L"sec-find:" + std::to_wstring(i),
                Level(s.score) + what + L". " + s.why + L". " + Short(s.path, 90) + L". 2 клика - что с этим сделать"));
    }
    if (checked && g_finds.empty())
        item.children.push_back(
            Act(L"Чисто", L"tools-shield.png", L"",
                checkedNote + L". Без подписи " + std::to_wstring(sec.unsignedCount) +
                    L" - это обычно (свои сборки, мелкие программы); странными они становятся по месту и имени"));
    if (!checked)
        item.children.push_back(Act(L"Проверка", L"tools-shield.png", L"", L"Первая проверка подписей идёт в фоне"));
    item.children.push_back(Act(L"Заново", L"tools-shield.png", L"sec-rescan",
                                L"Ещё раз проверить запущенное, автозапуск и реестр; итог - карточкой"));
    std::wstring av = tools::AntivirusUi();
    item.children.push_back(Act(av.empty() ? L"Defender" : L"Kaspersky", L"tools-shield.png", L"sec-av",
                                av.empty() ? L"Открыть «Защиту от вирусов» Windows - там полная и офлайн-проверка"
                                           : L"Открыть Kaspersky - там полная проверка компьютера"));
    item.children.push_back(Act(L"Снимок", L"tools-shield.png", L"sec-restore",
                                L"Точка восстановления системы - перед драйвером, правками реестра или "
                                L"«Починить Windows»; нужны права"));
    item.children.push_back(Act(L"Починить Windows", L"tools-box.png", L"sec-repair",
                                L"Сверить системные файлы (SFC) или восстановить их из Windows Update (DISM); "
                                L"нужны права - Windows спросит"));
    g_secApps = tools::SecurityApps(g_cfg->securityDir);
    for (size_t i = 0; i < g_secApps.size(); ++i) {
        Item a = Act(g_secApps[i].name, L"", L"sec-app:" + std::to_wstring(i), g_secApps[i].exe);
        a.iconFull.clear();  // значок самой программы
        a.full = g_secApps[i].exe;
        item.children.push_back(a);
    }
    return item;
}

}  // namespace

void ShowRing(RadialMenu& menu) {
    g_roots = ProjectRoots(*g_cfg);
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_scanLock);
    bool sized = g_scanAt != 0;
    Scan scan = g_scan;
    ReleaseSRWLockShared(&g_scanLock);
    if (!sized || now - g_scanAt > 5 * 60 * 1000) RefreshScan();
    g_junk = sized ? scan.junk : tools::FindJunk(false);
    g_projects = scan.projects;
    g_conflicts = scan.conflicts;

    std::vector<Item> ring;
    std::wstring status;
    // диски: место, здоровье, рост за неделю, драйвер
    Item disks = Act(L"Диски", L"tools-disk.png", L"", L"Место на дисках, здоровье, что выросло за неделю");
    for (const tools::Volume& v : tools::Volumes()) {
        std::wstring letter = v.root.substr(0, 2);
        int pct = v.total ? (int)(v.free * 100 / v.total) : 0;
        disks.children.push_back(
            Act(letter + L" " + tools::Bytes(v.free), L"tools-size.png", L"scan:" + v.root,
                L"Свободно " + tools::Bytes(v.free) + L" из " + tools::Bytes(v.total) + L" (" + std::to_wstring(pct) +
                    L"%)" + (v.label.empty() ? L"" : L", «" + v.label + L"»") + L". 2 клика - что занимает место"));
        status += (status.empty() ? L"" : L"   |   ") + letter + L" свободно " + tools::Bytes(v.free);
    }
    disks.children.push_back(
        Act(L"Здоровье", L"tools-disk.png", L"health", L"Износ, температура и часы работы SSD; раз в неделю - сам"));
    for (size_t i = 0, shown = 0; i < scan.growth.size() && shown < 3; ++i) {
        const tools::Growth& g = scan.growth[i];
        if (g.delta < 200ll * 1024 * 1024) break;
        disks.children.push_back(Act(L"+" + tools::Bytes((ULONGLONG)g.delta) + L" " + Short(g.name, 12),
                                     L"tools-size.png", L"",
                                     g.name + L" выросло за неделю на " + tools::Bytes((ULONGLONG)g.delta) +
                                         L", сейчас " + tools::Bytes(g.now)));
        ++shown;
    }
    if (scan.driver.ok)
        disks.children.push_back(Act(scan.driver.newer ? L"Драйвер " + scan.driver.latest : L"Драйвер свежий",
                                     L"tools-box.png", scan.driver.newer ? L"driver" : L"",
                                     scan.driver.gpu + L": установлен " + scan.driver.installed + L", новейший " +
                                         scan.driver.latest +
                                         (scan.driver.newer ? L". 2 клика - открыть NVIDIA App" : L"")));
    ring.push_back(disks);

    // очистка: размеры из фонового подсчёта
    ULONGLONG junkTotal = 0, quickTotal = 0;  // всё и то, что пересоздаётся быстро (без шейдеров)
    int quick = 0;
    for (const tools::Junk& j : g_junk) {
        junkTotal += j.bytes;
        if (!j.slow) quickTotal += j.bytes, ++quick;
    }
    ULONGLONG binItems = 0, bin = tools::RecycleBinBytes(&binItems);
    Item clean = Act(L"Очистка", L"tools-clean.png", L"",
                     sized ? L"Можно освободить " + tools::Bytes(quickTotal + scan.ddcOld + bin) +
                                 L": кэши, временные файлы, старое из кэша Unreal, корзина"
                           : L"Временные файлы, кэши и корзина; размеры ещё считаются");
    if (sized && quick > 1)
        clean.children.push_back(Act(L"Всё сразу " + tools::Bytes(quickTotal), L"tools-clean.png", L"clean-all",
                                     L"Кэши и временные файлы (свежее суток не трогаю), кроме кэшей шейдеров; "
                                     L"занятое пропускается"));
    if (scan.ddcOld > 256ull * 1024 * 1024)
        clean.children.push_back(Act(L"Unreal: старое " + tools::Bytes(scan.ddcOld), L"tools-clean.png", L"ddc-trim",
                                     L"Из кэша Unreal - то, что не открывалось " + Days(kDdcDays) +
                                         L". Шейдеры текущих проектов остаются, пересборки не будет"));
    for (size_t i = 0; i < g_junk.size(); ++i)
        clean.children.push_back(
            Act(JunkShort(g_junk[i].name) + (sized ? L" " + tools::Bytes(g_junk[i].bytes) : L""), L"tools-clean.png",
                L"clean:" + std::to_wstring(i),
                g_junk[i].name + L": " + g_junk[i].path +
                    (g_junk[i].slow ? L". Соберётся заново: первые запуски игр и редактора будут медленнее (спрошу)"
                                    : L". Программа скачает или создаст заново")));
    if (bin)
        clean.children.push_back(
            Act(L"Корзина " + tools::Bytes(bin), L"tools-clean.png", L"bin",
                Plural((int)binItems, L"объект", L"объекта", L"объектов") + L". Очистить насовсем (спрошу ещё раз)"));
    AcquireSRWLockShared(&g_scanLock);
    std::vector<tools::DupGroup> dups = g_dups;
    bool dupsKnown = g_dupsAt != 0;
    ReleaseSRWLockShared(&g_scanLock);
    for (size_t i = 0; i < dups.size() && i < 6; ++i) {
        const tools::DupGroup& g = dups[i];
        ULONGLONG extra = g.size * (g.paths.size() - 1);
        clean.children.push_back(
            Act(L"Копии " + Short(Leaf(g.paths[0]), 10) + L" " + tools::Bytes(extra), L"tools-dup.png",
                L"dup:" + std::to_wstring(i),
                Plural((int)g.paths.size(), L"одинаковый файл", L"одинаковых файла", L"одинаковых файлов") + L" по " +
                    tools::Bytes(g.size) + L". Останется " + g.paths[0] + L". 2 клика - копии в Корзину (спрошу)"));
    }
    clean.children.push_back(
        Act(L"Дубли", L"tools-dup.png", L"dup-scan",
            std::wstring(dupsKnown ? (dups.empty() ? L"Дублей от 20 МБ не было. " : L"Искал недавно. ") : L"") +
                L"Одинаковые файлы от 20 МБ в Загрузках, на Рабочем столе, в Документах, Видео, Изображениях и "
                L"Музыке. Папки проектов и библиотеки ассетов не трогаю"));
    g_dupList = dups;
    ring.push_back(clean);

    // проекты: пересобираемый мусор (node_modules, Intermediate, target, .venv, build)
    ULONGLONG projTotal = 0, idleTotal = 0;
    int idleCount = 0;
    for (const tools::ProjectJunk& p : g_projects) {
        projTotal += p.bytes;
        if (p.idleDays >= kIdleDays) idleTotal += p.bytes, ++idleCount;
    }
    Item projects = Act(L"Проекты", L"tools-box.png", L"",
                        !sized               ? L"Мусор сборки в проектах; ещё считается"
                        : g_projects.empty() ? L"Мусора сборки в проектах не нашлось"
                                             : L"Мусор сборки в проектах: " + tools::Bytes(projTotal) +
                                                   L"; соберётся заново командой сборки или установки");
    if (idleCount > 1)
        projects.children.push_back(Act(L"Давние " + tools::Bytes(idleTotal), L"tools-clean.png", L"proj-idle",
                                        L"Проекты без правок дольше " + Days(kIdleDays) + L": " +
                                            std::to_wstring(idleCount) + L" папок мусора (спрошу)"));
    for (size_t i = 0; i < g_projects.size() && i < 14; ++i) {
        const tools::ProjectJunk& p = g_projects[i];
        projects.children.push_back(Act(Short(p.name, 12) + L" " + tools::Bytes(p.bytes), L"tools-clean.png",
                                        L"proj:" + std::to_wstring(i),
                                        p.kind + L": " + p.path + L". Правок " +
                                            (p.idleDays ? Days(p.idleDays) + L" не было" : std::wstring(L"сегодня")) +
                                            L". 2 клика - удалить (спрошу)"));
    }
    if (projects.children.empty())
        projects.children.push_back(Act(L"Чисто", L"tools-box.png", L"", L"Искал в папках из колец"));
    ring.push_back(projects);

    // PATH: проблемы, конфликты версий и что найдётся первым
    int dead = 0, dup = 0;
    for (const tools::PathEntry& e : tools::ReadPath()) {
        if (!e.system && e.dead) ++dead;
        if (!e.system && e.dup) ++dup;
    }
    Item path =
        Act(L"PATH", L"tools-path.png", L"",
            dead + dup || !g_conflicts.empty() ? L"Лишних записей: " + std::to_wstring(dead + dup) +
                                                     L", конфликтов версий: " + std::to_wstring(g_conflicts.size())
                                               : L"Чисто; ниже - какая команда найдётся первой");
    if (dead + dup)
        path.children.push_back(Act(
            L"Почистить " + std::to_wstring(dead + dup), L"tools-clean.png", L"path-clean",
            L"Мёртвых " + std::to_wstring(dead) + L", дублей " + std::to_wstring(dup) + L"; старый PATH - в бэкап"));
    for (size_t i = 0; i < g_conflicts.size(); ++i) {
        const tools::Conflict& c = g_conflicts[i];
        path.children.push_back(
            Act(c.cmd + L" " + c.winnerVer + L" > " + c.loserVer, L"tools-env.png", L"conflict:" + std::to_wstring(i),
                c.cmd + L" " + c.winnerVer + L" (" + c.winner + L") затеняет " + c.loserVer + L" (" + c.loser + L")" +
                    (c.fixable ? L". 2 клика - поднять " + c.loserVer + L" выше"
                               : L". Первая папка в системном PATH - поменять можно только с правами")));
    }
    g_which.clear();
    for (const tools::Which& w : tools::WhichAll())
        if (w.cmd == L"python" || w.cmd == L"pip" || w.cmd == L"node" || w.cmd == L"g++" || w.cmd == L"cmake" ||
            w.cmd == L"git") {
            path.children.push_back(
                Act(w.cmd, L"tools-env.png", L"which:" + std::to_wstring(g_which.size()),
                    w.path + (w.other.empty() ? L"" : L"; затенено ещё " + std::to_wstring(w.other.size())) +
                        L". 2 клика - показать в Проводнике"));
            g_which.push_back(w);
        }
    g_vers.clear();
    std::wstring installed;
    for (const tools::ToolVersion& v : scan.versions) {
        if (!v.installed.empty()) installed += (installed.empty() ? L"" : L", ") + v.name + L" " + v.installed;
        if (!v.newer) continue;
        path.children.push_back(Act(v.name + L" " + v.latest, L"tools-update.png",
                                    L"ver:" + std::to_wstring(g_vers.size()),
                                    v.note + L". Установлен " + (v.installed.empty() ? L"?" : v.installed) + L" (" +
                                        v.path + L"). 2 клика - откуда обновить"));
        g_vers.push_back(v);
    }
    path.children.push_back(Act(L"Версии", L"tools-update.png", L"ver-check",
                                (scan.versions.empty() ? std::wstring(L"Ещё не проверялось. ") : installed + L". ") +
                                    L"Проверить сейчас, что вышло новее (раз в неделю - само)"));
    path.children.push_back(Act(L"Добавить", L"tools-path.png", L"path-add",
                                L"Добавить в PATH папку из буфера (скопируй её в Проводнике)"));
    ring.push_back(path);

    // порты: с проектом, который их занял
    g_ports.clear();
    std::vector<tools::Listener> ls = tools::Listeners();
    tools::ListenerProjects(ls);
    Item ports = Act(L"Порты", L"tools-ports.png", L"", L"Кто занял порты разработки и из какого проекта");
    g_oldPorts.clear();
    for (const tools::Listener& l : ls)
        if (l.dev && g_ports.size() < 12) {
            std::wstring who = l.project.empty() ? NoExe(l.process) : Leaf(l.project);
            int age = tools::ProcessMinutes(l.pid);
            // сервер разработки, забытый с прошлого раза: работает дольше 6 часов
            if (age >= 360 && !tools::ProtectedExe(l.process)) g_oldPorts.push_back(l);
            ports.children.push_back(
                Act(std::to_wstring(l.port) + L" " + Short(who, 14), L"tools-ports.png",
                    L"port:" + std::to_wstring(g_ports.size()),
                    std::wstring(l.udp ? L"UDP " : L"TCP ") + std::to_wstring(l.port) + L", " + l.process + L" (PID " +
                        std::to_wstring(l.pid) + L")" + (l.project.empty() ? L"" : L", проект " + l.project) +
                        (l.command.empty() ? L"" : L": " + l.command) +
                        (age >= 60 ? L", работает " + std::to_wstring(age / 60) + L" ч" : std::wstring()) +
                        L". 2 клика - открыть или завершить"));
            g_ports.push_back(l);
        }
    if (!g_oldPorts.empty())
        ports.children.push_back(Act(L"Старые " + std::to_wstring(g_oldPorts.size()), L"tools-clean.png", L"ports-old",
                                     L"Серверы разработки, работающие дольше 6 часов - скорее всего забытые с прошлого "
                                     L"раза. 2 клика - завершить (спрошу)"));
    if (ports.children.empty())
        ports.children.push_back(Act(L"Свободно", L"tools-ports.png", L"studio:3", L"Порты разработки никто не занял"));
    ring.push_back(ports);

    // процессы: память и самые тяжёлые программы
    tools::MemoryState mem = tools::Memory();
    g_apps = tools::HeavyApps(8);
    Item procs = Act(L"Процессы", L"tools-box.png", L"",
                     L"Свободно " + tools::Bytes(mem.avail) + L" из " + tools::Bytes(mem.total) +
                         L" памяти; ниже - самые тяжёлые программы");
    for (size_t i = 0; i < g_apps.size(); ++i)
        procs.children.push_back(Act(Short(NoExe(g_apps[i].name), 14) + L" " + tools::Bytes(g_apps[i].bytes),
                                     L"tools-box.png", L"app:" + std::to_wstring(i),
                                     Plural((int)g_apps[i].pids.size(), L"процесс", L"процесса", L"процессов") +
                                         L", память " + tools::Bytes(g_apps[i].bytes) +
                                         L". 2 клика - закрыть (спрошу)"));
    AcquireSRWLockShared(&g_watchLock);
    std::vector<tools::ProcInfo> snap = g_procs;
    g_leakList = g_leaks;
    tools::CommitInfo cm = g_commit;
    bool hogsReady = g_hist.Ready(10);
    std::vector<tools::ProcHistory::Hog> hogs = g_hist.CpuHogs(10, true);
    double down = g_netDown, up = g_netUp;
    ReleaseSRWLockShared(&g_watchLock);
    if (snap.empty()) snap = tools::Processes();  // слежка ещё не успела
    if (!cm.limit) cm = tools::Commit();
    g_tails = tools::BuildLeftovers(snap);
    if (!g_tails.empty()) {
        ULONGLONG tm = 0;
        std::wstring names;
        for (const tools::Leftover& t : g_tails) {
            tm += t.mem;
            if (names.size() < 90) names += (names.empty() ? L"" : L", ") + NoExe(t.name);
        }
        procs.children.push_back(Act(L"Хвосты " + std::to_wstring(g_tails.size()), L"tools-clean.png", L"tails",
                                     L"Остались после сборки: " + names + L"; держат " + tools::Bytes(tm) +
                                         L". 2 клика - завершить все (спрошу)"));
    }
    for (size_t i = 0; i < g_leakList.size() && i < 3; ++i)
        procs.children.push_back(Act(
            L"Растёт " + Short(NoExe(g_leakList[i].name), 10), L"tools-bug.png", L"leak:" + std::to_wstring(i),
            L"Память: " + tools::Bytes(g_leakList[i].was) + L" -> " + tools::Bytes(g_leakList[i].now) + L" за " +
                std::to_wstring(g_leakList[i].minutes) + L" мин, ровно и без остановки. 2 клика - закрыть (спрошу)"));
    procs.children.push_back(Act(L"Подкачка", L"tools-size.png", L"pagefile",
                                 L"Выделено " + tools::Bytes(cm.total) + L" из " + tools::Bytes(cm.limit) +
                                     L" (память " + tools::Bytes(cm.phys) + L" + подкачка " +
                                     tools::Bytes(tools::PagefileBytes()) + L"), пик с загрузки " +
                                     tools::Bytes(cm.peak) + L". 2 клика - настройки подкачки"));
    ring.push_back(procs);

    // фон: батарея, кто ест процессор без окна, кто качает, что мешает сну
    tools::BatteryInfo bat = tools::Battery();
    wchar_t speed[96];
    swprintf(speed, 96, L"сеть за минуту: приём %ls/с, отдача %ls/с", tools::Bytes((ULONGLONG)down).c_str(),
             tools::Bytes((ULONGLONG)up).c_str());
    Item bg = Act(L"Фон", L"tools-battery.png", L"", std::wstring(L"Что работает без окна и тратит батарею; ") + speed);
    if (bat.present && bat.percent >= 0) {
        std::wstring note =
            bat.ac ? (bat.charging ? L"Заряжается" : L"От сети")
                   : L"От батареи" + (bat.minutesLeft > 0 ? L", осталось " + std::to_wstring(bat.minutesLeft / 60) +
                                                                L" ч " + std::to_wstring(bat.minutesLeft % 60) + L" мин"
                                                          : std::wstring());
        if (bat.designed && bat.full)
            note += L". Держит " + std::to_wstring(bat.full / 1000) + L"." + std::to_wstring(bat.full % 1000 / 100) +
                    L" из " + std::to_wstring(bat.designed / 1000) + L" Вт*ч по паспорту, износ " +
                    std::to_wstring(bat.full >= bat.designed ? 0 : (int)(100 - bat.full * 100 / bat.designed)) + L"%";
        if (bat.cycles > 0) note += L", циклов " + std::to_wstring(bat.cycles);
        bg.children.push_back(Act(L"Батарея " + std::to_wstring(bat.percent) + L"%", L"tools-battery.png", L"", note));
    }
    g_hogs.clear();
    for (const tools::ProcHistory::Hog& h : hogs)
        if (h.percent >= 1.0 && g_hogs.size() < 6) g_hogs.push_back(h);
    for (size_t i = 0; i < g_hogs.size(); ++i) {
        wchar_t pct[16];
        swprintf(pct, 16, L"%.1f%%", g_hogs[i].percent);
        bg.children.push_back(Act(Short(NoExe(g_hogs[i].name), 10) + L" " + pct, L"tools-box.png",
                                  L"hog:" + std::to_wstring(i),
                                  std::wstring(L"В среднем ") + pct + L" всего процессора за 10 минут, без окна; " +
                                      Plural((int)g_hogs[i].pids.size(), L"процесс", L"процесса", L"процессов") +
                                      L", память " + tools::Bytes(g_hogs[i].mem) + L". 2 клика - закрыть (спрошу)"));
    }
    if (!hogsReady)
        bg.children.push_back(Act(L"Собираю", L"tools-box.png", L"",
                                  L"Процессор в фоне считается за 10 минут, история копится с запуска RadialMenu"));
    else if (g_hogs.empty())
        bg.children.push_back(Act(L"Тихо", L"tools-box.png", L"",
                                  L"За 10 минут ни одна программа без окна не съела больше 1% процессора"));
    bg.children.push_back(Act(L"Кто качает", L"tools-ports.png", L"net-top",
                              std::wstring(L"Трафик по программам за 3 секунды (TCP; QUIC браузеров не видно). ") +
                                  speed + (net::AdminReady() ? L"" : L". Нужна задача с правами (спрошу)")));
    bg.children.push_back(Act(L"Мешает сну", L"tools-battery.png", L"power",
                              std::wstring(L"Кто не даёт уснуть или выключить экран и кто будит по таймеру") +
                                  (net::AdminReady() ? L"" : L". Нужна задача с правами (спрошу)")));
    AcquireSRWLockShared(&g_watchLock);
    g_wake = g_wakeCache;
    ReleaseSRWLockShared(&g_watchLock);
    if (!g_wake.empty()) {
        std::wstring list;
        for (const std::wstring& w : g_wake) list += (list.empty() ? L"" : L", ") + w;
        bg.children.push_back(Act(L"Будят " + std::to_wstring(g_wake.size()), L"tools-battery.png", L"wake",
                                  L"Могут разбудить из сна: " + list + L". 2 клика - Диспетчер устройств"));
    }
    ring.push_back(bg);

    // автозагрузка: что включено и сколько памяти держит
    g_starts = tools::StartupItems();
    tools::StartupWeight(g_starts);
    std::vector<size_t> order;
    for (size_t i = 0; i < g_starts.size(); ++i)
        if (g_starts[i].enabled) order.push_back(i);
    std::sort(order.begin(), order.end(), [](size_t a, size_t b) { return g_starts[a].memory > g_starts[b].memory; });
    ULONGLONG startMem = 0;
    for (size_t i : order) startMem += g_starts[i].memory;
    Item starts = Act(L"Автозагрузка", L"tools-box.png", L"",
                      Plural((int)order.size(), L"программа", L"программы", L"программ") + L" при входе, держат " +
                          tools::Bytes(startMem) + L" памяти");
    for (size_t i : order) {
        const tools::StartItem& s = g_starts[i];
        starts.children.push_back(Act(
            Short(s.name, 14) + (s.memory ? L" " + tools::Bytes(s.memory) : L""), L"tools-box.png",
            L"start-off:" + std::to_wstring(i),
            s.where + L": " + Short(s.command, 90) +
                (s.running ? L". Сейчас работает, " + tools::Bytes(s.memory) : std::wstring(L". Сейчас не запущена")) +
                (s.canToggle ? L". 2 клика - выключить (спрошу)" : L". Выключить - в Диспетчере задач (нужны права)")));
    }
    ring.push_back(starts);

    ring.push_back(Act(L"Терминал", L"tools-env.png", L"term",
                       L"Терминал в папке открытого Проводника (или из буфера) с окружением проекта: venv, MSYS2, "
                       L"node_modules, Unreal"));
    ring.push_back(Act(L"Кто держит", L"tools-lock.png", L"lock-clip",
                       L"Кто не даёт удалить файл или папку: скопируй её в Проводнике и выбери это"));
    ring.push_back(SecurityItem());
    ring.push_back(Act(L"Все утилиты", L"tools-box.png", L"studio:0", L"PATH, диски, автозагрузка и порты в студии"));
    if (sized && quickTotal + scan.ddcOld + bin > 0)
        status += L"   |   можно освободить " + tools::Bytes(quickTotal + scan.ddcOld + bin);
    if (projTotal) status += L"   |   мусор проектов " + tools::Bytes(projTotal);
    status += L"   |   память свободна " + tools::Bytes(mem.avail);

    menu.onNet = [](const std::wstring& key) { Run(key); };
    menu.netStatus = [status] { return status; };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForWindow(g_app);
    menu.ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

void Run(const std::wstring& key) {
    auto idx = [&](size_t prefix, size_t size) {
        size_t i = (size_t)_wtoi(key.c_str() + prefix);
        return i < size ? i : (size_t)-1;
    };
    if (key.rfind(L"studio:", 0) == 0) {
        if (g_openStudio) g_openStudio(_wtoi(key.c_str() + 7), L"");
        return;
    }
    if (key.rfind(L"scan:", 0) == 0) {
        if (g_openStudio) g_openStudio(1, key.substr(5));
        return;
    }
    if (key == L"health")
        return RunJob(L"Здоровье дисков", [](const tools::Log& log, std::wstring& res) {
            std::vector<tools::Physical> d = tools::PhysicalDisks();
            bool ok = tools::DiskHealth(d, log);
            for (const tools::Physical& p : d) {
                res += (res.empty() ? L"" : L"\n") + Short(p.name, 28) + L": " +
                       (p.health == 0   ? L"исправен"
                        : p.health == 1 ? L"предупреждение"
                                        : L"плохо");
                if (p.wear >= 0) res += L", износ " + std::to_wstring(p.wear) + L"%";
                if (p.temp > 0) res += L", " + std::to_wstring(p.temp) + L" °C";
            }
            return ok;
        });
    if (key == L"driver") {
        std::wstring app = L"C:\\Program Files\\NVIDIA Corporation\\NVIDIA App\\CEF\\NVIDIA App.exe";
        ShellExecuteW(nullptr, L"open",
                      GetFileAttributesW(app.c_str()) != INVALID_FILE_ATTRIBUTES ? app.c_str()
                                                                                 : L"https://www.nvidia.com/drivers/",
                      nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (key == L"clean-all") {
        std::vector<tools::Junk> quick;
        for (const tools::Junk& j : g_junk)
            if (!j.slow) quick.push_back(j);
        return RunClean(L"Очистка", quick);
    }
    if (key == L"ddc-trim")
        return RunJob(L"Кэш Unreal", [](const tools::Log& log, std::wstring& res) {
            ULONGLONG freed = tools::TrimDdc(kDdcDays, log);
            res = L"Освобождено " + tools::Bytes(freed) + L". Нужное недавно осталось на месте.";
            g_scanAt = 0;
            return true;
        });
    if (key == L"bin") {
        ULONGLONG items = 0, bytes = tools::RecycleBinBytes(&items);
        Ask(L"Очистить корзину?",
            Plural((int)items, L"объект", L"объекта", L"объектов") + L", " + tools::Bytes(bytes) +
                L". Вернуть файлы будет нельзя.",
            L"Очистить", L"Не надо", [] {
                RunJob(L"Корзина", [](const tools::Log&, std::wstring& res) {
                    ULONGLONG b = tools::RecycleBinBytes();
                    bool ok = tools::EmptyRecycleBin();
                    res = ok ? L"Освобождено " + tools::Bytes(b) + L"." : L"Корзину очистить не получилось.";
                    return ok;
                });
            });
        return;
    }
    if (key.rfind(L"clean:", 0) == 0) {
        size_t i = idx(6, g_junk.size());
        if (i == (size_t)-1) return;
        tools::Junk j = g_junk[i];
        auto go = [j] { RunClean(L"Очистка: " + j.name, {j}); };
        if (!j.slow) return go();
        Ask(j.name + L": очистить?",
            tools::Bytes(j.bytes) + L". Шейдеры соберутся заново: первые запуски игр и редактора будут медленнее.",
            L"Очистить", L"Не надо", go);
        return;
    }
    if (key.rfind(L"proj:", 0) == 0 || key == L"proj-idle") {
        std::vector<tools::ProjectJunk> pick;
        if (key == L"proj-idle") {
            for (const tools::ProjectJunk& p : g_projects)
                if (p.idleDays >= kIdleDays) pick.push_back(p);
        } else {
            size_t i = idx(5, g_projects.size());
            if (i == (size_t)-1) return;
            pick.push_back(g_projects[i]);
        }
        if (pick.empty()) return;
        ULONGLONG total = 0;
        std::wstring names;
        for (const tools::ProjectJunk& p : pick) {
            total += p.bytes;
            if (names.size() < 120) names += (names.empty() ? L"" : L", ") + p.name + L"\\" + Leaf(p.path);
        }
        Ask(pick.size() == 1 ? L"Удалить " + Leaf(pick[0].path) + L" в " + pick[0].name + L"?"
                             : L"Удалить мусор " + std::to_wstring(pick.size()) + L" давних проектов?",
            tools::Bytes(total) + L": " + names +
                L". Исходники не трогаю; соберётся заново (npm install, сборка, Unreal пересоберёт).",
            L"Удалить", L"Не надо", [pick] {
                RunJob(L"Мусор проектов", [pick](const tools::Log& log, std::wstring& res) {
                    std::vector<std::wstring> paths;
                    for (const tools::ProjectJunk& p : pick) paths.push_back(p.path);
                    ULONGLONG freed = tools::CleanProjects(paths, log);
                    res = L"Освобождено " + tools::Bytes(freed) + L".";
                    g_scanAt = 0;
                    return freed > 0;
                });
            });
        return;
    }
    if (key.rfind(L"which:", 0) == 0) {
        size_t i = idx(6, g_which.size());
        if (i == (size_t)-1) return;
        std::wstring p = g_which[i].path;
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    if (key.rfind(L"conflict:", 0) == 0) {
        size_t i = idx(9, g_conflicts.size());
        if (i == (size_t)-1) return;
        tools::Conflict c = g_conflicts[i];
        if (!c.fixable) {
            g_card.Show(BoostCard::DONE, c.cmd + L": " + c.winnerVer + L" впереди",
                        L"Папка с " + c.winnerVer +
                            L" в системном PATH, а он всегда раньше своего. Поменять порядок "
                            L"можно в «Переменных среды» Windows с правами администратора.",
                        Theme(), 12000, Toast::G_INFO);
            return;
        }
        std::wstring dir = c.loser.substr(0, c.loser.find_last_of(L'\\'));
        Ask(L"Сделать " + c.cmd + L" " + c.loserVer + L" первым?",
            dir + L" встанет в начало PATH пользователя (старый PATH - в бэкап). Новые терминалы увидят сразу.",
            L"Поднять", L"Не надо", [dir] {
                RunJob(L"PATH", [dir](const tools::Log& log, std::wstring&) {
                    bool ok = tools::MoveUserPathFirst(dir, log);
                    g_scanAt = 0;
                    return ok;
                });
            });
        return;
    }
    if (key == L"term") {
        std::wstring dir = tools::ExplorerFolder();
        if (dir.empty()) {
            std::wstring p = tools::ClipboardPath();
            DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
            if (a != INVALID_FILE_ATTRIBUTES)
                dir = (a & FILE_ATTRIBUTE_DIRECTORY) ? p : p.substr(0, p.find_last_of(L'\\'));
        }
        if (dir.empty()) {
            g_card.Show(BoostCard::DONE, L"Где открыть терминал?",
                        L"Открой папку проекта в Проводнике или скопируй её (Ctrl+C) и выбери снова.", Theme(), 0,
                        Toast::G_WARN);
            return;
        }
        tools::TermEnv env = tools::DetectEnv(dir);
        if (!tools::OpenEnvTerminal(env)) {
            g_card.Show(BoostCard::DONE, L"Терминал не открылся", dir, Theme());
            return;
        }
        std::wstring what;
        for (const std::wstring& w : env.what) what += (what.empty() ? L"" : L", ") + w;
        g_card.Show(
            BoostCard::DONE, L"Терминал: " + Leaf(env.dir),
            what.empty() ? L"Обычное окружение: проектных инструментов не нашлось." : L"Подключено: " + what + L".",
            Theme(), 4500, Toast::G_OK);
        return;
    }
    if (key == L"path-clean")
        return RunJob(L"Чищу PATH", [](const tools::Log& log, std::wstring& res) {
            int n = tools::CleanUserPath(log);
            res = n ? L"Убрано записей: " + std::to_wstring(n) + L". Старый PATH - в бэкапах." : L"Чистить нечего.";
            return true;
        });
    if (key == L"syspath-clean")
        return RunJob(L"Системный PATH",
                      [](const tools::Log& log, std::wstring&) { return net::RunAdmin(net::JOB_SYSPATH_CLEAN, log); });
    if (key == L"path-add") {
        std::wstring p = tools::ClipboardPath();
        if (p.empty() || !(GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
            g_card.Show(BoostCard::DONE, L"В буфере нет папки", L"Скопируй папку в Проводнике (Ctrl+C) и выбери снова.",
                        Theme());
            return;
        }
        return RunJob(L"PATH", [p](const tools::Log& log, std::wstring&) { return tools::AddUserPath(p, log); });
    }
    if (key.rfind(L"port:", 0) == 0) {
        size_t i = idx(5, g_ports.size());
        if (i == (size_t)-1) return;
        tools::Listener l = g_ports[i];
        Ask(
            L"Порт " + std::to_wstring(l.port) + L": " + (l.project.empty() ? NoExe(l.process) : Leaf(l.project)),
            l.process + L", PID " + std::to_wstring(l.pid) + (l.command.empty() ? L"" : L": " + l.command) +
                L". Открыть http://localhost:" + std::to_wstring(l.port) + L" или завершить?",
            L"Открыть", L"Завершить",
            [l] {
                std::wstring url = L"http://localhost:" + std::to_wstring(l.port);
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            },
            [l] {
                RunJob(L"Порт " + std::to_wstring(l.port), [l](const tools::Log&, std::wstring& res) {
                    bool ok = tools::KillPid(l.pid, l.process);
                    res = ok ? NoExe(l.process) + L" завершён, порт свободен."
                             : L"Процесс не даёт себя завершить (возможно, запущен с правами).";
                    return ok;
                });
            });
        return;
    }
    if (key.rfind(L"app:", 0) == 0) {
        size_t i = idx(4, g_apps.size());
        if (i == (size_t)-1) return;
        tools::HeavyApp a = g_apps[i];
        Ask(L"Закрыть " + NoExe(a.name) + L"?",
            Plural((int)a.pids.size(), L"процесс", L"процесса", L"процессов") + L", " + tools::Bytes(a.bytes) +
                L". Сначала попрошу закрыться (окна сохранят своё), через 5 с - силой.",
            L"Закрыть", L"Не надо", [a] { CloseHeavy(a); });
        return;
    }
    if (key.rfind(L"start-off:", 0) == 0) {
        size_t i = idx(10, g_starts.size());
        if (i == (size_t)-1) return;
        tools::StartItem s = g_starts[i];
        if (!s.canToggle) {
            g_card.Show(BoostCard::DONE, s.name + L": для всего компьютера",
                        L"Такую запись выключают в Диспетчере задач, вкладка «Автозагрузка» (нужны права).", Theme(),
                        9000, Toast::G_INFO);
            return;
        }
        Ask(L"Не запускать " + s.name + L" при входе?",
            s.where + (s.memory ? L", держит " + tools::Bytes(s.memory) : L"") +
                L". Запись не удаляется - включить обратно можно в студии или Диспетчере задач.",
            L"Выключить", L"Не надо", [s] {
                std::wstring msg;
                bool ok = tools::SetStartup(s, false, [&](const std::wstring& t) { msg = t; });
                g_card.Show(BoostCard::DONE, ok ? L"Автозагрузка" : L"Не получилось", msg, Theme());
            });
        return;
    }
    if (key == L"tails") {
        if (g_tails.empty()) return;
        std::vector<tools::Leftover> tails = g_tails;
        ULONGLONG mem = 0;
        for (const tools::Leftover& t : tails) mem += t.mem;
        Ask(L"Завершить хвосты сборки?",
            Plural((int)tails.size(), L"процесс", L"процесса", L"процессов") + L", " + tools::Bytes(mem) + L": " +
                tails[0].name + L" - " + tails[0].why + (tails.size() > 1 ? L" и другие." : L"."),
            L"Завершить", L"Не надо", [tails] { KillLeftovers(tails); });
        return;
    }
    if (key.rfind(L"leak:", 0) == 0 || key.rfind(L"hog:", 0) == 0) {
        bool leak = key[0] == L'l';
        size_t i = leak ? idx(5, g_leakList.size()) : idx(4, g_hogs.size());
        if (i == (size_t)-1) return;
        std::wstring name = leak ? g_leakList[i].name : g_hogs[i].name;
        tools::HeavyApp a = AppByName(name);
        if (a.pids.empty() || tools::ProtectedExe(name)) {
            g_card.Show(BoostCard::DONE, NoExe(name),
                        tools::ProtectedExe(name) ? L"Это системное - не закрываю." : L"Уже закрыта.", Theme());
            return;
        }
        Ask(L"Закрыть " + NoExe(name) + L"?",
            Plural((int)a.pids.size(), L"процесс", L"процесса", L"процессов") + L", " + tools::Bytes(a.bytes) +
                L". Сначала попрошу закрыться (окна сохранят своё), через 5 с - силой.",
            L"Закрыть", L"Не надо", [a] { CloseHeavy(a); });
        return;
    }
    if (key == L"pagefile") return OpenPagefile();
    if (key == L"ports-old") {
        std::vector<tools::Listener> old = g_oldPorts;
        if (old.empty()) return;
        std::wstring list;
        for (const tools::Listener& l : old)
            if (list.size() < 100)
                list += (list.empty() ? L"" : L", ") + std::to_wstring(l.port) + L" " + NoExe(l.process);
        Ask(L"Завершить старые серверы?", list + L". Работают дольше 6 часов.", L"Завершить", L"Не надо", [old] {
            RunJob(L"Старые серверы", [old](const tools::Log&, std::wstring& res) {
                int ok = 0;
                for (const tools::Listener& l : old) ok += tools::KillPid(l.pid, l.process);
                res = L"Завершено " + std::to_wstring(ok) + L" из " + std::to_wstring(old.size()) + L".";
                return ok > 0;
            });
        });
        return;
    }
    if (key == L"net-top" || key == L"power") {
        bool top = key == L"net-top";
        if (!net::AdminReady()) {
            Ask(top ? L"Кто качает: нужны права" : L"Что мешает сну: нужны права",
                L"Это Windows отдаёт только с правами администратора. Один раз создам задачу (Windows спросит), "
                L"дальше - без вопросов; задача умеет только свой короткий список действий.",
                L"Создать", L"Не надо", [] {
                    RunJob(L"Задача с правами", [](const tools::Log&, std::wstring& res) {
                        bool ok = net::SetupAdmin();
                        res = ok ? L"Готово. Выбери пункт ещё раз." : L"Не создалась (отказ в окне Windows?).";
                        return ok;
                    });
                });
            return;
        }
        RunJob(top ? L"Кто качает" : L"Мешает сну", [top](const tools::Log& log, std::wstring& res) {
            std::vector<std::wstring> lines;
            bool ok = net::RunAdmin(top ? net::JOB_NET_TOP : net::JOB_POWER, [&](const std::wstring& s) {
                if (!s.empty() && s[0] == L'@')
                    lines.push_back(s);
                else
                    log(s);
            });
            if (top) {
                for (size_t i = 0; i < lines.size() && i < 4; ++i) {
                    std::vector<std::wstring> f = Split(lines[i], L'\t');
                    if (f.size() < 5) continue;
                    res += (res.empty() ? L"" : L"; ") + NoExe(f[2]) + L" приём " +
                           tools::Bytes(_wcstoui64(f[3].c_str(), nullptr, 10)) + L"/с, отдача " +
                           tools::Bytes(_wcstoui64(f[4].c_str(), nullptr, 10)) + L"/с";
                }
                if (res.empty()) res = L"Сейчас почти ничего не качается (по TCP; QUIC браузеров не видно).";
            } else {
                for (const std::wstring& l : lines) res += (res.empty() ? L"" : L"; ") + l.substr(l.find(L'\t') + 1);
                if (res.empty()) res = L"Ничего не держит компьютер без сна и не будит по таймеру.";
            }
            return ok;
        });
        return;
    }
    if (key == L"wake") {
        ShellExecuteW(nullptr, L"open", L"devmgmt.msc", nullptr, nullptr, SW_SHOWNORMAL);
        g_card.Show(BoostCard::DONE, L"Кто будит",
                    L"Свойства устройства - «Управление электропитанием» - снять «Разрешить выводить компьютер из "
                    L"ждущего режима». Мышь и клавиатуру обычно оставляют.",
                    Theme(), 20000, Toast::G_INFO);
        return;
    }
    if (key.rfind(L"ver:", 0) == 0) {
        size_t i = idx(4, g_vers.size());
        if (i == (size_t)-1) return;
        const tools::ToolVersion& v = g_vers[i];
        if (v.name == L"MSYS2") {
            CopyText(L"pacman -Syu");
            ShellExecuteW(nullptr, L"open", L"D:\\msys64\\ucrt64.exe", nullptr, L"D:\\msys64", SW_SHOWNORMAL);
            g_card.Show(BoostCard::DONE, L"MSYS2: " + v.latest + L" обновлений",
                        L"Команда pacman -Syu в буфере - вставь в окно MSYS2. Если попросит перезапуск, закрой окно "
                        L"и выполни её ещё раз.",
                        Theme(), 20000, Toast::G_INFO);
            return;
        }
        const wchar_t* url = v.name == L"Python"  ? L"https://www.python.org/downloads/windows/"
                             : v.name == L"Node"  ? L"https://nodejs.org/en/download"
                             : v.name == L"CMake" ? L"https://cmake.org/download/"
                                                  : L"https://git-scm.com/downloads/win";
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (key == L"ver-check")
        return RunJob(L"Версии", [](const tools::Log& log, std::wstring& res) {
            log(L"Спрашиваю python.org, nodejs, GitHub и MSYS2 (до минуты)...");
            std::vector<tools::ToolVersion> v = tools::ToolVersions();
            AcquireSRWLockExclusive(&g_scanLock);
            g_scan.versions = v;
            ReleaseSRWLockExclusive(&g_scanLock);
            if (!v.empty()) SetDay(L"versions", Today());
            for (const tools::ToolVersion& t : v)
                if (t.newer) res += (res.empty() ? L"" : L"; ") + t.name + L": " + t.note;
            if (res.empty()) res = L"Всё свежее.";
            return !v.empty();
        });
    if (key == L"dup-scan")
        return RunJob(L"Дубли", [](const tools::Log& log, std::wstring& res) {
            log(L"Ищу одинаковые файлы от 20 МБ...");
            std::vector<tools::DupGroup> d = tools::FindDuplicates(tools::DupRoots(), 20ull * 1024 * 1024);
            ULONGLONG extra = 0;
            for (const tools::DupGroup& g : d) extra += g.size * (g.paths.size() - 1);
            AcquireSRWLockExclusive(&g_scanLock);
            g_dups = d;
            g_dupsAt = GetTickCount64();
            ReleaseSRWLockExclusive(&g_scanLock);
            res = d.empty() ? L"Дублей от 20 МБ нет."
                            : L"Найдено " + Plural((int)d.size(), L"группа", L"группы", L"групп") + L", лишнего " +
                                  tools::Bytes(extra) + L". Они - в «Очистке».";
            return true;
        });
    if (key.rfind(L"dup:", 0) == 0) {
        size_t i = idx(4, g_dupList.size());
        if (i == (size_t)-1) return;
        tools::DupGroup g = g_dupList[i];
        std::vector<std::wstring> copies(g.paths.begin() + 1, g.paths.end());
        std::wstring list;
        for (const std::wstring& c : copies)
            if (list.size() < 140) list += (list.empty() ? L"" : L", ") + c;
        Ask(
            L"Убрать копии " + Leaf(g.paths[0]) + L"?",
            L"Останется " + g.paths[0] + L". В Корзину: " + list + L". Перед этим сверю SHA-256.", L"В Корзину",
            L"Показать",
            [g, copies] {
                RunJob(L"Дубли", [g, copies](const tools::Log& log, std::wstring& res) {
                    int n = tools::RecycleDuplicates(g.paths[0], copies, log);
                    res = n ? L"В Корзине: " + std::to_wstring(n) + L", освободится " +
                                  tools::Bytes(g.size * (ULONGLONG)n) + L" после её очистки."
                            : L"Ничего не убрано: файлы изменились или уже не копии.";
                    AcquireSRWLockExclusive(&g_scanLock);
                    g_dups.erase(std::remove_if(g_dups.begin(), g_dups.end(),
                                                [&](const tools::DupGroup& x) { return x.hash == g.hash; }),
                                 g_dups.end());
                    ReleaseSRWLockExclusive(&g_scanLock);
                    return n > 0;
                });
            },
            [g] {
                ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + g.paths[0] + L"\"").c_str(), nullptr,
                              SW_SHOWNORMAL);
            });
        return;
    }
    if (key == L"sec-restore") {
        if (net::AdminReady())
            return RunJob(L"Точка восстановления", [](const tools::Log& log, std::wstring&) {
                log(L"Создаю (до пары минут)...");
                return net::RunAdmin(net::JOB_RESTORE_POINT, log, 300000);
            });
        Ask(L"Создать точку восстановления?",
            L"Снимок системных файлов и реестра: если обновление драйвера или правка реестра что-то сломает, можно "
            L"будет откатиться. Нужны права - Windows спросит.",
            L"Создать", L"Не надо", [] {
                const wchar_t* ps =
                    L"-NoProfile -ExecutionPolicy Bypass -Command \"Write-Host 'Создаю точку восстановления...'; "
                    L"try { Checkpoint-Computer -Description 'MoonRadial' -RestorePointType MODIFY_SETTINGS "
                    L"-ErrorAction Stop; Write-Host 'Готово (если точка уже была меньше суток назад, Windows "
                    L"новую не создаёт).' } catch { Write-Host ('Не получилось: ' + $_.Exception.Message) }; "
                    L"Read-Host 'Enter - закрыть'\"";
                if ((INT_PTR)ShellExecuteW(nullptr, L"runas", L"powershell.exe", ps, nullptr, SW_SHOWNORMAL) <= 32)
                    g_card.Show(BoostCard::DONE, L"Не запустилось", L"Без прав администратора точку не создать.",
                                Theme(), 0, Toast::G_WARN);
            });
        return;
    }
    if (key == L"sec-rescan") {
        if (g_secRunning) {
            g_card.Show(BoostCard::DONE, L"Защита: уже проверяю", L"Итог придёт карточкой.", Theme(), 0, Toast::G_INFO);
            return;
        }
        g_secWarned.clear();  // перепроверка по просьбе - сказать заново
        g_secAsked = true;
        RefreshSecurity();
        scene::GuardBegin((int)tools::Processes().size());
        SceneCard::Get().Show(SceneCard::GUARD, L"Защита: проверка", Theme());
        return;
    }
    if (key == L"sec-av") {
        std::wstring av = tools::AntivirusUi();
        ShellExecuteW(nullptr, L"open", av.empty() ? L"windowsdefender://threat/" : av.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return;
    }
    if (key == L"sec-repair") {
        auto run = [](const wchar_t* title, const wchar_t* cmd) {
            std::wstring args = std::wstring(L"/k title ") + title + L" & " + cmd;
            HINSTANCE r = ShellExecuteW(nullptr, L"runas", L"cmd.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32)
                g_card.Show(BoostCard::DONE, L"Не запустилось", L"Без прав администратора проверка не идёт.", Theme(),
                            0, Toast::G_WARN);
        };
        Ask(
            L"Починить системные файлы?",
            L"SFC сверит файлы Windows и заменит испорченные (минут 10-15). Если SFC не справится - DISM "
            L"восстановит хранилище из Windows Update (минут 20, нужен интернет), потом снова SFC.",
            L"SFC", L"DISM", [run] { run(L"SFC", L"sfc /scannow"); },
            [run] { run(L"DISM", L"DISM /Online /Cleanup-Image /RestoreHealth"); }, 30000, Toast::G_INFO);
        return;
    }
    if (key.rfind(L"sec-app:", 0) == 0) {
        size_t i = idx(8, g_secApps.size());
        if (i == (size_t)-1) return;
        std::wstring exe = g_secApps[i].exe, dir = exe.substr(0, exe.find_last_of(L'\\'));
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL) <= 32)
            g_card.Show(BoostCard::DONE, L"Не запустилось", exe, Theme(), 0, Toast::G_WARN);
        return;
    }
    if (key.rfind(L"sec-find:", 0) == 0) {
        size_t i = idx(9, g_finds.size());
        if (i == (size_t)-1) return;
        SecurityAction(g_finds[i]);
        return;
    }
    if (key == L"lock-clip") {
        std::wstring p = tools::ClipboardPath();
        if (p.empty()) {
            g_card.Show(BoostCard::DONE, L"В буфере нет файла",
                        L"Скопируй файл или папку в Проводнике (Ctrl+C) и выбери снова.", Theme());
            return;
        }
        std::vector<tools::Locker> who = tools::WhoLocks(p);
        std::wstring name = Leaf(p);
        if (who.empty()) {
            g_card.Show(BoostCard::DONE, L"Никто не держит", L"«" + name + L"» свободен - удаляется и переносится.",
                        Theme());
            return;
        }
        std::wstring list;
        for (size_t i = 0; i < who.size() && i < 4; ++i) list += (i ? L", " : L"") + who[i].name;
        if (who.size() > 4) list += L" и ещё " + std::to_wstring(who.size() - 4);
        Ask(L"«" + Short(name, 28) + L"» держат", list + L".", L"Завершить", L"Не надо", [who] {
            RunJob(L"Освобождаю файл", [who](const tools::Log&, std::wstring& res) {
                int n = 0;
                for (const tools::Locker& l : who) n += tools::KillPid(l.pid) ? 1 : 0;
                res = L"Завершено " + std::to_wstring(n) + L" из " + std::to_wstring(who.size()) +
                      (n == (int)who.size() ? L". Файл свободен." : L". Кого-то завершить не удалось (нужны права).");
                return n == (int)who.size();
            });
        });
        return;
    }
}

void CloseHeavy(const tools::HeavyApp& a) {
    RunJob(L"Закрываю " + NoExe(a.name), [a](const tools::Log& log, std::wstring& res) {
        tools::AppClose c = tools::CloseApp(a, 5000, log);
        if (c.closed + c.killed == 0) {
            res = L"Программа не даёт себя завершить (возможно, запущена с правами).";
            return false;
        }
        res = NoExe(a.name) + L": закрылись сами " + std::to_wstring(c.closed) + L", силой " +
              std::to_wstring(c.killed) + (c.failed ? L", не далось " + std::to_wstring(c.failed) : L"") +
              L". Свободно " + tools::Bytes(tools::Memory().avail) + L".";
        return c.failed == 0;
    });
}

bool MemoryOffer(const std::wstring& why, const std::vector<std::wstring>& keep, bool force) {
    tools::MemoryState m = tools::Memory();
    // памяти и так хватает: больше 4 ГБ свободно и занято меньше 80%
    if (!force && m.avail > 4ull * 1024 * 1024 * 1024 && m.loadPercent < 80) return false;
    std::vector<tools::HeavyApp> apps = tools::HeavyApps(10);
    std::vector<tools::HeavyApp> pick;
    for (const tools::HeavyApp& a : apps) {
        bool skip = false;
        for (const std::wstring& k : keep) skip = skip || _wcsicmp(a.name.c_str(), k.c_str()) == 0;
        if (!skip && a.bytes >= 300ull * 1024 * 1024) pick.push_back(a);
    }
    if (pick.empty()) {
        if (force)
            g_card.Show(BoostCard::DONE, L"Закрывать нечего",
                        L"Свободно " + tools::Bytes(m.avail) + L"; тяжёлых программ не видно.", Theme());
        return force;
    }
    std::wstring list;
    for (size_t i = 0; i < pick.size() && i < 3; ++i)
        list += (i ? L", " : L"") + NoExe(pick[i].name) + L" " + tools::Bytes(pick[i].bytes);
    tools::HeavyApp top = pick[0];
    Ask(
        L"Свободно " + tools::Bytes(m.avail) + (why.empty() ? L"" : L" " + why),
        L"Из " + tools::Bytes(m.total) + L". Тяжелее всего: " + list + L".", L"Закрыть " + Short(NoExe(top.name), 14),
        L"Не надо", [top] { CloseHeavy(top); }, nullptr, 25000, Toast::G_WARN);
    return true;
}

void OnJob(WPARAM wp, LPARAM lp) {
    if (wp == kWeeklyTag) {
        std::unique_ptr<WeeklyMsg> m((WeeklyMsg*)lp);
        OnWeekly(m.get());
        return;
    }
    if (wp == kWatchTag) return TryProactive();
    if (wp == kSecTag) {
        if (!g_secAsked) return OnSecurity();
        g_secAsked = false;
        AcquireSRWLockShared(&g_secLock);
        size_t n = g_sec.procs.size() + g_sec.autos.size() + g_sec.hijacks.size() + g_sec.tasks.size() +
                   g_sec.exts.size() + g_sec.nets.size();
        int procs = g_sec.processes, autos = g_sec.autoruns;
        ReleaseSRWLockShared(&g_secLock);
        std::wstring checked = L"Проверено " + Plural(procs, L"программа", L"программы", L"программ") + L" и " +
                               Plural(autos, L"запись", L"записи", L"записей") + L" автозапуска.";
        if (SceneCard::Get().Visible()) {  // итог - в сцене: чистые слетаются в щит, находки - строками под ним
            std::vector<scene::GuardFind> finds;
            AcquireSRWLockShared(&g_secLock);
            for (const auto* v : {&g_sec.procs, &g_sec.autos, &g_sec.hijacks, &g_sec.tasks, &g_sec.exts, &g_sec.nets})
                for (const tools::Suspect& s : *v) finds.push_back({s.name, s.why, s.score});
            ReleaseSRWLockShared(&g_secLock);
            std::stable_sort(finds.begin(), finds.end(),
                             [](const scene::GuardFind& a, const scene::GuardFind& b) { return a.score > b.score; });
            scene::GuardEnd(procs, finds);
            return;
        }
        if (!n)
            g_card.Show(BoostCard::DONE, L"Защита: ничего странного", checked, Theme(), 9000, Toast::G_OK);
        else
            g_card.Show(BoostCard::DONE, L"Защита: находок " + std::to_wstring(n),
                        checked + L" Что именно - кольцо «Утилиты», «Защита».", Theme(), 20000, Toast::G_WARN);
        return;
    }
    std::unique_ptr<JobMsg> m((JobMsg*)lp);
    if (m->quiet) {
        if (m->done) g_busy = false;
        return;
    }
    if (!m->done) {
        g_card.Show(BoostCard::WORKING, m->title + L"...", m->text, Theme());
        return;
    }
    g_busy = false;
    g_card.Show(BoostCard::DONE, m->ok ? m->title + L": готово" : m->title + L": не получилось", m->text, Theme(),
                9000);
}

void Tick() {
    g_card.Tick();
    SceneCard::Get().Poll();
    SceneCard::Get().Tick();
    ULONGLONG now = GetTickCount64();
    if (now - g_proactiveAt > 5000) TryProactive();  // отложенное, пока была другая карточка
    // фоновый подсчёт: первый раз через минуту после старта, дальше раз в полчаса
    if (now - g_started > 60000 && (!g_scanAt || now - g_scanAt > 30 * 60 * 1000)) RefreshScan();
    // «Защита»: первый раз через 3 минуты после старта (подписи считаются долго), дальше раз в полчаса
    if (now - g_started > 3 * 60 * 1000 && (!g_secAt || now - g_secAt > 30 * 60 * 1000)) RefreshSecurity();
    // недельные проверки: через 3 минуты после старта, потом раз в 6 часов смотрим, не пора ли
    if (now - g_weeklyCheck > (g_weeklyCheck == g_started ? 3 * 60 * 1000 : 6 * 3600 * 1000)) {
        g_weeklyCheck = now;
        Weekly();
    }
    if (!g_cfg->toolsDiskWarn || now - g_diskCheck < 10 * 60 * 1000) return;
    g_diskCheck = now;
    for (const tools::Volume& v : tools::Volumes()) {
        if (v.type != DRIVE_FIXED) continue;
        if (!tools::DiskWarn(v)) {
            g_warned.erase(v.root);  // место вернулось - в следующий раз снова скажем
            continue;
        }
        if (!g_warned.insert(v.root).second || g_busy) continue;
        std::wstring root = v.root;
        Ask(
            L"Мало места на " + v.root.substr(0, 2),
            L"Свободно " + tools::Bytes(v.free) + L" из " + tools::Bytes(v.total) +
                L". Кэши, мусор проектов и старое из кэша Unreal - в кольце «Утилиты».",
            L"Что занимает", L"Позже",
            [root] {
                if (g_openStudio) g_openStudio(1, root);
            },
            nullptr, 30000, Toast::G_WARN);
        break;
    }
}

void Shutdown() {
    if (g_watchStop) SetEvent(g_watchStop);
    if (g_watchThread) WaitForSingleObject(g_watchThread, 3000), CloseHandle(g_watchThread);
    g_card.Destroy();
}

State Snapshot() {
    State s;
    AcquireSRWLockShared(&g_scanLock);
    s.junk = g_scan.junk;
    s.projects = g_scan.projects;
    s.growth = g_scan.growth;
    s.ddcOld = g_scan.ddcOld;
    s.driver = g_scan.driver;
    s.at = g_scanAt;
    ReleaseSRWLockShared(&g_scanLock);
    s.scanning = g_scanRunning;
    return s;
}

void Refresh() { RefreshScan(); }

void PrimeForSnapshot(const Config* cfg) {
    if (!g_cfg) g_cfg = cfg;
    RefreshScan();
    for (int i = 0; i < 1200 && g_scanRunning; ++i) Sleep(100);
}

bool Busy() { return g_busy; }

void Demo(int what) {
    if (what == 0) {
        const ULONGLONG GB = 1024ull * 1024 * 1024;
        scene::CleanBegin({{L"Кэш npm", 7600 * GB / 1000},
                           {L"Кэш pip", 5 * GB},
                           {L"TEMP старше суток", 4600 * GB / 1000},
                           {L"Кэш uv", 900 * GB / 1000}});
        SceneCard::Get().Show(SceneCard::CLEAN, L"Очистка (проверка)", Theme());
        HANDLE th = CreateThread(
            nullptr, 0,
            [](void*) -> DWORD {
                scene::CleanFeed f = scene::Clean();
                for (size_t i = 0; i < f.items.size(); ++i) {
                    Sleep(200);
                    scene::CleanMark(i, 1);
                    Sleep(800);
                    scene::CleanMark(i, 2, f.items[i].bytes * 97 / 100);
                }
                scene::CleanEnd();
                return 0;
            },
            nullptr, 0, nullptr);
        if (th) CloseHandle(th);
        return;
    }
    scene::GuardBegin((int)tools::Processes().size());
    SceneCard::Get().Show(SceneCard::GUARD, L"Защита: проверка", Theme());
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            Sleep(2200);
            std::vector<scene::GuardFind> finds;
            if (p)
                finds = {{L"scvhost.exe", L"имя как у svchost, а лежит в TEMP; без подписи", 7},
                         {L"wscript.exe", L"запуск скрипта при входе из Run", 4}};
            scene::GuardEnd(scene::Guard().expect, finds);
            return 0;
        },
        what == 1 ? (void*)1 : nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

void CleanQuick() {
    AcquireSRWLockShared(&g_scanLock);
    g_junk = g_scan.junk.empty() ? tools::FindJunk(false) : g_scan.junk;
    ReleaseSRWLockShared(&g_scanLock);
    Run(L"clean-all");
}

void TrimDdc() { Run(L"ddc-trim"); }

void CleanProjectList(const std::vector<tools::ProjectJunk>& list) {
    if (list.empty()) return;
    RunJob(L"Мусор проектов", [list](const tools::Log& log, std::wstring& res) {
        std::vector<std::wstring> paths;
        for (const tools::ProjectJunk& p : list) paths.push_back(p.path);
        ULONGLONG freed = tools::CleanProjects(paths, log);
        res = L"Освобождено " + tools::Bytes(freed) + L".";
        g_scanAt = 0;
        return freed > 0;
    });
}

}  // namespace toolsui
