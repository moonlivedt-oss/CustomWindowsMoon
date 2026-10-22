#include "app/toolsui_internal.h"

#include <map>

#include "core/perf.h"
#include "tools/important.h"

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
std::vector<tools::BrowserExt> g_exts;
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

// Подпись находки в кольце: «Брандмауэр: javaw» -> «javaw» - вид и так виден по значку, а в карточке - целиком
std::wstring RingName(const std::wstring& s) {
    size_t c = s.find(L": ");
    return c != std::wstring::npos && c <= 14 && c + 2 < s.size() ? s.substr(c + 2) : s;
}

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
// Обход кэшей и проектов взят с диска (О5): насколько он был старше g_projAt (после загрузки ПК счётчик тиков меньше
// возраста файла - такой разницы в g_projAt не записать). Сбрасывается настоящим обходом.
std::atomic<ULONGLONG> g_projExtra{0};

bool ProjDue(ULONGLONG now) { return !g_projAt || now - g_projAt + g_projExtra > kProjEvery; }
// Старт мягче (Н9): тяжёлое после входа - не разом, а по очереди. Подсчёт места (кэши, проекты) - через 90 с,
// «Защита» - через 4 мин, суточный проход (снимок недели с подписями, устройства, обновления) - через 7 мин,
// недельные проверки (здоровье дисков, версии) - через 10 мин.
constexpr ULONGLONG kScanDelay = 90 * 1000, kSecDelay = 4 * 60 * 1000, kSysDelay = 7 * 60 * 1000,
                    kWeeklyDelay = 10 * 60 * 1000;

// Тяжёлое - не от батареи (Н8): обход кэшей, суточный проход, «Защита» и недельные проверки ждут зарядки, но не
// дольше суток с прошлого раза (время прошлого раза - в weekly.ini, секция heavy, переживает перезапуск).
namespace {
SRWLOCK g_heavyLock = SRWLOCK_INIT;
std::map<std::wstring, LONGLONG> g_heavy;  // что -> когда было, секунды unix; читается из файла один раз

LONGLONG UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (LONGLONG)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ull - 11644473600ull);
}

LONGLONG HeavyLast(const std::wstring& what) {
    AcquireSRWLockExclusive(&g_heavyLock);
    auto it = g_heavy.find(what);
    if (it == g_heavy.end()) {
        wchar_t v[32] = {};
        GetPrivateProfileStringW(L"heavy", what.c_str(), L"0", v, 32, (tools::DataDir() + L"\\weekly.ini").c_str());
        it = g_heavy.emplace(what, _wtoi64(v)).first;
    }
    LONGLONG t = it->second;
    ReleaseSRWLockExclusive(&g_heavyLock);
    return t;
}
}  // namespace

bool OnBattery() {
    static const bool fake = getenv("RADIAL_ON_BATTERY") != nullptr;  // проверка без выдёргивания зарядки
    if (fake) return true;
    SYSTEM_POWER_STATUS ps;
    return GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0;
}

bool HeavyWait(const std::wstring& what) { return tools::HeavyDefer(OnBattery(), UnixNow(), HeavyLast(what)); }

void HeavyDone(const std::wstring& what) {
    LONGLONG now = UnixNow();
    AcquireSRWLockExclusive(&g_heavyLock);
    g_heavy[what] = now;
    WritePrivateProfileStringW(L"heavy", what.c_str(), std::to_wstring(now).c_str(),
                               (tools::DataDir() + L"\\weekly.ini").c_str());
    ReleaseSRWLockExclusive(&g_heavyLock);
}

void ScanPass(const std::vector<std::wstring>& roots, bool proj, bool sys) {
    Scan s;
    unsigned gen = g_junkGen;
    AcquireSRWLockShared(&g_scanLock);
    Scan prev = g_scan;  // что в этот раз не пересчитываем - берём из прошлого
    ReleaseSRWLockShared(&g_scanLock);
    // кэши (TEMP, npm, pip, Unreal) - миллионы файлов и десятки секунд процессора: вместе с проектами, раз в 3 часа
    // (и по кнопке); между ними - прошлый ответ, а после чистки его поправляет JunkCleaned
    if (proj) {
        s.junk = tools::FindJunk(true, &s.ddcOld, kDdcDays);
    } else {
        s.junk = prev.junk;
        s.ddcOld = prev.ddcOld;
    }
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
    s.growth = proj || prev.growth.empty() ? tools::WeekGrowth(7) : prev.growth;  // пусто - обход был с диска
    CoInitializeEx(nullptr,
                   COINIT_MULTITHREADED);  // история обновлений - через COM
    s.git = proj || prev.git.empty() ? tools::GitRepos(roots) : prev.git;
    {
        PERF_CPU(sys ? "cpu.scan.sys" : "cpu.scan.light");  // суточная часть отдельно: её старт разнесён (Н9)
        s.vhd = sys ? tools::VirtualDisks() : prev.vhd;
        s.leftovers = sys ? tools::AppLeftovers() : prev.leftovers;
        s.devices = sys ? tools::DeviceProblems() : prev.devices;
        s.updates = sys ? tools::WindowsUpdates() : prev.updates;
        s.week = sys ? tools::WeekRecordAndCompare() : prev.week;
    }
    CoUninitialize();
    if (bg::Quitting()) return;
    AcquireSRWLockExclusive(&g_scanLock);
    if (gen != g_junkGen) s.junk = g_scan.junk;  // пока считали, прошла чистка: её размеры свежее
    s.driver = g_scan.driver;                    // драйвер и версии проверяются отдельно
    s.versions = g_scan.versions;
    g_scan = std::move(s);
    ++g_scanGen;
    g_scanAt = GetTickCount64();
    if (proj) g_projAt = g_scanAt.load(), g_projExtra = 0;
    if (sys) g_sysAt = g_scanAt.load();
    tools::ScanCache cache;
    if (proj) cache = {UnixNow(), g_scan.junk, g_scan.projects, g_scan.ddcOld};
    ReleaseSRWLockExclusive(&g_scanLock);
    if (proj) tools::SaveScanCache(cache);  // перезапуск в ближайшие 3 часа обойдётся без нового обхода
}

// Старт (О5): обход кэшей и проектов моложе 3 часов - с диска, следующий - в свой срок от того обхода.
void LoadScanFromDisk() {
    tools::ScanCache c;
    if (!tools::LoadScanCache(c)) return;
    LONGLONG age = UnixNow() - c.when;
    if (age < 0 || (ULONGLONG)age * 1000 >= kProjEvery) return;
    ULONGLONG ageMs = (ULONGLONG)age * 1000, now = GetTickCount64();
    AcquireSRWLockExclusive(&g_scanLock);
    g_scan.junk = std::move(c.junk);
    g_scan.projects = std::move(c.projects);
    g_scan.ddcOld = c.ddcOld;
    ++g_scanGen;
    ++g_junkGen;
    ReleaseSRWLockExclusive(&g_scanLock);
    g_projAt = now > ageMs ? now - ageMs : 1;
    g_projExtra = now > ageMs ? 0 : ageMs - now;
}

void RefreshScan(bool full) {
    if (!g_cfg || g_scanRunning.exchange(true)) return;  // студия в режиме снимка: утилиты не запущены
    ULONGLONG now = GetTickCount64();
    bool proj = full || (ProjDue(now) && !HeavyWait(L"scan"));
    bool sys = full || ((!g_sysAt || now - g_sysAt > kSysEvery) && now - g_started > kSysDelay && !HeavyWait(L"sys"));
    std::vector<std::wstring> roots = g_roots.empty() ? ProjectRoots(*g_cfg) : g_roots;
    bool started = bg::Run(
        [roots, proj, sys] {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // и диск - в последнюю очередь
            {
                PERF_CPU("cpu.scan");
                ScanPass(roots, proj, sys);
            }
            if (proj && !bg::Quitting()) HeavyDone(L"scan");
            if (sys && !bg::Quitting()) HeavyDone(L"sys");
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
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // чтение файлов для подписей - тоже
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            tools::SecurityScan s;
            {
                PERF_CPU("cpu.security");
                s = tools::ScanSecurity();
            }
            CoUninitialize();
            AcquireSRWLockExclusive(&g_secLock);
            g_sec = std::move(s);
            g_secAt = GetTickCount64();
            ReleaseSRWLockExclusive(&g_secLock);
            HeavyDone(L"sec");
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
    LoadScanFromDisk();
    // остановили по «Стоп», а задание доработало в фоне - итог карточкой (из фонового потока - в поток окна)
    net::SetLeftDone([](net::Job j, bool ok, const std::wstring& last) {
        auto* m = new LeftMsg{j, ok, last};
        if (!PostMessageW(g_app, WM_TOOLS_JOB, kLeftTag, (LPARAM)m)) delete m;
    });
    g_watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_watchThread = CreateThread(nullptr, 0, WatchLoop, nullptr, 0, nullptr);
    GuardStart();
    DevrunStart();
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
    if (wp == kDrainTag) {  // Л38: журнал всегда, карточка - если не занята и не игра
        std::unique_ptr<DrainMsg> m((DrainMsg*)lp);
        autos::Note(m->out.title + L". " + m->out.text);
        if (!g_busy && !g_game && !autos::Digest(m->out.title, m->out.text))
            g_card.Show(BoostCard::DONE, m->out.title, m->out.text, Theme(), 15000, Toast::G_INFO);
        return;
    }
    if (wp == kBuildTag) {
        std::unique_ptr<BuildMsg> m((BuildMsg*)lp);
        BuildFound(*m);
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
    WeekNews();
    // фоновый подсчёт: первый раз через 90 с после старта, дальше раз в полчаса (кэши и проекты - раз в 3 часа,
    // суточная часть - своим сроком); в игре и при открытом Unreal (шейдеры, сборка) обход диска ждёт - он отнимал
    // бы и процессор, и диск. От батареи тяжёлые части ждут зарядки (HeavyWait), лёгкие идут как обычно
    if (!g_game && !g_dev && now - g_started > kScanDelay && !g_scanRunning) {
        bool due = !g_scanAt || now - g_scanAt > 30 * 60 * 1000;
        bool projDue = ProjDue(now) && !HeavyWait(L"scan");
        bool sysDue = (!g_sysAt || now - g_sysAt > kSysEvery) && now - g_started > kSysDelay && !HeavyWait(L"sys");
        if (due || projDue || sysDue) RefreshScan();
    }
    // «Защита»: первый раз через 4 минуты после старта (подписи считаются долго), дальше раз в полчаса; не в игре
    if (!g_game && now - g_started > kSecDelay && (!g_secAt || now - g_secAt > 30 * 60 * 1000) && !HeavyWait(L"sec"))
        RefreshSecurity();
    // недельные проверки: через 10 минут после старта, потом раз в 6 часов смотрим, не пора ли; от батареи - ждут
    if (now - g_weeklyCheck > (g_weeklyCheck == g_started ? kWeeklyDelay : 6 * 3600 * 1000) && !HeavyWait(L"weekly")) {
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
    DevrunStop();
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
        s.week = g_scan.week;
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
    tools::DropScanCache();
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

void SnapRing(RadialMenu& menu, const Config* cfg, UINT dpi, int root, const std::wstring& path, bool demo) {
    if (demo) {  // эталон: выдуманные пункты
        menu.SnapNet(cfg, DemoRingItems(), dpi, root, 1500, path);
        return;
    }
    PrimeForSnapshot(cfg);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    PrimeSecurityForSnapshot();
    std::wstring status;
    menu.SnapNet(cfg, RingItems(status), dpi, root, 1500, path);
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
    v.exts = g_sec.extList;
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

}  // namespace toolsui
