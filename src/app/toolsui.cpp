#include "app/toolsui_internal.h"

namespace toolsui {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
BoostCard g_card;
std::function<void(int, const std::wstring&)> g_openStudio;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_jobCancel{false};  // «Стоп» в карточке задачи RunJob (stoppable)
bool g_jobStoppable = false;           // у текущей задачи есть «Стоп»; только поток окна
std::atomic<bool> g_cleanBusy{false};  // чистка кэшей: одна на всю программу (см. CleanClaim)
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
std::vector<tools::DupGroup> g_dupList;  // что показано в кольце (номера dup:i)
std::vector<tools::Suspect> g_finds;     // «Защита»: что показано в кольце
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
std::atomic<ULONGLONG> g_secAt{0};  // 0 - ещё не проверяли
std::atomic<bool> g_secRunning{false};
std::set<std::wstring> g_secWarned;  // о чём уже сказали карточкой (путь или запись)
bool g_secAsked = false;             // проверку просили из кольца: итог показать, даже если всё чисто

SRWLOCK g_scanLock = SRWLOCK_INIT;
Scan g_scan;
std::atomic<ULONGLONG> g_scanAt{0};  // 0 - не считалось или нужно пересчитать
ULONGLONG g_started = 0;
std::atomic<bool> g_scanRunning{false}, g_weeklyRunning{false};
// меняется, когда размеры кэшей поправила чистка: фоновый подсчёт, начатый раньше, их не перетирает
std::atomic<unsigned> g_junkGen{0};
// номер изменения g_scan (для SnapshotIfChanged): растёт под g_scanLock при каждой правке того, что в State
unsigned g_scanGen = 0;
std::vector<std::wstring> g_roots;  // где искать проекты (из колец), обновляется при открытии кольца

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

// «Стоп» в карточке хода: каждый показ WORKING ставит кнопку заново (между ними карточку мог занять другой итог)
void ArmStop() {
    if (g_jobStoppable && !g_jobCancel) g_card.onStop = [] { g_jobCancel = true; };
}

void RunJob(const std::wstring& title, std::function<bool(const tools::Log&, std::wstring&)> work, bool quiet,
            bool stoppable) {
    if (g_busy) {
        g_card.Show(BoostCard::DONE, L"Подождите", L"Предыдущее действие ещё идёт.", Theme());
        return;
    }
    g_busy = true;
    g_jobCancel = false;
    g_jobStoppable = stoppable && !quiet;
    if (!quiet) g_card.Show(BoostCard::WORKING, title, L"Начинаю...", Theme());
    ArmStop();
    bool started = bg::Run([title, work = std::move(work), quiet] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring last;
        tools::Log log = [&](const std::wstring& s) {
            last = s;
            auto* m = new JobMsg{title, s, false, false, quiet};
            if (!PostMessageW(g_app, WM_TOOLS_JOB, 0, (LPARAM)m)) delete m;
        };
        std::wstring result;
        bool ok = work(log, result);
        auto* m = new JobMsg{title, result.empty() ? last : result, true, ok, quiet};
        if (!PostMessageW(g_app, WM_TOOLS_JOB, 0, (LPARAM)m)) delete m;
        CoUninitialize();
    });
    if (!started) g_busy = false, g_jobStoppable = false;
}

std::wstring Short(std::wstring s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + L"…" : s; }

// Очистка с живой сценой: места по одному, лента scene::Clean ведёт вихрь в карточке (и в студии, если открыта).
void RunClean(const std::wstring& title, const std::vector<tools::Junk>& list) {
    if (!CleanClaim()) {
        g_card.Show(BoostCard::DONE, L"Подождите", L"Уже идёт очистка.", Theme());
        return;
    }
    if (g_busy) {
        CleanRelease();
        return RunJob(title, nullptr);  // скажет «подождите»
    }
    std::vector<scene::CleanItem> items;
    for (const tools::Junk& j : list) items.push_back({j.name, j.bytes});
    scene::CleanBegin(items);
    SceneCard::Get().Show(SceneCard::CLEAN, title, Theme());
    RunJob(
        title,
        [list](const tools::Log& log, std::wstring& res) {
            std::wstring who;
            res = L"Освобождено " + tools::Bytes(CleanList(list, log, &who)) + L"." +
                  (who.empty() ? L"" : L" Не дали удалить: " + who + L".");
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

// Фоновый подсчёт: кэши, кэш Unreal, мусор проектов, конфликты PATH; заодно -
// снимок размеров для «роста». Части меняются с разной скоростью, и обходить
// всё раз в полчаса - лишняя работа диска (особенно от батареи): кэши, TEMP,
// кэш Unreal и PATH - каждый раз; мусор проектов, git и снимок роста - раз в 3
// часа; диски WSL, хвосты программ, устройства и история обновлений - раз в
// сутки. full - всё сразу (кнопка «Обновить»).
constexpr ULONGLONG kProjEvery = 3 * 3600 * 1000ull, kSysEvery = 24 * 3600 * 1000ull;
std::atomic<ULONGLONG> g_projAt{0}, g_sysAt{0};

void ScanPass(const std::vector<std::wstring>& roots, bool proj, bool sys) {
    Scan s;
    unsigned gen = g_junkGen;
    AcquireSRWLockShared(&g_scanLock);
    Scan prev = g_scan;  // что в этот раз не пересчитываем - берём из прошлого
    ReleaseSRWLockShared(&g_scanLock);
    s.junk = tools::FindJunk(true);
    s.ddcOld = tools::OldBytes(tools::DdcDir(), kDdcDays);
    s.projects = proj ? tools::FindProjectJunk(roots) : prev.projects;
    s.conflicts = tools::PathConflicts();
    if (proj) {
        // снимок размеров на сегодня: кэши, мусор проектов по проектам, обычные
        // папки (рост сравнивается по дням)
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
        for (const auto& [project, b] : perProject)
            names.push_back({L"Мусор " + Leaf(project), project + L"\\*сборка"}), bytes.push_back(b);
        for (const KNOWNFOLDERID* id : {&FOLDERID_Downloads, &FOLDERID_Desktop, &FOLDERID_Videos}) {
            std::wstring f = KnownFolder(*id);
            if (f.empty()) continue;
            names.push_back({Leaf(f), f});
            bytes.push_back(tools::DirBytes(f));
        }
        tools::RecordSizes(names, bytes);
        tools::RecordFree();  // прогноз места (И10)
    }
    s.growth = proj ? tools::WeekGrowth(7) : prev.growth;
    CoInitializeEx(nullptr,
                   COINIT_MULTITHREADED);  // история обновлений - через COM
    s.git = proj ? tools::GitRepos(roots) : prev.git;
    s.vhd = sys ? tools::VirtualDisks() : prev.vhd;
    s.leftovers = sys ? tools::AppLeftovers() : prev.leftovers;
    s.devices = sys ? tools::DeviceProblems() : prev.devices;
    s.updates = sys ? tools::WindowsUpdates() : prev.updates;
    CoUninitialize();
    if (bg::Quitting()) return;
    AcquireSRWLockExclusive(&g_scanLock);
    if (gen != g_junkGen) s.junk = g_scan.junk;  // пока считали, прошла чистка: её размеры свежее
    s.driver = g_scan.driver;                    // драйвер и версии проверяются отдельно
    s.versions = g_scan.versions;
    g_scan = std::move(s);
    ++g_scanGen;
    g_scanAt = GetTickCount64();
    if (proj) g_projAt = g_scanAt.load();
    if (sys) g_sysAt = g_scanAt.load();
    ReleaseSRWLockExclusive(&g_scanLock);
}

void RefreshScan(bool full) {
    if (!g_cfg || g_scanRunning.exchange(true)) return;  // студия в режиме снимка: утилиты не запущены
    ULONGLONG now = GetTickCount64();
    bool proj = full || !g_projAt || now - g_projAt > kProjEvery;
    bool sys = full || !g_sysAt || now - g_sysAt > kSysEvery;
    std::vector<std::wstring> roots = g_roots.empty() ? ProjectRoots(*g_cfg) : g_roots;
    bool started = bg::Run(
        [roots, proj, sys] {
            ScanPass(roots, proj, sys);
            g_scanRunning = false;
        },
        THREAD_PRIORITY_LOWEST);  // фоновый обход диска не мешает работе
    if (!started) g_scanRunning = false;
}

// Проверка «Защиты» в фоне: подписи, места запуска, автозапуск, перехваты в реестре. Итог - сообщением в окно,
// чтобы карточка об опасном показалась из основного потока.
void RefreshSecurity() {
    if (!g_app || g_secRunning.exchange(true)) return;
    bool started = bg::Run(
        [] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            tools::SecurityScan s = tools::ScanSecurity();
            CoUninitialize();
            AcquireSRWLockExclusive(&g_secLock);
            g_sec = std::move(s);
            g_secAt = GetTickCount64();
            ReleaseSRWLockExclusive(&g_secLock);
            g_secRunning = false;
            PostMessageW(g_app, WM_TOOLS_JOB, kSecTag, 0);
        },
        THREAD_PRIORITY_LOWEST);
    if (!started) g_secRunning = false;
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
         std::function<void()> onYes, std::function<void()> onNo, int hold, Toast::Glyph glyph) {
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

// Остатки обновлений Windows: кэш Delivery Optimization и вытесненные версии компонентов - через задачу с правами.
void SysClean() {
    Ask(L"Почистить остатки обновлений?",
        L"Кэш Delivery Optimization и старые версии компонентов Windows (DISM). Установленные обновления остаются, "
        L"последние по-прежнему можно удалить. Нужны права администратора, 10-25 минут.",
        L"Почистить", L"Не надо", [] {
            RunJob(
                L"Остатки обновлений",
                [](const tools::Log& log, std::wstring& res) {
                    ULONGLONG freed = 0;
                    bool ok = net::RunAdmin(
                        net::JOB_SYS_CLEAN,
                        [&](const std::wstring& s) {
                            if (s.rfind(L"@freed=", 0) == 0)
                                freed = _wcstoui64(s.c_str() + 7, nullptr, 10);
                            else
                                log(s);
                        },
                        100 * 60 * 1000, &g_jobCancel);  // два замера DISM и сама чистка
                    res = ok            ? L"Освобождено " + tools::Bytes(freed) + L"."
                          : g_jobCancel ? L"Остановлено: DISM доделает чистку в фоне, итог не жду."
                                        : L"Не получилось - подробности выше.";
                    return ok;
                },
                false, true);
        });
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

std::atomic<bool> g_game{false};
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

struct SelfMsg {
    std::wstring line, leak;
};

struct LeftMsg {
    net::Job job;
    bool ok;
    std::wstring last;
};

std::wstring JobTitle(net::Job j) {
    switch (j) {
        case net::JOB_REPAIR:
            return L"Починка Windows";
        case net::JOB_SYS_CLEAN:
            return L"Остатки обновлений";
        case net::JOB_VHD_COMPACT:
            return L"Сжатие дисков";
        case net::JOB_MFT_SIZES:
            return L"Чтение таблицы MFT";
        default:
            return L"Задание с правами";
    }
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

    ULONGLONG lastIn = 0, lastOut = 0, lastAt = 0, nextFull = 0, quickAt = 0, gpuAt = 0, selfAt = 0;
    std::vector<tools::SelfSample> self;  // раз в час, за последние сутки с запасом
    int n = 0;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= selfAt) {  // своя память и дескрипторы: в журнал, рост за сутки - карточка (утечка у себя)
            self.push_back(tools::SampleSelf());
            if (self.size() > 30) self.erase(self.begin());
            auto* m = new SelfMsg{tools::SelfLine(self.back()), L""};
            tools::SelfLeak(self, m->leak);
            if (!PostMessageW(g_app, WM_TOOLS_JOB, kSelfTag, (LPARAM)m)) delete m;
            selfAt = now + 3600 * 1000;
        }
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

// weekly.ini пишут и поток окна, и фоновые проверки (недельная, версии): чтение-правка-запись файла - под замком
SRWLOCK g_dayLock = SRWLOCK_INIT;

int LastDay(const std::wstring& what) {
    wchar_t v[32] = L"";
    AcquireSRWLockShared(&g_dayLock);
    GetPrivateProfileStringW(L"weekly", what.c_str(), L"0", v, 32, (tools::DataDir() + L"\\weekly.ini").c_str());
    ReleaseSRWLockShared(&g_dayLock);
    return _wtoi(v);
}

void SetDay(const std::wstring& what, int day) {
    AcquireSRWLockExclusive(&g_dayLock);
    WritePrivateProfileStringW(L"weekly", what.c_str(), std::to_wstring(day).c_str(),
                               (tools::DataDir() + L"\\weekly.ini").c_str());
    ReleaseSRWLockExclusive(&g_dayLock);
}

struct WeeklyMsg {
    std::wstring growth, health;
    tools::Driver driver;
    std::vector<tools::ToolVersion> versions;
    bool versionsChecked = false;
};

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
    Want wanted{growth, health, driver, versions};
    bool started = bg::Run(
        [wanted] {
            const Want* want = &wanted;
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
        },
        THREAD_PRIORITY_LOWEST);
    if (!started) g_weeklyRunning = false;
}

void OnWeekly(WeeklyMsg* m) {
    if (m->driver.ok) {
        AcquireSRWLockExclusive(&g_scanLock);
        g_scan.driver = m->driver;
        ++g_scanGen;
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
    // остановили по «Стоп», а задание доработало в фоне - итог карточкой (из фонового потока - в поток окна)
    net::SetLeftDone([](net::Job j, bool ok, const std::wstring& last) {
        auto* m = new LeftMsg{j, ok, last};
        if (!PostMessageW(g_app, WM_TOOLS_JOB, kLeftTag, (LPARAM)m)) delete m;
    });
    g_watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_watchThread = CreateThread(nullptr, 0, WatchLoop, nullptr, 0, nullptr);
    GuardStart();
    // живой рост: Загрузки и TEMP - чаще всего именно они съедают место незаметно
    tools::LiveStart({{L"Загрузки", KnownFolder(FOLDERID_Downloads)}, {L"TEMP", tools::TempDir()}});
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

// «Мало места»: что предложить, кроме общих слов. Кэши и давние проекты - из последнего фонового подсчёта, самое
// большое на диске - из свежей таблицы MFT (мгновенно; без неё обход диска шёл бы минутами - тогда не говорим).
struct DiskMsg {
    std::wstring root;
    ULONGLONG free = 0, total = 0;
    std::wstring advice;
};

std::wstring DiskAdvice(const std::wstring& root) {
    State s = Snapshot();
    auto onDisk = [&](const std::wstring& p) { return p.size() >= 2 && _wcsnicmp(p.c_str(), root.c_str(), 2) == 0; };
    constexpr ULONGLONG kWorth = 100ull * 1024 * 1024;
    std::vector<std::wstring> parts;
    ULONGLONG caches = 0;
    for (const tools::Junk& j : s.junk)
        if (!j.slow && onDisk(j.path)) caches += j.bytes;
    if (caches >= kWorth) parts.push_back(L"кэши и TEMP - " + tools::Bytes(caches));
    ULONGLONG idle = 0;
    std::vector<std::wstring> names;
    for (const tools::ProjectJunk& p : s.projects) {
        if (p.idleDays < kIdleDays || !onDisk(p.path)) continue;
        idle += p.bytes;
        std::wstring n = Leaf(p.project);
        if (names.size() < 3 && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    }
    if (idle >= kWorth) {
        std::wstring list;
        for (const std::wstring& n : names) list += (list.empty() ? L"" : L", ") + n;
        parts.push_back(L"сборки давних проектов - " + tools::Bytes(idle) + L" (" + list + L")");
    }
    std::wstring out;
    // прогноз (И10): через сколько дней кончится и что растёт быстрее всего
    for (const tools::Forecast& f : tools::SpaceForecast())
        if (onDisk(f.root) && f.days >= 0 && f.days <= 365) {
            out = L"При нынешнем темпе " + tools::ForecastText(f).substr(3) + L".";
            for (const tools::Growth& g : s.growth)
                if (onDisk(g.path) && g.delta > (LONGLONG)kWorth) {
                    out += L" Быстрее всего растёт " + g.name + L" (+" + tools::Bytes((ULONGLONG)g.delta) +
                           L" за неделю).";
                    break;
                }
            out += L" ";
        }
    if (!parts.empty()) {
        out += L"Можно освободить: ";
        for (size_t i = 0; i < parts.size(); ++i) out += (i ? L"; " : L"") + parts[i];
        out += L".";
    }
    std::vector<tools::FolderSize> top;
    if (tools::MftCached(root, top)) {
        std::wstring big;
        int n = 0;
        for (const tools::FolderSize& f : top) {
            if (f.name.empty() || f.name[0] == L'(' || n >= 2) continue;
            big += (n++ ? L", " : L"") + f.name + L" " + tools::Bytes(f.bytes);
        }
        if (!big.empty()) out += (out.empty() ? L"" : L" ") + std::wstring(L"Больше всего занимают: ") + big + L".";
    }
    return out;
}

void OnJob(WPARAM wp, LPARAM lp) {
    if (wp == kDiskTag) {
        std::unique_ptr<DiskMsg> m((DiskMsg*)lp);
        if (g_busy) return;
        std::wstring root = m->root;
        Ask(
            L"Мало места на " + root.substr(0, 2),
            L"Свободно " + tools::Bytes(m->free) + L" из " + tools::Bytes(m->total) + L". " +
                (m->advice.empty() ? L"Кэши, мусор проектов и старое из кэша Unreal - в кольце «Утилиты»." : m->advice),
            L"Что занимает", L"Позже",
            [root] {
                if (g_openStudio) g_openStudio(1, root);
            },
            nullptr, 40000, Toast::G_WARN);
        return;
    }
    if (wp == kLeftTag) {
        std::unique_ptr<LeftMsg> m((LeftMsg*)lp);
        std::wstring title = JobTitle(m->job);
        autos::Note(title + L": доделано в фоне после «Стоп», " + (m->ok ? L"успешно" : L"с ошибкой") +
                    (m->last.empty() ? L"" : L". " + m->last));
        if (m->job == net::JOB_MFT_SIZES) return;  // таблица просто ляжет в кэш - карточка ни к чему
        g_card.Show(BoostCard::DONE, m->ok ? title + L": доделано в фоне" : title + L": не получилось",
                    (m->last.empty() ? std::wstring(L"Задание, остановленное по «Стоп», закончилось.") : m->last) +
                        (m->job == net::JOB_REPAIR ? L" Журнал SFC: C:\\Windows\\Logs\\CBS\\CBS.log." : L""),
                    Theme(), 20000, m->ok ? Toast::G_OK : Toast::G_FAIL);
        return;
    }
    if (wp == kSelfTag) {
        std::unique_ptr<SelfMsg> m((SelfMsg*)lp);
        autos::Note(m->line);  // журнал пишет поток окна: так строки не перемешаются с остальной автоматикой
        static ULONGLONG said = 0;
        ULONGLONG now = GetTickCount64();
        if (!m->leak.empty() && (!said || now - said > 24 * 3600 * 1000ull) && !g_game) {
            said = now;
            autos::Note(L"Похоже на утечку у себя: " + m->leak + L" за сутки");
            g_card.Show(BoostCard::DONE, L"RadialMenu растёт",
                        L"За сутки: " + m->leak +
                            L". Похоже на утечку у себя - перезапуск вернёт память; строки "
                            L"по часам - в журнале автоматики.",
                        Theme(), 20000, Toast::G_WARN);
        }
        return;
    }
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
        // «@pct 45 DISM: 45%» - ход долгого задания с правами: число - в полоску, остальное - строкой
        float pct = -1;
        std::wstring text = m->text;
        if (text.rfind(L"@pct ", 0) == 0) {
            pct = _wtoi(text.c_str() + 5) / 100.0f;
            size_t sp = text.find(L' ', 5);
            text = sp == std::wstring::npos ? L"" : text.substr(sp + 1);
        }
        g_card.progress = pct;  // до Show: высота карточки считается с полоской
        g_card.Show(BoostCard::WORKING, m->title + L"...", text, Theme());
        ArmStop();
        return;
    }
    g_busy = false;
    g_jobStoppable = false;
    g_card.Show(BoostCard::DONE, m->ok ? m->title + L": готово" : m->title + L": не получилось", m->text, Theme(),
                9000);
}

// TEMP за час вырос на 5 ГБ и больше - что-то пишет без остановки (зависшая сборка, распаковка, кэш программы).
// Не чаще раза в 2 часа и не в игре.
void LiveAlert(ULONGLONG now) {
    static ULONGLONG said = 0;
    if (g_game || (said && now - said < 2 * 3600 * 1000ull)) return;
    for (const tools::LiveSize& l : tools::LiveSizes()) {
        if (!l.ready || l.path != tools::TempDir() || l.hour < 5ll * 1024 * 1024 * 1024) continue;
        said = now;
        Ask(
            L"TEMP растёт: +" + tools::Bytes((ULONGLONG)l.hour) + L" за час",
            L"Временные файлы занимают уже " + tools::Bytes(l.now) +
                L". Обычно это зависшая сборка, распаковка или кэш программы. Кто держит файлы - в «Что занимает "
                L"место».",
            L"Показать", L"Позже",
            [p = l.path] {
                if (g_openStudio) g_openStudio(1, p);
            },
            nullptr, 30000, Toast::G_WARN);
        return;
    }
}

void Tick() {
    tools::MftSearchForget(120000);  // индекс поиска папок (палитра) не держим без дела
    GuardTick();
    g_card.Tick();
    SceneCard::Get().Poll();
    SceneCard::Get().Tick();
    ULONGLONG now = GetTickCount64();
    if (now - g_proactiveAt > 5000) TryProactive();  // отложенное, пока была другая карточка
    LiveAlert(now);
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
        // что предложить - в фоне (таблица MFT в кэше - это чтение файла), карточка - сообщением в поток окна
        auto* m = new DiskMsg{v.root, v.free, v.total, L""};
        if (!bg::Run([m] {
                m->advice = DiskAdvice(m->root);
                if (!PostMessageW(g_app, WM_TOOLS_JOB, kDiskTag, (LPARAM)m)) delete m;
            }))
            delete m;
        break;
    }
}

void Shutdown() {
    tools::LiveStop();
    GuardStop();  // приоритеты и запрет сна - как были
    if (g_watchStop) SetEvent(g_watchStop);
    if (g_watchThread) WaitForSingleObject(g_watchThread, 3000), CloseHandle(g_watchThread);
    g_card.Destroy();
}

State Snapshot() {
    State s;
    unsigned gen = ~0u;
    SnapshotIfChanged(s, gen);
    return s;
}

bool SnapshotIfChanged(State& s, unsigned& gen) {
    AcquireSRWLockShared(&g_scanLock);
    bool changed = gen != g_scanGen;
    if (changed) {
        s.junk = g_scan.junk;
        s.projects = g_scan.projects;
        s.growth = g_scan.growth;
        s.ddcOld = g_scan.ddcOld;
        s.driver = g_scan.driver;
        gen = g_scanGen;
    }
    s.at = g_scanAt;
    ReleaseSRWLockShared(&g_scanLock);
    s.scanning = g_scanRunning;
    return changed;
}

void Refresh() { RefreshScan(true); }

void JunkCleaned(const std::vector<std::pair<std::wstring, ULONGLONG>>& freed) {
    AcquireSRWLockExclusive(&g_scanLock);
    for (tools::Junk& j : g_scan.junk)
        for (const auto& cleaned : freed)
            if (_wcsicmp(cleaned.first.c_str(), j.path.c_str()) == 0) j.bytes = 0;
    // что осталось после чистки, удалить сейчас нельзя: занято (программа держит файл) или свежее (fresh).
    // Обещать это снова - та же кнопка «Тихая чистка N» без толку; фоновый подсчёт пересчитает через полчаса
    g_scan.junk.erase(std::remove_if(g_scan.junk.begin(), g_scan.junk.end(),
                                     [](const tools::Junk& j) { return !j.bytes && !j.fresh; }),
                      g_scan.junk.end());
    ++g_junkGen;
    ++g_scanGen;
    ReleaseSRWLockExclusive(&g_scanLock);
}

void SystemClean() { SysClean(); }

std::wstring BusyNames(const std::vector<std::wstring>& files) {
    std::vector<std::pair<std::wstring, int>> names;  // имя программы и сколько её файлов
    for (const tools::Locker& l : tools::WhoLocksFiles(files)) {
        std::wstring n = NoExe(l.name);
        auto it = std::find_if(names.begin(), names.end(),
                               [&](const auto& x) { return _wcsicmp(x.first.c_str(), n.c_str()) == 0; });
        if (it == names.end())
            names.push_back({n, 1});
        else
            ++it->second;
    }
    std::wstring out;
    for (size_t i = 0; i < names.size() && i < 4; ++i) out += (out.empty() ? L"" : L", ") + names[i].first;
    if (names.size() > 4) out += L" и ещё " + std::to_wstring(names.size() - 4);
    if (out.empty() && !files.empty())
        out = Plural((int)files.size(), L"файл", L"файла", L"файлов") + L" заняты системой";
    return out;
}

bool CleanClaim() {
    bool was = false;
    return g_cleanBusy.compare_exchange_strong(was, true);
}
void CleanRelease() { g_cleanBusy = false; }
bool CleanBusy() { return g_cleanBusy; }

ULONGLONG CleanList(const std::vector<tools::Junk>& list, const tools::Log& log, std::wstring* lockers) {
    std::vector<std::pair<std::wstring, ULONGLONG>> freed;
    std::vector<std::wstring> busy;
    ULONGLONG total = tools::CleanJunks(
        list, [](size_t i) { scene::CleanMark(i, 1); },
        [&](size_t i, ULONGLONG f) {
            scene::CleanMark(i, 2, f);
            freed.push_back({list[i].path, f});
        },
        log, &busy);
    JunkCleaned(freed);  // «можно освободить» - сразу по факту, без второго обхода и не через полчаса
    scene::CleanEnd();
    CleanRelease();
    // занятое: кто держит - по именам программ, чтобы было ясно, что закрыть
    std::wstring who = BusyNames(busy);
    if (!who.empty() && log) log(L"Не дали удалить: " + who + L".");
    if (lockers) *lockers = who;
    return total;
}

void PrimeForSnapshot(const Config* cfg) {
    if (!g_cfg) g_cfg = cfg;
    RefreshScan(true);
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
        bg::Run([] {
            scene::CleanFeed f = scene::Clean();
            for (size_t i = 0; i < f.items.size(); ++i) {
                Sleep(200);
                scene::CleanMark(i, 1);
                Sleep(800);
                scene::CleanMark(i, 2, f.items[i].bytes * 97 / 100);
            }
            scene::CleanEnd();
        });
        return;
    }
    scene::GuardBegin((int)tools::Processes().size());
    SceneCard::Get().Show(SceneCard::GUARD, L"Защита: проверка", Theme());
    bg::Run([what] {
        Sleep(2200);
        std::vector<scene::GuardFind> finds;
        if (what == 1)
            finds = {{L"scvhost.exe", L"имя как у svchost, а лежит в TEMP; без подписи", 7},
                     {L"wscript.exe", L"запуск скрипта при входе из Run", 4}};
        scene::GuardEnd(scene::Guard().expect, finds);
    });
}

void TrimDdc() { Run(L"ddc-trim"); }

SecurityView SecurityFinds() {
    SecurityView v;
    AcquireSRWLockShared(&g_secLock);
    v.checked = g_secAt != 0;
    v.finds = AllFinds(g_sec);
    v.processes = g_sec.processes;
    v.autoruns = g_sec.autoruns;
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
