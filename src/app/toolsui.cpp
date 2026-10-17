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
std::vector<tools::VirtualDisk> g_vhd;      // «Docker и WSL» в «Очистке»
std::vector<tools::Leftover2> g_leftovers;  // хвосты удалённых программ (номера left:i)
std::vector<tools::GitRepo> g_git;          // кольцо Git (номера git:i)
std::vector<tools::DeviceProblem> g_devs;   // «Система»: устройства с ошибками
tools::UpdateInfo g_updates;                // «Система»: история Центра обновления
ULONGLONG g_rebootAt = 0;                   // когда последний раз смотрели, ждёт ли Windows перезагрузки

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
    // система и разработка: всё это секунды, поэтому тоже в фоне
    std::vector<tools::GitRepo> git;
    std::vector<tools::VirtualDisk> vhd;
    std::vector<tools::Leftover2> leftovers;
    std::vector<tools::DeviceProblem> devices;
    tools::UpdateInfo updates;
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

// значок набора 4, если он уже нарисован, иначе нынешний (art/ICONS-PROMPTS.md, лист sheet-tools3.png)
const wchar_t* Have(const wchar_t* want, const wchar_t* now) {
    return Exists(ExeDir() + L"\\icons\\" + want) ? want : now;
}

Item Act(const std::wstring& name, const wchar_t* icon, const std::wstring& key, const std::wstring& note) {
    Item it;
    it.name = name;
    it.iconFull = ExeDir() + L"\\icons\\" + icon;
    if (!Exists(it.iconFull)) it.iconFull = ExeDir() + L"\\icons\\tools-box.png";  // новый значок ещё не нарисован
    it.special = 3;
    it.path = key;
    it.note = note;
    return it;
}

void RunJob(const std::wstring& title, std::function<bool(const tools::Log&, std::wstring&)> work, bool quiet = false) {
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

// Полный путь в System32 (пустое имя - сама папка): с правами запускаем только так.
std::wstring Sys32(const wchar_t* name) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    return *name ? std::wstring(sys) + L"\\" + name : std::wstring(sys);
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
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // история обновлений - через COM
            s.git = tools::GitRepos(*r);
            s.vhd = tools::VirtualDisks();
            s.leftovers = tools::AppLeftovers();
            s.devices = tools::DeviceProblems();
            s.updates = tools::WindowsUpdates();
            CoUninitialize();
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

// «Это моё»: находки, которые больше не показывать (ignored.tsv в папке данных, по строке на находку).
// Процесс запоминается по пути - тот же exe под другим номером тоже свой.
std::set<std::wstring> g_ignored;
bool g_ignoredLoaded = false;

std::wstring IgnoredFile() { return tools::DataDir() + L"\\ignored.tsv"; }

std::string ReadSmall(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string s(256 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return s;
}

void WriteSmall(const std::wstring& path, const std::string& s) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &put, nullptr);
    CloseHandle(f);
}

const std::set<std::wstring>& Ignored() {
    if (!g_ignoredLoaded) {
        g_ignoredLoaded = true;
        for (const std::wstring& l : Split(Widen(ReadSmall(IgnoredFile())), L'\n')) {
            std::wstring t = Trim(l);
            if (!t.empty()) g_ignored.insert(t);
        }
    }
    return g_ignored;
}

bool IsIgnored(const tools::Suspect& s) { return Ignored().count(FindKey(s)) != 0; }

void SaveIgnored() {
    std::wstring all;
    for (const std::wstring& k : g_ignored) all += k + L"\n";
    WriteSmall(IgnoredFile(), Narrow(all));
}

// все находки последней проверки, кроме отмеченных «Это моё»; вызывать под g_secLock
std::vector<tools::Suspect> AllFinds(const tools::SecurityScan& sec) {
    std::vector<tools::Suspect> all;
    for (const auto* v : {&sec.hijacks, &sec.procs, &sec.autos, &sec.tasks, &sec.nets, &sec.exts})
        for (const tools::Suspect& s : *v)
            if (!IsIgnored(s)) all.push_back(s);
    return all;
}

// После фоновой проверки: об опасном (счёт 6+) - карточка, про каждую находку один раз.
void OnSecurity() {
    if (g_busy) return;
    AcquireSRWLockShared(&g_secLock);
    std::vector<tools::Suspect> all = AllFinds(g_sec);
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

volatile bool g_game = false;
bool g_dev = false;
// видеокарта за игру: собирает поток слежки, итог - карточкой после игры
struct GpuStats {
    int max = 0, samples = 0, thermal = 0, power = 0;
} g_gpu;
bool g_ddcAsk = false;     // Unreal закрыт: спросить про старый DDC, если он большой
ULONGLONG g_devEnded = 0;  // когда закрылся Unreal: через 2 минуты - смотреть хвосты
std::set<std::wstring> g_leakWarned;
ULONGLONG g_commitWarned = 0, g_proactiveAt = 0;

std::vector<tools::Suspect> g_netAlerts;  // новое в hosts, DNS, прокси - ждёт карточки
std::set<std::wstring> g_netKnown;        // что уже видели (ключ находки)

std::wstring NetKey(const tools::Suspect& s) { return s.name + L"|" + s.path; }

// Мгновенная часть слежки: поменялись прокси, DNS адаптеров, hosts или Run - сразу сравнить с тем, что было.
void QuickPass() {
    std::vector<tools::Suspect> nets = tools::ScanNetwork();
    std::vector<tools::StartItem> fresh = tools::NewStartupItems();
    AcquireSRWLockExclusive(&g_watchLock);
    for (const tools::Suspect& s : nets)
        if (s.score >= tools::kSuspectScore && g_netKnown.insert(NetKey(s)).second) g_netAlerts.push_back(s);
    g_newStarts.insert(g_newStarts.end(), fresh.begin(), fresh.end());
    bool any = !g_netAlerts.empty() || !fresh.empty();
    ReleaseSRWLockExclusive(&g_watchLock);
    if (any) PostMessageW(g_app, WM_TOOLS_JOB, kWatchTag, 0);
}

DWORD WINAPI WatchLoop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // что уже было до запуска - не новость
    for (const tools::Suspect& s : tools::ScanNetwork()) g_netKnown.insert(NetKey(s));
    // уведомления об изменениях: ключи реестра и папка с hosts. Каждое срабатывание взводится заново.
    struct Key {
        HKEY root;
        const wchar_t* path;
        HKEY h = nullptr;
        HANDLE ev = nullptr;
    } keys[] = {
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"},
        {HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces"},
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"},
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"},
    };
    auto arm = [](Key& k) {
        if (k.h)
            RegNotifyChangeKeyValue(k.h, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, k.ev,
                                    TRUE | REG_NOTIFY_THREAD_AGNOSTIC);
    };
    std::vector<HANDLE> waits{g_watchStop};
    for (Key& k : keys) {
        if (RegOpenKeyExW(k.root, k.path, 0, KEY_NOTIFY, &k.h) != ERROR_SUCCESS) k.h = nullptr;
        k.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        arm(k);
        if (k.h && k.ev) waits.push_back(k.ev);
    }
    wchar_t etc[MAX_PATH] = {};
    GetSystemDirectoryW(etc, MAX_PATH);
    wcscat_s(etc, L"\\drivers\\etc");
    HANDLE hosts =
        FindFirstChangeNotificationW(etc, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (hosts != INVALID_HANDLE_VALUE) waits.push_back(hosts);

    ULONGLONG lastIn = 0, lastOut = 0, lastAt = 0, nextFull = 0, quickAt = 0, gpuAt = 0;
    int n = 0;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= nextFull) {
            std::vector<tools::ProcInfo> ps = tools::Processes();
            ULONGLONG in = 0, out = 0;
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
            nextFull = now + (g_game ? 300000 : 60000);  // в игре реже: и так ни о чём не спросим
            tools::RecordBattery();                      // износ по дням и сколько времени на зарядке
        }
        if (quickAt && now >= quickAt) {
            quickAt = 0;
            QuickPass();
        }
        // в игре - видеокарта раз в 10 секунд: максимум температуры и сколько упиралась в перегрев
        if (g_game && now >= gpuAt) {
            gpuAt = now + 10000;
            tools::GpuSample g = tools::GpuNow();
            if (g.ok) {
                AcquireSRWLockExclusive(&g_watchLock);
                g_gpu.max = std::max(g_gpu.max, g.temp);
                g_gpu.samples++;
                g_gpu.thermal += g.thermal;
                g_gpu.power += g.powerCap;
                ReleaseSRWLockExclusive(&g_watchLock);
            }
        }
        now = GetTickCount64();
        ULONGLONG due = quickAt ? std::min(quickAt, nextFull) : nextFull;
        if (g_game) due = std::min(due, gpuAt);
        DWORD wait = due > now ? (DWORD)std::min<ULONGLONG>(due - now, 60000) : 0;
        DWORD r = WaitForMultipleObjects((DWORD)waits.size(), waits.data(), FALSE, wait);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;  // выход
        if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + waits.size()) {
            HANDLE h = waits[r - WAIT_OBJECT_0];
            if (h == hosts)
                FindNextChangeNotification(hosts);
            else
                for (Key& k : keys)
                    if (k.ev == h) arm(k);
            // правки обычно идут пачкой (установщик пишет несколько значений): ждём, пока утихнет
            quickAt = GetTickCount64() + 1500;
        }
    }
    for (Key& k : keys) {
        if (k.h) RegCloseKey(k.h);
        if (k.ev) CloseHandle(k.ev);
    }
    if (hosts != INVALID_HANDLE_VALUE) FindCloseChangeNotification(hosts);
    CoUninitialize();
    return 0;
}

}  // namespace

Watched Watch() {
    Watched w;
    AcquireSRWLockShared(&g_watchLock);
    w.leaks = g_leaks;
    w.hogs = g_hist.CpuHogs(10, true);
    w.hogsReady = g_hist.Ready(10);
    w.commit = g_commit;
    ReleaseSRWLockShared(&g_watchLock);
    if (!w.commit.limit) w.commit = tools::Commit();
    return w;
}

std::vector<tools::ToolVersion> Versions() {
    AcquireSRWLockShared(&g_scanLock);
    std::vector<tools::ToolVersion> v = g_scan.versions;
    ReleaseSRWLockShared(&g_scanLock);
    return v;
}

void SetVersions(const std::vector<tools::ToolVersion>& v) {
    AcquireSRWLockExclusive(&g_scanLock);
    g_scan.versions = v;
    ReleaseSRWLockExclusive(&g_scanLock);
}

namespace {

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

void SecurityAction(const tools::Suspect& s);
int LastDay(const std::wstring& what);
void SetDay(const std::wstring& what, int day);

// Перезагрузка через 30 секунд (без прав): успеть сохранить, отменить - shutdown /a.
void RebootSoon() {
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", Sys32(L"shutdown.exe").c_str(), L"/r /t 30", Sys32(L"").c_str(),
                               SW_HIDE) <= 32) {
        g_card.Show(BoostCard::DONE, L"Не получилось", L"Перезагрузите через меню Пуск.", Theme(), 0, Toast::G_WARN);
        return;
    }
    g_card.Show(BoostCard::DONE, L"Перезагрузка через 30 секунд",
                L"Сохраните открытое. Передумали - Win+R, shutdown /a, Enter.", Theme(), 30000, Toast::G_INFO);
}

// Минута загрузки Windows (от 1601 года): по ней «уже говорили в эту загрузку».
int BootMinute() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int)(now / 600000000ull - GetTickCount64() / 60000);
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
    bool haveNet = !g_netAlerts.empty();
    tools::Suspect netFind = haveNet ? g_netAlerts.front() : tools::Suspect{};
    if (haveNet) g_netAlerts.erase(g_netAlerts.begin());
    ReleaseSRWLockExclusive(&g_watchLock);
    g_proactiveAt = now;
    if (haveNet) {  // hosts, DNS или прокси только что поменялись - сказать сразу, а не через полчаса
        SecurityAction(netFind);
        return;
    }
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
    if (g_ddcAsk && !g_dev) {
        g_ddcAsk = false;
        AcquireSRWLockShared(&g_scanLock);
        ULONGLONG old = g_scan.ddcOld;
        ReleaseSRWLockShared(&g_scanLock);
        if (old > 5ull * 1024 * 1024 * 1024) {
            Ask(
                L"Почистить старый кэш Unreal?",
                tools::Bytes(old) +
                    L" в DDC не открывалось больше месяца. Unreal пересоберёт нужное сам, когда понадобится.",
                L"Почистить", L"Оставить", [] { Run(L"ddc-trim"); }, nullptr, 30000, Toast::G_INFO);
            return;
        }
    }
    // Windows ждёт перезагрузки: смотреть раз в 10 минут, говорить один раз за загрузку
    if (now - g_started > 5 * 60 * 1000 && (!g_rebootAt || now - g_rebootAt > 10 * 60 * 1000)) {
        g_rebootAt = now;
        std::vector<std::wstring> why = tools::PendingReboot();
        int boot = BootMinute();
        if (!why.empty() && std::abs(LastDay(L"reboot-said") - boot) > 2) {
            SetDay(L"reboot-said", boot);
            std::wstring list;
            for (const std::wstring& w : why) list += (list.empty() ? L"" : L", ") + w;
            Ask(
                L"Windows ждёт перезагрузки",
                list +
                    L". «Завершение работы» не поможет: с быстрым запуском Windows не выключается до конца и "
                    L"установку не доделывает. Нужна именно перезагрузка.",
                L"Перезагрузить", L"Позже", [] { RebootSoon(); }, nullptr, 40000, Toast::G_INFO);
            return;
        }
    }
    if (g_devEnded && now - g_devEnded > 2 * 60 * 1000) {
        g_devEnded = 0;
        std::vector<tools::Leftover> tails;
        ULONGLONG mem = 0;
        for (const tools::Leftover& t : tools::BuildLeftovers(procs))
            if (t.why.find(L"Unreal") != std::wstring::npos) tails.push_back(t), mem += t.mem;
        g_ddcAsk = true;  // следующим заходом - старый кэш Unreal
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

void GameGpuSummary();

void OnModes(bool game, bool dev) {
    if (g_dev && !dev) g_devEnded = GetTickCount64();  // Unreal закрыт: через пару минут - не остались ли воркеры
    if (dev) g_devEnded = 0;
    if (game && !g_game) {  // игра началась: замеры видеокарты с нуля
        AcquireSRWLockExclusive(&g_watchLock);
        g_gpu = {};
        ReleaseSRWLockExclusive(&g_watchLock);  // первый замер - когда слежка проснётся (не дольше минуты)
    }
    if (!game && g_game) {
        g_game = false;
        GameGpuSummary();
    }
    g_game = game;
    g_dev = dev;
}

namespace {

std::wstring Level(int score) { return score >= kDangerScore ? L"Опасно: " : L"Странно: "; }

const wchar_t* FindWhat(tools::FindKind k) {
    static const wchar_t* kWhat[] = {
        L"процесс", L"автозапуск", L"реестр",       L"задача планировщика", L"расширение браузера", L"сеть",
        L"PATH",    L"служба",     L"подписка WMI", L"сам RadialMenu",      L"сертификат",          L"брандмауэр"};
    static_assert(sizeof(kWhat) / sizeof(kWhat[0]) == tools::kFindKinds, "вид находки без названия");
    return k >= 0 && k < tools::kFindKinds ? kWhat[k] : L"находка";
}

// значок находки по виду: у системных - свой (набор 3), у процессов, автозапуска, реестра и прочего - жук
const wchar_t* FindIcon(tools::FindKind k) {
    switch (k) {
        case tools::FIND_PATH:
            return L"tools-path-open.png";
        case tools::FIND_SVC:
            return L"tools-service.png";
        case tools::FIND_WMI:
            return L"tools-wmi.png";
        case tools::FIND_SELF:
            return L"tools-integrity.png";
        case tools::FIND_CERT:
            return L"tools-certs.png";
        case tools::FIND_FW:
            return L"tools-firewall.png";
        default:
            return L"tools-bug.png";
    }
}

// FILETIME (UTC) -> «12.10.2026 14:30» по местному времени; withTime=false - только дата
std::wstring FtText(ULONGLONG ft, bool withTime = true) {
    FILETIME u{(DWORD)ft, (DWORD)(ft >> 32)}, l;
    SYSTEMTIME t;
    if (!ft || !FileTimeToLocalFileTime(&u, &l) || !FileTimeToSystemTime(&l, &t)) return L"?";
    wchar_t buf[32];
    if (withTime)
        swprintf(buf, 32, L"%02d.%02d.%04d %02d:%02d", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute);
    else
        swprintf(buf, 32, L"%02d.%02d.%04d", t.wDay, t.wMonth, t.wYear);
    return buf;
}

ULONGLONG FtNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

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
// Незнакомый файл - в Песочницу Windows: сначала без сети (так безопаснее), с сетью - по второй кнопке.
// Песочница выключена - сказать, где включить.
void OpenInSandbox(const std::wstring& file) {
    if (!tools::SandboxAvailable()) {
        Ask(
            L"Песочница Windows выключена",
            L"Включается в «Компоненты Windows» - «Песочница Windows» (нужны права и перезагрузка; Windows Pro "
            L"у вас есть, виртуализация в BIOS тоже нужна).",
            L"Компоненты", L"Понятно", [] { tools::OpenSandboxFeature(); }, nullptr, 30000, Toast::G_INFO);
        return;
    }
    std::wstring name = file.substr(file.find_last_of(L'\\') + 1);
    std::wstring origin = tools::FileOrigin(file);      // откуда скачан: адрес из метки Windows
    std::wstring kit = ExpandPath(g_cfg->securityDir);  // Sysinternals для проверки поведения
    auto run = [file, kit](bool net, bool check) {
        std::wstring err;
        if (tools::RunInSandbox(file, net, err, check ? kit : L""))
            g_card.Show(BoostCard::DONE, check ? L"Проверка поведения открывается" : L"Песочница открывается",
                        check ? L"Секунд 20-30. Внутри откроется окно проверки: снимок системы, Process Monitor и "
                                L"Process Explorer, потом сама программа. По Enter - отчёт на рабочем столе песочницы."
                              : L"Секунд 10-20. Файл внутри - только для чтения; закроете окно - всё, что программа "
                                L"сделала, исчезнет.",
                        Theme(), 15000, Toast::G_OK);
        else
            g_card.Show(BoostCard::DONE, L"Песочница: не вышло", err, Theme(), 0, Toast::G_WARN);
    };
    // сеть - вторым вопросом: без неё безопаснее, с ней видно, куда программа подключается
    auto ask = [name, run](bool check) {
        Ask((check ? L"Проверить: " : L"Открыть: ") + name,
            L"Без сети программа ничего не скачает и никуда не отправит. С сетью - видно, куда она подключается, но "
            L"она "
            L"сможет связаться со своим сервером и с устройствами домашней сети.",
            L"Без сети", L"С сетью", [run, check] { run(false, check); }, [run, check] { run(true, check); }, 30000,
            Toast::G_INFO);
    };
    Ask(
        L"Песочница: " + name,
        (origin.empty() ? L"" : L"Скачан: " + Short(origin, 100) + L". ") +
            L"«Проверить поведение» - снимок системы до и после, запись всех действий программы и отчёт, где она "
            L"пыталась закрепиться. «Просто открыть» - одноразовая Windows с программой.",
        L"Проверить поведение", L"Просто открыть", [ask] { ask(true); }, [ask] { ask(false); }, 30000, Toast::G_INFO);
}

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
    std::wstring origin = file ? tools::FileOrigin(s.path) : L"";
    std::wstring facts = s.why + L". " + s.path + (s.signer.empty() ? L"" : L", подпись «" + s.signer + L"»") +
                         (s.parent.empty() ? L"" : L", запустил " + s.parent) +
                         (origin.empty() ? L"" : L", скачан: " + Short(origin, 90));
    if (s.kind == tools::FIND_CERT) {
        bool user = s.where.find(L"пользователя") != std::wstring::npos;
        Ask(
            Level(s.score) + Short(s.name, 60),
            s.why + L". Хранилище: " + s.where +
                L". Убрать: «Доверенные корневые центры сертификации» - «Сертификаты», правый щелчок - «Удалить». "
                L"Kaspersky, Docker, свои сертификаты разработки - оставить.",
            L"Открыть сертификаты", L"Понятно",
            [user] {
                ShellExecuteW(nullptr, L"open", user ? L"certmgr.msc" : L"certlm.msc", nullptr, nullptr, SW_SHOWNORMAL);
            },
            nullptr, 30000, Toast::G_WARN);
        return;
    }
    if (s.kind == tools::FIND_FW) {
        std::wstring path = s.path;
        Ask(
            Level(s.score) + Short(s.name, 60),
            s.why + L". " + s.path + (origin.empty() ? L"" : L", скачан: " + Short(origin, 90)) +
                L". Выключить: «Правила для входящих подключений», правило с этим именем - «Отключить правило».",
            L"Брандмауэр", file ? L"VirusTotal" : L"Понятно",
            [] { ShellExecuteW(nullptr, L"open", L"wf.msc", nullptr, nullptr, SW_SHOWNORMAL); },
            [path, file] {
                if (file) OpenVirusTotal(path);
            },
            30000, Toast::G_WARN);
        return;
    }
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
    if (s.kind == tools::FIND_PATH) {
        // папка PATH открыта на запись: права закрывает задача с правами (копия прав - в бэкапах помощника);
        // папки нет - её убирает «Почистить системный PATH»
        bool missing = GetFileAttributesW(s.path.c_str()) == INVALID_FILE_ATTRIBUTES;
        std::wstring dir = s.path;
        Ask(
            Level(s.score) + L"PATH открыт на запись", s.path + L": " + s.why + L". Нужны права - Windows спросит.",
            missing ? L"Убрать из PATH" : L"Закрыть запись", L"Показать",
            [missing] {
                RunJob(missing ? L"Системный PATH" : L"Права папок PATH",
                       [missing](const tools::Log& log, std::wstring&) {
                           bool ok = net::RunAdmin(missing ? net::JOB_SYSPATH_CLEAN : net::JOB_PATH_ACL, log);
                           g_secAt = 0;  // перепроверить
                           return ok;
                       });
            },
            [dir, missing] {
                if (!missing) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            },
            30000, Toast::G_WARN);
        return;
    }
    if (s.kind == tools::FIND_SVC) {
        std::wstring where = s.where, path = s.path;
        Ask(
            Level(s.score) + L"служба " + s.name,
            s.why + L". Файл: " + path +
                L". Чинить - права папки службы или "
                L"переустановка программы; ключ службы скопирую для regedit.",
            L"Открыть regedit", file ? L"VirusTotal" : L"Понятно",
            [where] {
                CopyText(where);
                ShellExecuteW(nullptr, L"open", L"regedit.exe", nullptr, nullptr, SW_SHOWNORMAL);
            },
            [path, file] {
                if (file) OpenVirusTotal(path);
            },
            30000, Toast::G_WARN);
        return;
    }
    if (s.kind == tools::FIND_WMI) {
        // имя подписки - в команду удаления: её выполняет пользователь сам, в PowerShell от администратора
        std::wstring name = s.name.rfind(L"WMI: ", 0) == 0 ? s.name.substr(5) : s.name;
        std::wstring cls = s.where.substr(s.where.find(L", ") + 2);
        std::wstring cmd = L"Get-CimInstance -Namespace root/subscription -ClassName " + cls +
                           L" | Where-Object Name -eq '" + name + L"' | Remove-CimInstance";
        Ask(
            Level(s.score) + s.name, s.why + L". Выполняет: " + Short(s.path, 140), L"Скопировать удаление", L"Понятно",
            [cmd] {
                CopyText(cmd);
                g_card.Show(BoostCard::DONE, L"Команда в буфере",
                            L"Вставьте её в PowerShell, запущенный от администратора. Если подписку ставили вы сами - "
                            L"не удаляйте.",
                            Theme(), 15000, Toast::G_INFO);
            },
            nullptr, 30000, Toast::G_WARN);
        return;
    }
    if (s.kind == tools::FIND_SELF) {
        Ask(
            Level(s.score) + s.name + L" изменён", s.why + L".", L"Kaspersky", L"Понятно",
            [] {
                std::wstring av = tools::AntivirusUi();
                ShellExecuteW(nullptr, L"open", av.empty() ? L"windowsdefender://threat/" : av.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
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

    g_finds = AllFinds(sec);
    g_secStarts = sec.starts;
    int danger = 0;
    for (const tools::Suspect& s : g_finds) danger += s.score >= kDangerScore;

    std::wstring checkedNote = L"Проверено " + Plural(sec.processes, L"программа", L"программы", L"программ") + L" и " +
                               Plural(sec.autoruns, L"запись", L"записи", L"записей") + L" автозапуска";
    Item item = Act(L"Защита",
                    !checked          ? L"tools-wait.png"
                    : danger          ? L"tools-bug.png"
                    : g_finds.empty() ? L"tools-ok.png"
                                      : L"tools-shield.png",
                    L"",
                    !checked          ? L"Подписи, места запуска, автозапуск и перехваты в реестре; проверяю..."
                    : g_finds.empty() ? checkedNote + L": ничего странного"
                    : danger          ? L"Опасного: " + std::to_wstring(danger) + L", всего находок " +
                                   std::to_wstring(g_finds.size()) + L". " + checkedNote
                             : L"Странного: " + std::to_wstring(g_finds.size()) + L". " + checkedNote);
    for (size_t i = 0; i < g_finds.size() && i < 12; ++i) {
        const tools::Suspect& s = g_finds[i];
        std::wstring what = FindWhat(s.kind);
        item.children.push_back(
            Act(Short(NoExe(s.name), 14), FindIcon(s.kind), L"sec-find:" + std::to_wstring(i),
                Level(s.score) + what + L". " + s.why + L". " + Short(s.path, 90) + L". 2 клика - что с этим сделать"));
    }
    item.children.push_back(Act(L"Заново", L"tools-scan.png", L"sec-rescan",
                                L"Ещё раз проверить запущенное, автозапуск и реестр; итог - карточкой"));
    item.children.push_back(Act(L"Списком", L"tools-shield.png", L"studio:6",
                                L"Все находки в студии: по видам, с причинами; «Это моё» - больше не показывать"));
    std::wstring av = tools::AntivirusUi();
    Item avItem = Act(av.empty() ? L"Defender" : L"Kaspersky", L"tools-shield.png", L"sec-av",
                      av.empty() ? L"Открыть «Защиту от вирусов» Windows - там полная и офлайн-проверка"
                                 : L"Открыть Kaspersky - там полная проверка компьютера");
    if (!av.empty()) {  // значок самого Kaspersky, как у программ из папки защиты
        avItem.iconFull.clear();
        avItem.full = av;
    }
    item.children.push_back(avItem);
    item.children.push_back(Act(L"Песочница", L"tools-sandbox.png", L"sec-sandbox",
                                tools::SandboxAvailable()
                                    ? L"Скопируйте exe в Проводнике (Ctrl+C) и 2 клика: он откроется в одноразовой "
                                      L"Windows - закрыли окно, и всё, что он сделал, исчезло"
                                    : L"Песочница Windows выключена: 2 клика - как включить"));
    item.children.push_back(Act(L"Камера и микрофон", L"tools-privacy.png", L"sec-privacy",
                                L"Кто и когда брал камеру, микрофон и геопозицию (Windows записывает сама)"));
    g_secApps = tools::SecurityApps(g_cfg->securityDir);
    for (size_t i = 0; i < g_secApps.size(); ++i) {
        Item a = Act(g_secApps[i].name, L"", L"sec-app:" + std::to_wstring(i), g_secApps[i].exe);
        a.iconFull.clear();  // значок самой программы
        a.full = g_secApps[i].exe;
        item.children.push_back(a);
    }
    return item;
}

// «Восстановление»: свой Recovery Kit. Всё с правами - через задачу помощника (одно окно UAC).
Item RecoveryItem() {
    std::vector<tools::Broken> broken = tools::FindBroken();  // реестр, мгновенно
    // «Восстановление» в боковой сектор кольца не влезает даже мелким шрифтом - на кольце «Починка»
    Item item = Act(L"Починка", L"rec-kit.png", L"",
                    broken.empty() ? L"Восстановление: починить Windows, точки восстановления, Центр обновления, "
                                     L"Проводник, особая загрузка, офлайн-проверка. Сломанного вирусами нет"
                                   : L"Испорчено вирусами или программами: " + std::to_wstring(broken.size()) +
                                         L". Ниже - вернуть и починить остальное");
    if (!broken.empty()) {
        std::wstring list;
        for (const tools::Broken& b : broken)
            if (list.size() < 140) list += (list.empty() ? L"" : L"; ") + b.what;
        item.children.push_back(Act(L"Вернуть " + std::to_wstring(broken.size()), L"rec-unbreak.png", L"rec-unbreak",
                                    list + L". 2 клика - вернуть как было в Windows (спрошу)"));
    }
    item.children.push_back(Act(L"Починить Windows", L"tools-repair.png", L"rec-repair",
                                L"DISM восстановит хранилище из Windows Update, потом SFC сверит системные файлы - "
                                L"одним заданием, ход в карточке (до часа, нужен интернет и права)"));
    item.children.push_back(Act(L"Точки", L"tools-snapshot.png", L"rec-points",
                                L"Точки восстановления: создать новую (перед драйвером, правками реестра или "
                                L"починкой) или откатиться к прежней"));
    item.children.push_back(Act(L"Центр обновления", L"rec-update.png", L"rec-wu",
                                L"Обновления зависли или не ставятся: остановить службы, убрать кэш загрузок и "
                                L"catroot2 (в копию .old), запустить заново. Спрошу"));
    item.children.push_back(Act(L"Проводник", L"rec-explorer.png", L"rec-explorer",
                                L"Значки «поехали» или эскизы старые: очистить их кэш и перезапустить Проводник"));
    item.children.push_back(Act(L"Особая загрузка", L"rec-boot.png", L"rec-boot",
                                L"Перезагрузка в меню восстановления: безопасный режим, откат, командная строка "
                                L"(спрошу)"));
    std::wstring av = tools::AntivirusUi();
    item.children.push_back(Act(L"Офлайн-проверка", L"rec-offline.png", L"rec-offline",
                                av.empty() ? L"Проверка Microsoft Defender до загрузки Windows - когда вирус мешает "
                                             L"обычной; компьютер перезагрузится (спрошу)"
                                           : L"У вас Kaspersky: проверка до загрузки - его средствами; 2 клика - "
                                             L"подсказка"));
    item.children.push_back(Act(L"Не грузится?", L"rec-kit.png", L"rec-memo",
                                L"Что делать, если Windows не загружается: куда нажать и где точки, флешка "
                                L"восстановления"));
    return item;
}

// «Система»: перезагрузка, обновления, устройства, видеокарта, батарея, отчёт.
Item SystemItem(const Scan& scan) {
    std::vector<std::wstring> reboot = tools::PendingReboot();
    g_updates = scan.updates;
    g_devs.clear();
    int off = 0;
    for (const tools::DeviceProblem& d : scan.devices)
        if (d.disabled)
            ++off;
        else
            g_devs.push_back(d);
    int issues = (int)!reboot.empty() + (int)!g_updates.failed.empty() + (int)g_devs.size();
    Item item = Act(L"Система", reboot.empty() ? L"tools-report.png" : L"tools-reboot.png", L"",
                    issues ? L"Стоит посмотреть: " + std::to_wstring(issues) +
                                 L". Перезагрузка, обновления, устройства, видеокарта, батарея, отчёт"
                           : L"Перезагрузка, обновления, устройства, видеокарта, батарея и отчёт о системе");
    if (!reboot.empty()) {
        std::wstring list;
        for (const std::wstring& w : reboot) list += (list.empty() ? L"" : L", ") + w;
        item.children.push_back(Act(L"Перезагрузка", L"tools-reboot.png", L"sys-reboot",
                                    L"Windows ждёт: " + list +
                                        L". «Завершение работы» не поможет (быстрый запуск). 2 клика - перезагрузить "
                                        L"через 30 с (спрошу)"));
    }
    if (g_updates.ok) {
        std::wstring failed;
        for (const std::wstring& f : g_updates.failed)
            if (failed.size() < 120) failed += (failed.empty() ? L"" : L"; ") + f;
        item.children.push_back(Act(
            g_updates.failed.empty() ? L"Обновления"
                                     : L"Обновления: ошибок " + std::to_wstring(g_updates.failed.size()),
            L"tools-update.png", L"sys-wu",
            (g_updates.lastOk
                 ? L"Последнее установлено " + FtText(g_updates.lastOk, false) + L": " + Short(g_updates.lastTitle, 80)
                 : std::wstring(L"Установленных за последнее время нет")) +
                (failed.empty() ? L"" : L". Не установились за 30 дней: " + failed) + L". 2 клика - Центр обновления"));
    }
    for (size_t i = 0; i < g_devs.size() && i < 4; ++i)
        item.children.push_back(Act(Short(g_devs[i].name, 14), L"tools-devices.png", L"sys-dev",
                                    g_devs[i].name + L": " + g_devs[i].text + L". 2 клика - Диспетчер устройств"));
    if (g_devs.empty() && g_scanAt)
        item.children.push_back(Act(L"Устройства", L"tools-devices.png", L"sys-dev",
                                    std::wstring(L"Ошибок в Диспетчере устройств нет") +
                                        (off ? L"; выключено вручную: " + std::to_wstring(off) : L"") +
                                        L". 2 клика - открыть"));
    item.children.push_back(Act(L"Видеокарта", L"tools-temp.png", L"sys-gpu",
                                L"Температура, нагрузка и питание сейчас (nvidia-smi); в игре слежка меряет сама и "
                                L"после игры скажет, если был перегрев"));
    if (tools::Battery().present)
        item.children.push_back(Act(L"Износ батареи", L"tools-battery.png", L"sys-battery",
                                    L"Ёмкость по паспорту, сейчас и месяц назад; совет про ограничение заряда"));
    item.children.push_back(Act(L"Отчёт", L"tools-report.png", L"sys-report",
                                L"Железо, диски, драйверы, находки «Защиты» - одним текстом в Документы, чтобы "
                                L"показать, если просите помощи"));
    return item;
}

// «Git»: проекты с незакоммиченным и неотправленным; 2 клика - терминал с окружением проекта.
Item GitItem(const Scan& scan) {
    g_git.clear();
    int changed = 0, ahead = 0;
    for (const tools::GitRepo& r : scan.git)
        if (r.changed || r.ahead) {
            g_git.push_back(r);
            changed += r.changed > 0;
            ahead += r.ahead > 0;
        }
    Item item = Act(L"Git", L"tools-git.png", L"",
                    !g_scanAt       ? L"Проекты с незакоммиченным и неотправленным; считается в фоне"
                    : g_git.empty() ? L"Все проекты из колец закоммичены и отправлены"
                                    : L"С правками: " + std::to_wstring(changed) + L", не отправлено в GitHub: " +
                                          std::to_wstring(ahead));
    for (size_t i = 0; i < g_git.size() && i < 12; ++i) {
        const tools::GitRepo& r = g_git[i];
        std::wstring label = Short(r.name, 12);
        if (r.ahead) label += L" +" + std::to_wstring(r.ahead);
        std::wstring note = r.path + L", ветка " + r.branch;
        if (r.changed) note += L". Изменено файлов: " + std::to_wstring(r.changed);
        if (r.ahead) note += L". Коммитов не в GitHub: " + std::to_wstring(r.ahead);
        if (r.noUpstream) note += L". Ветка не связана с удалённой (не отправлялась)";
        if (r.behind) note += L". Отстаёт на " + std::to_wstring(r.behind);
        item.children.push_back(
            Act(label, L"tools-git.png", L"git:" + std::to_wstring(i), note + L". 2 клика - терминал в папке"));
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
    for (const tools::Volume& v : tools::VolumesCached()) {
        std::wstring letter = v.root.substr(0, 2);
        int pct = v.total ? (int)(v.free * 100 / v.total) : 0;
        disks.children.push_back(
            Act(letter + L" " + tools::Bytes(v.free), L"tools-disk.png", L"scan:" + v.root,
                L"Свободно " + tools::Bytes(v.free) + L" из " + tools::Bytes(v.total) + L" (" + std::to_wstring(pct) +
                    L"%)" + (v.label.empty() ? L"" : L", «" + v.label + L"»") + L". 2 клика - что занимает место"));
        status += (status.empty() ? L"" : L"   |   ") + letter + L" свободно " + tools::Bytes(v.free);
    }
    disks.children.push_back(
        Act(L"Здоровье", L"tools-health.png", L"health", L"Износ, температура и часы работы SSD; раз в неделю - сам"));
    for (size_t i = 0, shown = 0; i < scan.growth.size() && shown < 3; ++i) {
        const tools::Growth& g = scan.growth[i];
        if (g.delta < 200ll * 1024 * 1024) break;
        disks.children.push_back(Act(L"+" + tools::Bytes((ULONGLONG)g.delta) + L" " + Short(g.name, 12),
                                     L"tools-update.png", L"",
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
        clean.children.push_back(Act(L"Всё сразу " + tools::Bytes(quickTotal),
                                     Have(L"tools-dustpan.png", L"tools-clean.png"), L"clean-all",
                                     L"Кэши и временные файлы (свежее суток не трогаю), кроме кэшей шейдеров; "
                                     L"занятое пропускается"));
    if (scan.ddcOld > 256ull * 1024 * 1024)
        clean.children.push_back(Act(L"Unreal: старое " + tools::Bytes(scan.ddcOld),
                                     Have(L"tools-archive.png", L"tools-clean.png"), L"ddc-trim",
                                     L"Из кэша Unreal - то, что не открывалось " + Days(kDdcDays) +
                                         L". Шейдеры текущих проектов остаются, пересборки не будет"));
    for (size_t i = 0; i < g_junk.size(); ++i)
        clean.children.push_back(
            Act(JunkShort(g_junk[i].name) + (sized ? L" " + tools::Bytes(g_junk[i].bytes) : L""),
                Have(L"tools-cache.png", L"tools-clean.png"), L"clean:" + std::to_wstring(i),
                g_junk[i].name + L": " + g_junk[i].path +
                    (g_junk[i].slow ? L". Соберётся заново: первые запуски игр и редактора будут медленнее (спрошу)"
                                    : L". Программа скачает или создаст заново")));
    if (bin)
        clean.children.push_back(
            Act(L"Корзина " + tools::Bytes(bin), L"recycle.png", L"bin",
                Plural((int)binItems, L"объект", L"объекта", L"объектов") + L". Очистить насовсем (спрошу ещё раз)"));
    // Docker и WSL: их диски только растут, место внутри освобождается, а файл - нет
    g_vhd = scan.vhd;
    ULONGLONG vhdTotal = 0;
    for (const tools::VirtualDisk& v : g_vhd) vhdTotal += v.bytes;
    if (vhdTotal > 2ull * 1024 * 1024 * 1024)
        clean.children.push_back(Act(L"Docker и WSL " + tools::Bytes(vhdTotal), L"tools-containers.png", L"vhd",
                                     L"Диски Docker и WSL: " + Short(g_vhd[0].name, 20) + L" " +
                                         tools::Bytes(g_vhd[0].bytes) +
                                         L" и др. 2 клика - почистить Docker или сжать диски"));
    // хвосты программ, которых больше нет
    g_leftovers = scan.leftovers;
    for (size_t i = 0; i < g_leftovers.size() && i < 6; ++i)
        clean.children.push_back(
            Act(L"Хвост " + Short(g_leftovers[i].name, 12) + L" " + tools::Bytes(g_leftovers[i].bytes),
                L"tools-leftovers.png", L"left:" + std::to_wstring(i),
                g_leftovers[i].path + L": такой программы больше нет, папку не открывали " +
                    Days(g_leftovers[i].idleDays) + L". 2 клика - в Корзину (спрошу)"));
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
    Item projects = Act(L"Проекты", L"tools-projects.png", L"",
                        !sized               ? L"Мусор сборки в проектах; ещё считается"
                        : g_projects.empty() ? L"Мусора сборки в проектах не нашлось"
                                             : L"Мусор сборки в проектах: " + tools::Bytes(projTotal) +
                                                   L"; соберётся заново командой сборки или установки");
    if (idleCount > 1)
        projects.children.push_back(Act(L"Давние " + tools::Bytes(idleTotal),
                                        Have(L"tools-shelved.png", L"tools-clean.png"), L"proj-idle",
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
    ring.push_back(projects);

    // PATH: проблемы, конфликты версий и что найдётся первым
    int dead = 0, dup = 0;
    for (const tools::PathEntry& e : tools::ReadPath()) {
        if (!e.system && e.dead) ++dead;
        if (!e.system && e.dup) ++dup;
    }
    // папки системного PATH, куда может писать кто угодно, - опаснее всего в разделе: они первыми
    std::vector<tools::PathAcl> open = tools::SystemPathAcl();
    Item path =
        Act(L"PATH", open.empty() ? L"tools-path.png" : L"tools-path-open.png", L"",
            (open.empty() ? L"" : L"Открыто на запись: " + std::to_wstring(open.size()) + L". ") +
                (dead + dup || !g_conflicts.empty() ? L"Лишних записей: " + std::to_wstring(dead + dup) +
                                                          L", конфликтов версий: " + std::to_wstring(g_conflicts.size())
                                                    : L"Чисто; ниже - какая команда найдётся первой"));
    for (const tools::PathAcl& a : open)
        path.children.push_back(Act(L"Закрыть " + Leaf(a.dir), L"tools-path-open.png", L"path-acl",
                                    a.dir + (a.exists ? L": писать сюда может любой пользователь, а находят отсюда "
                                                        L"файлы и программы с правами. 2 клика - закрыть (спрошу права)"
                                                      : L": папки нет, а создать её может любой. 2 клика - убрать из "
                                                        L"PATH (спрошу права)")));
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
        ports.children.push_back(Act(L"Старые " + std::to_wstring(g_oldPorts.size()),
                                     Have(L"tools-stale-server.png", L"tools-clean.png"), L"ports-old",
                                     L"Серверы разработки, работающие дольше 6 часов - скорее всего забытые с прошлого "
                                     L"раза. 2 клика - завершить (спрошу)"));
    if (ports.children.empty()) ports.note = L"Порты разработки свободны: ни один сервер проекта не слушает";
    ring.push_back(ports);

    // процессы: память и самые тяжёлые программы
    tools::MemoryState mem = tools::Memory();
    g_apps = tools::HeavyApps(8);
    Item procs = Act(L"Процессы", L"tools-procs.png", L"",
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
        procs.children.push_back(Act(L"Хвосты " + std::to_wstring(g_tails.size()),
                                     Have(L"tools-cable.png", L"tools-clean.png"), L"tails",
                                     L"Остались после сборки: " + names + L"; держат " + tools::Bytes(tm) +
                                         L". 2 клика - завершить все (спрошу)"));
    }
    for (size_t i = 0; i < g_leakList.size() && i < 3; ++i)
        procs.children.push_back(Act(
            L"Растёт " + Short(NoExe(g_leakList[i].name), 10), L"tools-bug.png", L"leak:" + std::to_wstring(i),
            L"Память: " + tools::Bytes(g_leakList[i].was) + L" -> " + tools::Bytes(g_leakList[i].now) + L" за " +
                std::to_wstring(g_leakList[i].minutes) + L" мин, ровно и без остановки. 2 клика - закрыть (спрошу)"));
    procs.children.push_back(Act(L"Подкачка", Have(L"tools-pagefile.png", L"tools-mem.png"), L"pagefile",
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
    Item bg = Act(L"Фон", L"tools-bg.png", L"", std::wstring(L"Что работает без окна и тратит батарею; ") + speed);
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
        bg.note += L". Процессор в фоне считается за 10 минут с запуска RadialMenu";
    else if (g_hogs.empty())
        bg.note += L". За 10 минут ни одна программа без окна не съела больше 1% процессора";
    bg.children.push_back(Act(L"Кто качает", L"net-procs.png", L"net-top",
                              std::wstring(L"Трафик по программам за 3 секунды (TCP; QUIC браузеров не видно). ") +
                                  speed + (net::AdminReady() ? L"" : L". Нужна задача с правами (спрошу)")));
    bg.children.push_back(Act(L"Мешает сну", Have(L"tools-alarm.png", L"tools-battery.png"), L"power",
                              std::wstring(L"Кто не даёт уснуть или выключить экран и кто будит по таймеру") +
                                  (net::AdminReady() ? L"" : L". Нужна задача с правами (спрошу)")));
    AcquireSRWLockShared(&g_watchLock);
    g_wake = g_wakeCache;
    ReleaseSRWLockShared(&g_watchLock);
    if (!g_wake.empty()) {
        std::wstring list;
        for (const std::wstring& w : g_wake) list += (list.empty() ? L"" : L", ") + w;
        bg.children.push_back(Act(L"Будят " + std::to_wstring(g_wake.size()),
                                  Have(L"tools-alarm.png", L"tools-battery.png"), L"wake",
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
    Item starts = Act(L"Автозагрузка", L"tools-start.png", L"",
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
    ring.push_back(GitItem(scan));

    // Первое кольцо - шесть групп (art/tools_rings_sketch.html, вариант A): во втором - утилиты группы,
    // щелчок по утилите - её пункты на месте второго кольца. Последний пункт группы - она же в студии.
    enum { DISKS, CLEAN, PROJECTS, PATH, PORTS, PROCS, BG, STARTS, TERM, LOCK, GIT };
    auto group = [&](const std::wstring& name, const wchar_t* icon, const std::wstring& note,
                     std::initializer_list<int> tools, int studioTab, const std::wstring& studioNote) {
        Item g = Act(name, icon, L"", note);
        for (int t : tools) {
            Item it = ring[t];
            if (!it.children.empty()) it.note += L". Щелчок - открыть";
            g.children.push_back(std::move(it));
        }
        g.children.push_back(Act(L"В студии", Have(L"tools-studio.png", L"tools-box.png"),
                                 L"studio:" + std::to_wstring(studioTab), studioNote));
        return g;
    };
    std::vector<Item> groups;
    groups.push_back(group(L"Место", Have(L"grp-space.png", L"tools-disk.png"),
                           L"Диски, очистка, мусор проектов. " + ring[CLEAN].note, {DISKS, CLEAN, PROJECTS}, 1,
                           L"Что занимает место: соты по папкам, кэши"));
    groups.push_back(group(L"Память", Have(L"grp-memory.png", L"tools-mem.png"),
                           L"Процессы, фон, автозагрузка. " + ring[PROCS].note, {PROCS, BG, STARTS}, 4,
                           L"Колба памяти (слои программ, утечки, подкачка) и батарея (ветки к фоновым программам) в "
                           L"студии"));
    groups.push_back(group(L"Разработка", Have(L"grp-dev.png", L"tools-git.png"),
                           L"Git, PATH, порты, терминал, кто держит файл. " + ring[GIT].note,
                           {GIT, PATH, PORTS, TERM, LOCK}, 0, L"PATH и переменные, порты, версии в студии"));
    groups.push_back(SecurityItem());
    groups.push_back(RecoveryItem());
    groups.push_back(SystemItem(scan));
    ring = std::move(groups);
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
    if (key == L"path-acl") {  // то же, что находка «Защиты»: закрыть запись или убрать мёртвую папку
        std::vector<tools::PathAcl> open = tools::SystemPathAcl();
        if (open.empty()) {
            g_card.Show(BoostCard::DONE, L"PATH", L"Открытых на запись папок в системном PATH уже нет.", Theme(), 0,
                        Toast::G_OK);
            return;
        }
        tools::Suspect s;
        s.kind = tools::FIND_PATH;
        s.score = 6;
        s.path = open[0].dir;
        s.why = L"писать сюда может любой пользователь";
        SecurityAction(s);
        return;
    }
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
    if (key == L"vhd") {
        if (g_vhd.empty()) return;
        std::vector<tools::VirtualDisk> disks = g_vhd;
        ULONGLONG total = 0;
        std::wstring list;
        for (const tools::VirtualDisk& v : disks) {
            total += v.bytes;
            if (list.size() < 120) list += (list.empty() ? L"" : L", ") + v.name + L" " + tools::Bytes(v.bytes);
        }
        Ask(
            L"Docker и WSL: " + tools::Bytes(total),
            list +
                L". «Почистить Docker» - остановленные контейнеры, ненужные сети и образы без имени (Docker "
                L"должен быть запущен). «Сжать диски» - закрою WSL и Docker и верну освобождённое внутри место "
                L"на диск (нужны права).",
            L"Почистить Docker", L"Сжать диски",
            [] {
                RunJob(L"Docker", [](const tools::Log& log, std::wstring& res) {
                    log(L"docker system prune: убираю неиспользуемое...");
                    std::wstring out;
                    int code = tools::RunCapture(L"docker system prune -f", out, 10 * 60 * 1000);
                    if (code == -1) {
                        res = L"Docker не найден в PATH.";
                        return false;
                    }
                    std::wstring freed;
                    for (const std::wstring& l : Split(out, L'\n'))
                        if (l.find(L"reclaimed") != std::wstring::npos) freed = l;
                    res = code != 0 ? L"Docker ответил ошибкой (Docker Desktop запущен?): " + Short(Trim(out), 140)
                          : freed.empty() ? L"Убирать было нечего."
                                          : freed + L". Чтобы место вернулось на диск - «Сжать диски».";
                    g_scanAt = 0;
                    return code == 0;
                });
            },
            [disks, total] {
                RunJob(L"Сжатие дисков", [disks, total](const tools::Log& log, std::wstring& res) {
                    log(L"Закрываю WSL и Docker (wsl --shutdown)...");
                    std::wstring out;
                    tools::RunCapture(L"\"" + Sys32(L"wsl.exe") + L"\" --shutdown", out, 60000);
                    log(L"Сжимаю диски (минуты, зависит от размера)...");
                    bool ok = net::RunAdmin(net::JOB_VHD_COMPACT, log, 60 * 60 * 1000);
                    ULONGLONG after = 0;
                    for (const tools::VirtualDisk& v : tools::VirtualDisks()) after += v.bytes;
                    res =
                        after < total ? L"Освобождено " + tools::Bytes(total - after) + L"." : L"Сжимать было нечего.";
                    g_scanAt = 0;
                    return ok;
                });
            },
            30000, Toast::G_INFO);
        return;
    }
    if (key.rfind(L"left:", 0) == 0) {
        size_t i = idx(5, g_leftovers.size());
        if (i == (size_t)-1) return;
        tools::Leftover2 l = g_leftovers[i];
        Ask(
            L"Убрать хвост " + l.name + L"?",
            l.path + L", " + tools::Bytes(l.bytes) + L". Программы с таким именем нет, папку не открывали " +
                Days(l.idleDays) + L". Уйдёт в Корзину - вернуть можно оттуда.",
            L"В Корзину", L"Показать",
            [l] {
                RunJob(L"Хвост " + l.name, [l](const tools::Log& log, std::wstring& res) {
                    int n = tools::RecycleLeftovers({l.path}, log);
                    res = n ? L"В Корзине, освободится " + tools::Bytes(l.bytes) + L" после её очистки."
                            : L"Не убрано: папка занята или уже не похожа на хвост.";
                    g_scanAt = 0;
                    return n > 0;
                });
            },
            [l] { ShellExecuteW(nullptr, L"open", l.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        return;
    }
    if (key.rfind(L"git:", 0) == 0) {
        size_t i = idx(4, g_git.size());
        if (i == (size_t)-1) return;
        tools::TermEnv env = tools::DetectEnv(g_git[i].path);
        if (!tools::OpenEnvTerminal(env)) g_card.Show(BoostCard::DONE, L"Терминал не открылся", env.dir, Theme());
        return;
    }
    // ---------------- восстановление ----------------
    if (key == L"rec-repair") {
        Ask(
            L"Починить Windows?",
            L"Сначала DISM сверит хранилище компонентов с Windows Update (до получаса, нужен интернет), потом SFC "
            L"проверит системные файлы. Компьютером можно пользоваться. Перед этим стоит сделать «Снимок».",
            L"Починить", L"Не надо",
            [] {
                RunJob(L"Починка Windows", [](const tools::Log& log, std::wstring&) {
                    return net::RunAdmin(net::JOB_REPAIR, log, 90 * 60 * 1000);
                });
            },
            nullptr, 30000, Toast::G_INFO);
        return;
    }
    if (key == L"rec-points") {
        Ask(
            L"Точки восстановления",
            L"«Создать» - снимок системных файлов и реестра сейчас (нужны права). «Откатить» - окно «Восстановление "
            L"системы»: точки с датами, откат к одной из них.",
            L"Создать", L"Откатить", [] { Run(L"sec-restore"); },
            [] {
                if ((INT_PTR)ShellExecuteW(nullptr, L"open", Sys32(L"rstrui.exe").c_str(), nullptr, Sys32(L"").c_str(),
                                           SW_SHOWNORMAL) <= 32)
                    g_card.Show(BoostCard::DONE, L"Не открылось", L"Восстановление системы выключено или нет прав.",
                                Theme(), 0, Toast::G_WARN);
            },
            30000, Toast::G_INFO);
        return;
    }
    if (key == L"rec-wu") {
        Ask(
            L"Сбросить Центр обновления?",
            L"Остановлю службы обновления, уберу кэш загрузок и каталог подписей (в копии .old - вернуть можно), "
            L"запущу заново. Windows скачает список обновлений с нуля. Нужны права.",
            L"Сбросить", L"Не надо",
            [] {
                RunJob(L"Центр обновления", [](const tools::Log& log, std::wstring&) {
                    return net::RunAdmin(net::JOB_WU_RESET, log, 10 * 60 * 1000);
                });
            },
            nullptr, 30000, Toast::G_INFO);
        return;
    }
    if (key == L"rec-unbreak") {
        std::vector<tools::Broken> broken = tools::FindBroken();
        if (broken.empty()) {
            g_card.Show(BoostCard::DONE, L"Всё на месте",
                        L"Диспетчер задач, regedit, командная строка, запуск .exe и .bat, Shell и Userinit - как в "
                        L"Windows. Чинить нечего.",
                        Theme(), 9000, Toast::G_OK);
            return;
        }
        std::wstring list;
        bool admin = false;
        for (const tools::Broken& b : broken) {
            admin |= b.admin;
            if (list.size() < 220) list += (list.empty() ? L"" : L"; ") + b.what;
        }
        Ask(
            L"Вернуть " + Plural((int)broken.size(), L"настройку", L"настройки", L"настроек") + L"?",
            list + L". Верну как в Windows; старые значения - в журнале." +
                (admin ? L" Часть - системная, нужны права." : L""),
            L"Вернуть", L"Не надо",
            [broken, admin] {
                RunJob(L"Вернуть сломанное", [broken, admin](const tools::Log& log, std::wstring& res) {
                    int fixed = tools::FixBrokenUser(broken, log);
                    bool ok = true;
                    if (admin) ok = net::RunAdmin(net::JOB_UNBREAK, log, 120000);
                    size_t left = tools::FindBroken().size();
                    res = left ? L"Вернул " + std::to_wstring(fixed) + L", осталось " + std::to_wstring(left) +
                                     L". Если ломается снова - что-то его ставит, стоит проверить «Защиту»."
                               : L"Всё вернул как в Windows.";
                    g_secAt = 0;
                    return ok && !left;
                });
            },
            nullptr, 30000, Toast::G_WARN);
        return;
    }
    if (key == L"rec-explorer") {
        Ask(
            L"Сбросить Проводник?",
            L"Удалю кэш значков и эскизов и перезапущу Проводник: окна папок закроются, панель задач мигнёт. "
            L"Файлы не трогаю.",
            L"Сбросить", L"Не надо",
            [] {
                RunJob(L"Проводник", [](const tools::Log& log, std::wstring&) { return tools::ResetExplorer(log); });
            },
            nullptr, 20000, Toast::G_INFO);
        return;
    }
    if (key == L"rec-boot") {
        Ask(
            L"Перезагрузить в особую загрузку?",
            L"Windows спросит права и через 15 секунд перезагрузится в синее меню. Там: «Поиск и устранение "
            L"неисправностей» - «Дополнительные параметры»: безопасный режим («Параметры загрузки», клавиша 4), "
            L"восстановление системы, командная строка. Сохраните открытое.",
            L"Перезагрузить", L"Не надо",
            [] {
                if (tools::RestartToRecovery())
                    g_card.Show(BoostCard::DONE, L"Перезагрузка через 15 секунд",
                                L"Передумали - Win+R, shutdown /a, Enter.", Theme(), 15000, Toast::G_INFO);
                else
                    g_card.Show(BoostCard::DONE, L"Не получилось", L"Без прав перезагрузку в меню не запустить.",
                                Theme(), 0, Toast::G_WARN);
            },
            nullptr, 30000, Toast::G_WARN);
        return;
    }
    if (key == L"rec-offline") {
        std::wstring av = tools::AntivirusUi();
        if (!av.empty()) {
            Ask(
                L"Офлайн-проверка: Kaspersky",
                L"Пока работает Kaspersky, Defender выключен, и его офлайн-проверка не запустится. В Kaspersky - "
                L"«Проверка» - «Полная». Если вирус мешает самому антивирусу: Kaspersky Virus Removal Tool (KVRT) "
                L"или загрузочная флешка Kaspersky Rescue Disk.",
                L"Открыть Kaspersky", L"Понятно",
                [av] { ShellExecuteW(nullptr, L"open", av.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }, nullptr, 40000,
                Toast::G_INFO);
            return;
        }
        Ask(
            L"Офлайн-проверка Defender?",
            L"Компьютер сразу перезагрузится и минут 15 будет проверяться до загрузки Windows - так находятся "
            L"вирусы, которые прячутся от обычной проверки. Сохраните открытое. Нужны права.",
            L"Перезагрузить и проверить", L"Не надо",
            [] {
                RunJob(L"Офлайн-проверка", [](const tools::Log& log, std::wstring&) {
                    return net::RunAdmin(net::JOB_DEFENDER_OFFLINE, log, 180000);
                });
            },
            nullptr, 30000, Toast::G_WARN);
        return;
    }
    if (key == L"rec-memo") {
        Ask(
            L"Если Windows не грузится",
            L"1. Две-три неудачные загрузки подряд - Windows сама откроет «Автоматическое восстановление»; или на "
            L"экране входа - Shift + «Перезагрузка». 2. Там: «Поиск и устранение неисправностей» - "
            L"«Дополнительные параметры»: «Восстановление системы» (точки, что делает «Снимок»), «Удалить "
            L"обновления», «Параметры загрузки» - 4, безопасный режим. 3. Меню не открывается вовсе - флешка "
            L"восстановления (от 16 ГБ); её делают заранее, на работающей Windows.",
            L"Сделать флешку", L"Понятно",
            [] {
                ShellExecuteW(nullptr, L"open", Sys32(L"RecoveryDrive.exe").c_str(), nullptr, Sys32(L"").c_str(),
                              SW_SHOWNORMAL);
            },
            nullptr, 90000, Toast::G_INFO);
        return;
    }
    // ---------------- система ----------------
    if (key == L"sys-reboot") {
        std::vector<std::wstring> why = tools::PendingReboot();
        if (why.empty()) {
            g_card.Show(BoostCard::DONE, L"Перезагрузка не нужна", L"Windows уже ничего не ждёт.", Theme(), 0,
                        Toast::G_OK);
            return;
        }
        Ask(L"Перезагрузить через 30 секунд?", L"Успеете сохранить открытое; отменить - shutdown /a.", L"Перезагрузить",
            L"Не надо", [] { RebootSoon(); });
        return;
    }
    if (key == L"sys-wu") {
        ShellExecuteW(nullptr, L"open", L"ms-settings:windowsupdate", nullptr, nullptr, SW_SHOWNORMAL);
        if (!g_updates.failed.empty())
            g_card.Show(BoostCard::DONE, L"Обновления не ставятся",
                        L"Если «Повторить» в Центре обновления не помогает - «Починка», «Центр обновления» "
                        L"сбросит его кэш.",
                        Theme(), 20000, Toast::G_INFO);
        return;
    }
    if (key == L"sys-dev") {
        ShellExecuteW(nullptr, L"open", L"devmgmt.msc", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (key == L"sys-gpu")
        return RunJob(L"Видеокарта", [](const tools::Log& log, std::wstring& res) {
            log(L"Спрашиваю nvidia-smi...");
            tools::GpuSample g = tools::GpuNow();
            if (!g.ok) {
                res = L"nvidia-smi не ответил: видеокарта NVIDIA спит или драйвер без него.";
                return false;
            }
            wchar_t w[32];
            swprintf(w, 32, L"%.0f", g.watts);
            res = std::to_wstring(g.temp) + L" °C, нагрузка " + std::to_wstring(g.load) + L"%, " + w + L" Вт" +
                  (g.thermal    ? L". Упирается в перегрев - частота снижена."
                   : g.powerCap ? L". Упирается в предел питания - это обычно для ноутбука."
                                : L". Ни во что не упирается.");
            return true;
        });
    if (key == L"sys-battery") {
        tools::BatteryTrend t = tools::BatteryHistory();
        if (!t.designed || !t.now) {
            g_card.Show(BoostCard::DONE, L"Батарея", L"Драйвер батареи не отдаёт ёмкость.", Theme(), 0, Toast::G_INFO);
            return;
        }
        auto wh = [](ULONGLONG mwh) {
            return std::to_wstring(mwh / 1000) + L"," + std::to_wstring(mwh % 1000 / 100) + L" Вт*ч";
        };
        int wear = t.now >= t.designed ? 0 : (int)(100 - t.now * 100 / t.designed);
        std::wstring text =
            L"Держит " + wh(t.now) + L" из " + wh(t.designed) + L" по паспорту, износ " + std::to_wstring(wear) + L"%.";
        if (t.monthAgo && t.monthDays >= 7)
            text += L" " + Days(t.monthDays) + L" назад было " + wh(t.monthAgo) +
                    (t.monthAgo > t.now ? L", минус " + wh(t.monthAgo - t.now) + L"." : L" - без потерь.");
        else
            text += L" История копится с этой версии: через неделю будет видно, как быстро уходит ёмкость.";
        if (t.mostlyAc)
            text +=
                L" Последние две недели ноутбук почти всё время на зарядке: в Armoury Crate включите "
                L"ограничение заряда (80%) - батарея будет изнашиваться заметно медленнее.";
        g_card.Show(BoostCard::DONE, L"Износ батареи " + std::to_wstring(wear) + L"%", text, Theme(), 40000,
                    Toast::G_INFO);
        return;
    }
    if (key == L"sys-report")
        return RunJob(L"Отчёт о системе", [](const tools::Log& log, std::wstring& res) {
            log(L"Собираю: железо, диски, драйверы, находки «Защиты»...");
            std::wstring text = tools::SystemReport();
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t day[16];
            swprintf(day, 16, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
            std::wstring path = KnownFolder(FOLDERID_Documents) + L"\\Отчёт RadialMenu " + day + L".txt";
            // UTF-8 с меткой: Блокнот откроет русский текст правильно
            WriteSmall(path, "\xEF\xBB\xBF" + Narrow(text));
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                res = L"Не записался: " + path;
                return false;
            }
            ShellExecuteW(nullptr, L"open", Sys32(L"notepad.exe").c_str(), (L"\"" + path + L"\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
            res = L"Сохранён: " + path + L". Перед отправкой посмотрите, нет ли лишних путей и имён.";
            return true;
        });
    if (key == L"sec-privacy") {
        std::vector<tools::PrivacyUse> uses = tools::PrivacyUses();
        ULONGLONG now = FtNow(), month = 30ull * 24 * 3600 * 10000000;
        // одна строка на программу: что брала, когда последний раз; «сейчас» - первыми (так уже отсортировано)
        struct Row {
            std::wstring app, caps;
            ULONGLONG start = 0;
            bool inUse = false, unsignedApp = false;
        };
        std::vector<Row> rows;
        for (const tools::PrivacyUse& u : uses) {
            if (!u.inUse && now - u.start > month) continue;
            auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.app == u.app; });
            if (it == rows.end()) {
                rows.push_back({u.app, u.capability, u.start, u.inUse, u.unsignedApp});
                continue;
            }
            if (it->caps.find(u.capability) == std::wstring::npos) it->caps += L", " + u.capability;
            it->inUse |= u.inUse;
            it->unsignedApp |= u.unsignedApp;
            it->start = std::max(it->start, u.start);
        }
        std::wstring text;
        for (size_t i = 0; i < rows.size() && i < 8; ++i)
            text += (text.empty() ? L"" : L"; ") + NoExe(rows[i].app) + L" (" + rows[i].caps + L") " +
                    (rows[i].inUse ? L"сейчас" : FtText(rows[i].start)) +
                    (rows[i].unsignedApp ? L", без подписи" : L"");
        if (rows.size() > 8) text += L" и ещё " + std::to_wstring(rows.size() - 8);
        bool now1 = !rows.empty() && rows[0].inUse;
        Ask(
            now1 ? L"Камера или микрофон заняты: " + NoExe(rows[0].app) : L"Камера и микрофон за 30 дней",
            text.empty() ? L"За 30 дней никто не брал камеру, микрофон и геопозицию."
                         : text + L". Незнакомое - запретить в Параметрах, а программу проверить в «Защите».",
            L"Параметры", L"Понятно",
            [] { ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL); },
            nullptr, 45000, now1 ? Toast::G_WARN : Toast::G_INFO);
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
                std::wstring sys = Sys32(L""), exe = Sys32(L"WindowsPowerShell\\v1.0\\powershell.exe");
                if ((INT_PTR)ShellExecuteW(nullptr, L"runas", exe.c_str(), ps, sys.c_str(), SW_SHOWNORMAL) <= 32)
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
    if (key == L"sec-sandbox") {
        std::wstring file = tools::ClipboardPath();
        DWORD a = file.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(file.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
            g_card.Show(BoostCard::DONE, L"Песочница: какой файл?",
                        L"Выделите exe (или установщик) в Проводнике, нажмите Ctrl+C и выберите «Песочницу» снова.",
                        Theme(), 12000, Toast::G_INFO);
            return;
        }
        OpenInSandbox(file);
        return;
    }
    if (key.rfind(L"sec-app:", 0) == 0) {
        size_t i = idx(8, g_secApps.size());
        if (i == (size_t)-1) return;
        std::wstring exe = g_secApps[i].exe, name = g_secApps[i].name;
        auto launch = [exe] {
            std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
            if ((INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL) <= 32)
                g_card.Show(BoostCard::DONE, L"Не запустилось", exe, Theme(), 0, Toast::G_WARN);
        };
        // папка защиты - обычная папка пользователя: туда может лечь и чужой exe. Подписанное доверенным
        // издателем запускается сразу, остальное - только после вопроса
        std::wstring signer;
        tools::SignState st = tools::CheckSign(exe, &signer);
        if (st == tools::SIGN_OK) {
            launch();
            return;
        }
        Ask(
            name + (st == tools::SIGN_BAD ? L": подпись повреждена" : L": без проверенной подписи"),
            exe +
                (st == tools::SIGN_BAD
                     ? L" изменён после подписи - его лучше удалить и скачать заново."
                     : L". Издатель не подтверждён: сначала стоит открыть его в песочнице или сверить хэш на "
                       L"VirusTotal.") +
                (tools::FileOrigin(exe).empty() ? L"" : L" Скачан: " + Short(tools::FileOrigin(exe), 100) + L"."),
            L"Запустить", tools::SandboxAvailable() ? L"Песочница" : L"VirusTotal", launch,
            [exe] {
                if (tools::SandboxAvailable())
                    OpenInSandbox(exe);
                else
                    OpenVirusTotal(exe);
            },
            30000, Toast::G_WARN);
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
        std::vector<tools::Suspect> all = AllFinds(g_sec);
        size_t n = all.size();
        int procs = g_sec.processes, autos = g_sec.autoruns;
        ReleaseSRWLockShared(&g_secLock);
        std::wstring checked = L"Проверено " + Plural(procs, L"программа", L"программы", L"программ") + L" и " +
                               Plural(autos, L"запись", L"записи", L"записей") + L" автозапуска.";
        if (SceneCard::Get().Visible()) {  // итог - в сцене: чистые слетаются в щит, находки - строками под ним
            std::vector<scene::GuardFind> finds;
            for (const tools::Suspect& s : all) finds.push_back({s.name, s.why, s.score});
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
    for (const tools::Volume& v : tools::VolumesCached()) {
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

void TrimDdc() { Run(L"ddc-trim"); }

SecurityView SecurityFinds() {
    SecurityView v;
    AcquireSRWLockShared(&g_secLock);
    v.checked = g_secAt != 0;
    v.finds = AllFinds(g_sec);
    v.processes = g_sec.processes;
    v.autoruns = g_sec.autoruns;
    g_secStarts = g_sec.starts;  // SecurityAct берёт запись автозагрузки по номеру из этой проверки
    ReleaseSRWLockShared(&g_secLock);
    v.running = g_secRunning;
    v.ignored = (int)Ignored().size();
    if (!v.checked && !v.running && g_cfg) RefreshSecurity();
    return v;
}

void SecurityRescan() { RefreshSecurity(); }

void PrimeSecurityForSnapshot() {
    tools::SecurityScan sec = tools::ScanSecurity();
    AcquireSRWLockExclusive(&g_secLock);
    g_sec = std::move(sec);
    g_secAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_secLock);
}

void SecurityAct(const tools::Suspect& s) { SecurityAction(s); }

void IgnoreFind(const tools::Suspect& s) {
    Ignored();
    g_ignored.insert(FindKey(s));
    g_secWarned.insert(FindKey(s));
    SaveIgnored();
}

void ClearIgnored() {
    Ignored();
    g_ignored.clear();
    SaveIgnored();
}

const wchar_t* FindKindName(tools::FindKind k) { return FindWhat(k); }

const wchar_t* FindKindIcon(tools::FindKind k) { return FindIcon(k); }

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

// Игра закончилась: как жила видеокарта (замеры раз в 10 секунд из слежки).
void GameGpuSummary() {
    AcquireSRWLockShared(&g_watchLock);
    GpuStats g = g_gpu;
    ReleaseSRWLockShared(&g_watchLock);
    if (g.samples < 6) return;  // меньше минуты - не о чем говорить
    int hot = g.thermal * 100 / g.samples, power = g.power * 100 / g.samples;
    if (g.max < 80 && hot < 5) return;  // всё спокойно - карточку не показываем
    g_card.Show(BoostCard::DONE, L"Видеокарта в игре: до " + std::to_wstring(g.max) + L" °C",
                hot >= 5
                    ? L"Упиралась в перегрев " + std::to_wstring(hot) +
                          L"% времени - частота падала. Помогут подставка, чистка вентиляторов и режим "
                          L"«Турбо» в Armoury Crate."
                    : L"Горячо, но без торможения. Ограничение по питанию: " + std::to_wstring(power) + L"% времени.",
                Theme(), 20000, Toast::G_INFO);
}

}  // namespace toolsui
