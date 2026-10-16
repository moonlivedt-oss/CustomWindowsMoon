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

// Тяжёлое (обход тысяч файлов, версии программ) считается в фоне раз в полчаса: кольцо открывается сразу
// и показывает последнее известное.
struct Scan {
    std::vector<tools::Junk> junk;
    std::vector<tools::ProjectJunk> projects;
    std::vector<tools::Conflict> conflicts;
    std::vector<tools::Growth> growth;
    ULONGLONG ddcOld = 0;  // кэш Unreal, не открывавшийся 30 дней
    tools::Driver driver;
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

void RunJob(const std::wstring& title, std::function<bool(const tools::Log&, std::wstring&)> work) {
    if (g_busy) {
        g_card.Show(BoostCard::DONE, L"Подождите", L"Предыдущее действие ещё идёт.", Theme());
        return;
    }
    g_busy = true;
    g_card.Show(BoostCard::WORKING, title, L"Начинаю...", Theme());
    struct Box {
        std::wstring title;
        std::function<bool(const tools::Log&, std::wstring&)> work;
    };
    auto* box = new Box{title, std::move(work)};
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<Box> b((Box*)p);
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::wstring last;
            tools::Log log = [&](const std::wstring& s) {
                last = s;
                auto* m = new JobMsg{b->title, s, false, false};
                if (!PostMessageW(g_app, WM_TOOLS_JOB, 0, (LPARAM)m)) delete m;
            };
            std::wstring result;
            bool ok = b->work(log, result);
            auto* m = new JobMsg{b->title, result.empty() ? last : result, true, ok};
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
    if (g_scanRunning) return;
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
            s.driver = g_scan.driver;  // драйвер проверяется раз в неделю отдельно
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
};
constexpr UINT kWeeklyTag = 0x57;  // wParam у WM_TOOLS_JOB: итог недельной проверки

void Weekly() {
    if (g_weeklyRunning) return;
    int today = Today();
    bool growth = today - LastDay(L"growth") >= 7, health = today - LastDay(L"health") >= 7,
         driver = today - LastDay(L"driver") >= 3;
    if (!growth && !health && !driver) return;
    g_weeklyRunning = true;
    struct Want {
        bool growth, health, driver;
    };
    auto* w = new Want{growth, health, driver};
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
}

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
    path.children.push_back(Act(L"Добавить", L"tools-path.png", L"path-add",
                                L"Добавить в PATH папку из буфера (скопируй её в Проводнике)"));
    ring.push_back(path);

    // порты: с проектом, который их занял
    g_ports.clear();
    std::vector<tools::Listener> ls = tools::Listeners();
    tools::ListenerProjects(ls);
    Item ports = Act(L"Порты", L"tools-ports.png", L"", L"Кто занял порты разработки и из какого проекта");
    for (const tools::Listener& l : ls)
        if (l.dev && g_ports.size() < 12) {
            std::wstring who = l.project.empty() ? NoExe(l.process) : Leaf(l.project);
            ports.children.push_back(
                Act(std::to_wstring(l.port) + L" " + Short(who, 14), L"tools-ports.png",
                    L"port:" + std::to_wstring(g_ports.size()),
                    std::wstring(l.udp ? L"UDP " : L"TCP ") + std::to_wstring(l.port) + L", " + l.process + L" (PID " +
                        std::to_wstring(l.pid) + L")" + (l.project.empty() ? L"" : L", проект " + l.project) +
                        (l.command.empty() ? L"" : L": " + l.command) + L". 2 клика - открыть или завершить"));
            g_ports.push_back(l);
        }
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
    ring.push_back(procs);

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
        std::vector<tools::Junk> all = g_junk;
        return RunJob(L"Очистка", [all](const tools::Log& log, std::wstring& res) {
            ULONGLONG freed = 0;
            for (const tools::Junk& j : all)
                if (!j.slow) freed += tools::CleanJunk(j, log);
            res = L"Освобождено " + tools::Bytes(freed) + L".";
            g_scanAt = 0;  // пересчитать размеры
            return true;
        });
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
        auto go = [j] {
            RunJob(L"Очистка: " + j.name, [j](const tools::Log& log, std::wstring& res) {
                ULONGLONG freed = tools::CleanJunk(j, log);
                res = L"Освобождено " + tools::Bytes(freed) + L".";
                g_scanAt = 0;
                return true;
            });
        };
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
    std::unique_ptr<JobMsg> m((JobMsg*)lp);
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
    ULONGLONG now = GetTickCount64();
    // фоновый подсчёт: первый раз через минуту после старта, дальше раз в полчаса
    if (now - g_started > 60000 && (!g_scanAt || now - g_scanAt > 30 * 60 * 1000)) RefreshScan();
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

void Shutdown() { g_card.Destroy(); }

}  // namespace toolsui
