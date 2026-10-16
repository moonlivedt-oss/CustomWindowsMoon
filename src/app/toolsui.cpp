#include "app/toolsui.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

#include <shellapi.h>

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
ULONGLONG g_diskCheck = 0;
std::set<std::wstring> g_warned;         // о каком диске уже сказали (пока место не вернётся)
std::vector<tools::StartItem> g_starts;  // автозагрузка на момент открытия кольца
std::vector<tools::Junk> g_junk;         // кэши на момент открытия кольца
std::vector<tools::HeavyApp> g_apps;     // тяжёлые программы на момент открытия кольца
std::vector<tools::Listener> g_ports;    // порты разработки на момент открытия кольца
std::vector<tools::Which> g_which;       // что найдётся первым на момент открытия кольца
// размеры кэшей считаются в фоне (обход тысяч файлов): кольцо показывает последнее известное
SRWLOCK g_junkLock = SRWLOCK_INIT;
std::vector<tools::Junk> g_junkSized;
ULONGLONG g_junkAt = 0, g_started = 0;
volatile bool g_junkRunning = false;

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

// короткое имя кэша для сектора: «Кэш npm» -> «npm»
std::wstring JunkShort(const std::wstring& n) {
    if (n == L"Временные файлы") return L"Временные";
    if (n == L"Аварийные дампы") return L"Дампы";
    if (n == L"Отчёты об ошибках") return L"Отчёты";
    if (n == L"Архивы Cargo") return L"Cargo";
    if (n == L"Кэш шейдеров DirectX") return L"DirectX";
    if (n == L"Кэш шейдеров NVIDIA") return L"NVIDIA";
    return n.rfind(L"Кэш ", 0) == 0 ? n.substr(4) : n;
}

void RefreshJunk() {
    if (g_junkRunning) return;
    g_junkRunning = true;
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void*) -> DWORD {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);  // фоновый обход диска не мешает работе
            std::vector<tools::Junk> j = tools::FindJunk(true);
            AcquireSRWLockExclusive(&g_junkLock);
            g_junkSized = std::move(j);
            g_junkAt = GetTickCount64();
            ReleaseSRWLockExclusive(&g_junkLock);
            g_junkRunning = false;
            return 0;
        },
        nullptr, 0, nullptr);
    if (th)
        CloseHandle(th);
    else
        g_junkRunning = false;
}

// Карточка-вопрос с двумя действиями; подписи кнопок после ответа возвращаются к обычным.
void Ask(const std::wstring& title, const std::wstring& sub, const std::wstring& yes, const std::wstring& no,
         std::function<void()> onYes, std::function<void()> onNo = nullptr, int hold = 20000) {
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
    g_card.Show(BoostCard::OFFER, title, sub, Theme(), hold);
}

}  // namespace

void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below,
          std::function<void(int, const std::wstring&)> openStudio) {
    g_app = app;
    g_cfg = cfg;
    g_openStudio = std::move(openStudio);
    g_card.below = below;
    g_card.Create(inst);
    g_diskCheck = GetTickCount64() - 9 * 60 * 1000;  // первая проверка места - через минуту после старта
    g_started = GetTickCount64();
}

void ShowRing(RadialMenu& menu) {
    std::vector<Item> ring;
    // диски
    std::vector<tools::Volume> vols = tools::Volumes();
    Item disks = Act(L"Диски", L"tools-disk.png", L"", L"Место на дисках; 2 клика по диску - что его занимает");
    std::wstring status;
    for (const tools::Volume& v : vols) {
        std::wstring letter = v.root.substr(0, 2);
        int pct = v.total ? (int)(v.free * 100 / v.total) : 0;
        disks.children.push_back(
            Act(letter + L" " + tools::Bytes(v.free), L"tools-size.png", L"scan:" + v.root,
                L"Свободно " + tools::Bytes(v.free) + L" из " + tools::Bytes(v.total) + L" (" + std::to_wstring(pct) +
                    L"%)" + (v.label.empty() ? L"" : L", «" + v.label + L"»") + L". 2 клика - что занимает место"));
        status += (status.empty() ? L"" : L"   |   ") + letter + L" свободно " + tools::Bytes(v.free);
    }
    disks.children.push_back(
        Act(L"Здоровье", L"tools-disk.png", L"health", L"Износ, температура и часы работы SSD; нужны права"));
    ring.push_back(disks);
    // очистка: размеры из фонового подсчёта; если он ещё не прошёл - список без размеров
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_junkLock);
    bool sized = g_junkAt != 0;
    g_junk = sized ? g_junkSized : tools::FindJunk(false);
    ReleaseSRWLockShared(&g_junkLock);
    if (!sized || now - g_junkAt > 5 * 60 * 1000) RefreshJunk();
    ULONGLONG junkTotal = 0, quickTotal = 0;  // всё и то, что пересоздаётся быстро (без шейдеров)
    int quick = 0;
    for (const tools::Junk& j : g_junk) {
        junkTotal += j.bytes;
        if (!j.slow) quickTotal += j.bytes, ++quick;
    }
    ULONGLONG binItems = 0, bin = tools::RecycleBinBytes(&binItems);
    Item clean = Act(L"Очистка", L"tools-clean.png", L"",
                     sized ? L"Можно освободить " + tools::Bytes(junkTotal + bin) + L": кэши, временные файлы, корзина"
                           : L"Временные файлы, кэши и корзина; размеры ещё считаются");
    if (sized && quick > 1)
        clean.children.push_back(Act(L"Всё сразу " + tools::Bytes(quickTotal), L"tools-clean.png", L"clean-all",
                                     L"Кэши и временные файлы (свежее суток не трогаю), кроме кэшей шейдеров; "
                                     L"занятое пропускается"));
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
    // PATH: проблемы и что найдётся первым - прямо секторами
    int dead = 0, dup = 0;
    for (const tools::PathEntry& e : tools::ReadPath()) {
        if (!e.system && e.dead) ++dead;
        if (!e.system && e.dup) ++dup;
    }
    Item path =
        Act(L"PATH", L"tools-path.png", L"",
            dead + dup ? L"Лишних записей: " + std::to_wstring(dead + dup) + L"; ниже - какая команда найдётся первой"
                       : L"Чисто; ниже - какая команда найдётся первой");
    if (dead + dup)
        path.children.push_back(Act(
            L"Почистить " + std::to_wstring(dead + dup), L"tools-clean.png", L"path-clean",
            L"Мёртвых " + std::to_wstring(dead) + L", дублей " + std::to_wstring(dup) + L"; старый PATH - в бэкап"));
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
    // порты: 2 клика - карточка «Открыть в браузере» или «Завершить»
    g_ports.clear();
    Item ports = Act(L"Порты", L"tools-ports.png", L"", L"Кто занял порты разработки");
    for (const tools::Listener& l : tools::Listeners())
        if (l.dev && g_ports.size() < 12) {
            ports.children.push_back(Act(std::to_wstring(l.port) + L" " + Short(NoExe(l.process), 14),
                                         L"tools-ports.png", L"port:" + std::to_wstring(g_ports.size()),
                                         std::wstring(l.udp ? L"UDP " : L"TCP ") + std::to_wstring(l.port) + L", " +
                                             l.process + L" (PID " + std::to_wstring(l.pid) +
                                             L"). 2 клика - открыть или завершить"));
            g_ports.push_back(l);
        }
    if (ports.children.empty())
        ports.children.push_back(Act(L"Свободно", L"tools-ports.png", L"studio:3", L"Порты разработки никто не занял"));
    ring.push_back(ports);
    // процессы: самые тяжёлые программы по памяти
    g_apps = tools::HeavyApps(8);
    ULONGLONG heavy = 0;
    for (const tools::HeavyApp& a : g_apps) heavy += a.bytes;
    Item procs =
        Act(L"Процессы", L"tools-box.png", L"", L"Самые тяжёлые программы по памяти: вместе " + tools::Bytes(heavy));
    for (size_t i = 0; i < g_apps.size(); ++i)
        procs.children.push_back(Act(Short(NoExe(g_apps[i].name), 14) + L" " + tools::Bytes(g_apps[i].bytes),
                                     L"tools-box.png", L"app:" + std::to_wstring(i),
                                     Plural((int)g_apps[i].pids.size(), L"процесс", L"процесса", L"процессов") +
                                         L", память " + tools::Bytes(g_apps[i].bytes) +
                                         L". 2 клика - завершить (спрошу)"));
    ring.push_back(procs);
    ring.push_back(Act(L"Кто держит", L"tools-lock.png", L"lock-clip",
                       L"Кто не даёт удалить файл или папку: скопируй её в Проводнике и выбери это"));
    ring.push_back(Act(L"Все утилиты", L"tools-box.png", L"studio:0", L"PATH, диски, автозагрузка и порты в студии"));
    if (sized && junkTotal + bin > 0) status += L"   |   можно освободить " + tools::Bytes(junkTotal + bin);
    if (dead + dup) status += L"   |   PATH: лишних " + std::to_wstring(dead + dup);

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
    if (key == L"clean-all") {
        std::vector<tools::Junk> all = g_junk;
        return RunJob(L"Очистка", [all](const tools::Log& log, std::wstring& res) {
            ULONGLONG freed = 0;
            for (const tools::Junk& j : all)
                if (!j.slow) freed += tools::CleanJunk(j, log);
            res = L"Освобождено " + tools::Bytes(freed) + L".";
            g_junkAt = 0;  // пересчитать размеры
            return true;
        });
    }
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
    if (key.rfind(L"which:", 0) == 0) {
        size_t i = (size_t)_wtoi(key.c_str() + 6);
        if (i >= g_which.size()) return;
        std::wstring p = g_which[i].path;
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    if (key.rfind(L"port:", 0) == 0) {
        size_t i = (size_t)_wtoi(key.c_str() + 5);
        if (i >= g_ports.size()) return;
        tools::Listener l = g_ports[i];
        Ask(
            L"Порт " + std::to_wstring(l.port) + L": " + NoExe(l.process),
            L"PID " + std::to_wstring(l.pid) + L". Открыть http://localhost:" + std::to_wstring(l.port) +
                L" в браузере или завершить процесс?",
            L"Открыть", L"Завершить",
            [l] {
                std::wstring url = L"http://localhost:" + std::to_wstring(l.port);
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            },
            [l] {
                bool ok = tools::KillPid(l.pid, l.process);
                g_card.Show(BoostCard::DONE, ok ? L"Порт свободен" : L"Не получилось",
                            ok ? NoExe(l.process) + L" завершён."
                               : L"Процесс не даёт себя завершить (возможно, запущен с правами).",
                            Theme());
            });
        return;
    }
    if (key.rfind(L"app:", 0) == 0) {
        size_t i = (size_t)_wtoi(key.c_str() + 4);
        if (i >= g_apps.size()) return;
        tools::HeavyApp a = g_apps[i];
        Ask(L"Завершить " + NoExe(a.name) + L"?",
            Plural((int)a.pids.size(), L"процесс", L"процесса", L"процессов") + L", " + tools::Bytes(a.bytes) +
                L". Сначала попрошу закрыться (окна сохранят своё), через 5 с - силой.",
            L"Закрыть", L"Не надо", [a] {
                RunJob(L"Закрываю " + NoExe(a.name), [a](const tools::Log& log, std::wstring& res) {
                    tools::AppClose c = tools::CloseApp(a, 5000, log);
                    if (c.closed + c.killed == 0) {
                        res = L"Программа не даёт себя завершить (возможно, запущена с правами).";
                        return false;
                    }
                    res = NoExe(a.name) + L": закрылись сами " + std::to_wstring(c.closed) + L", силой " +
                          std::to_wstring(c.killed) + (c.failed ? L", не далось " + std::to_wstring(c.failed) : L"") +
                          L".";
                    return c.failed == 0;
                });
            });
        return;
    }
    if (key.rfind(L"clean:", 0) == 0) {
        size_t i = (size_t)_wtoi(key.c_str() + 6);
        if (i >= g_junk.size()) return;
        tools::Junk j = g_junk[i];
        auto go = [j] {
            RunJob(L"Очистка: " + j.name, [j](const tools::Log& log, std::wstring& res) {
                ULONGLONG freed = tools::CleanJunk(j, log);
                res = L"Освобождено " + tools::Bytes(freed) + L".";
                g_junkAt = 0;
                return true;
            });
        };
        if (!j.slow) return go();
        Ask(j.name + L": очистить?",
            tools::Bytes(j.bytes) + L". Шейдеры соберутся заново: первые запуски игр и редактора будут медленнее.",
            L"Очистить", L"Не надо", go);
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
    if (key == L"which")
        return RunJob(L"Что где", [](const tools::Log&, std::wstring& res) {
            for (const tools::Which& w : tools::WhichAll()) {
                if (w.cmd != L"python" && w.cmd != L"g++" && w.cmd != L"node" && w.cmd != L"cmake" && w.cmd != L"git" &&
                    w.cmd != L"pip")
                    continue;
                std::wstring dir = w.path.substr(0, w.path.find_last_of(L'\\'));
                res += (res.empty() ? L"" : L"; ") + w.cmd + L" - " +
                       Short(dir.substr(dir.find_last_of(L'\\') + 1), 18) +
                       (w.other.empty() ? L"" : L" (+" + std::to_wstring(w.other.size()) + L")");
            }
            return true;
        });
    if (key == L"path-add") {
        std::wstring p = tools::ClipboardPath();
        if (p.empty() || !(GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
            g_card.Show(BoostCard::DONE, L"В буфере нет папки", L"Скопируй папку в Проводнике (Ctrl+C) и выбери снова.",
                        Theme());
            return;
        }
        return RunJob(L"PATH", [p](const tools::Log& log, std::wstring&) { return tools::AddUserPath(p, log); });
    }
    if (key.rfind(L"start-off:", 0) == 0) {
        size_t i = (size_t)_wtoi(key.c_str() + 10);
        if (i >= g_starts.size()) return;
        tools::StartItem s = g_starts[i];
        std::wstring msg;
        bool ok = tools::SetStartup(s, false, [&](const std::wstring& t) { msg = t; });
        g_card.Show(BoostCard::DONE, ok ? L"Автозагрузка" : L"Не получилось", msg, Theme());
        return;
    }
    if (key.rfind(L"kill:", 0) == 0) {
        DWORD pid = (DWORD)_wtoi(key.c_str() + 5);
        bool ok = tools::KillPid(pid);
        g_card.Show(BoostCard::DONE, ok ? L"Процесс завершён" : L"Не получилось",
                    ok ? L"PID " + std::to_wstring(pid) + L", порт свободен."
                       : L"Процесс не даёт себя завершить (возможно, запущен с правами).",
                    Theme());
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
        std::wstring name = p.substr(p.find_last_of(L'\\') + 1);
        if (who.empty()) {
            g_card.Show(BoostCard::DONE, L"Никто не держит", L"«" + name + L"» свободен - удаляется и переносится.",
                        Theme());
            return;
        }
        std::wstring list;
        for (size_t i = 0; i < who.size() && i < 4; ++i) list += (i ? L", " : L"") + who[i].name;
        if (who.size() > 4) list += L" и ещё " + std::to_wstring(who.size() - 4);
        Ask(L"«" + Short(name, 28) + L"» держат", list + L".", L"Завершить", L"Не надо", [who] {
            int n = 0;
            for (const tools::Locker& l : who) n += tools::KillPid(l.pid) ? 1 : 0;
            g_card.Show(BoostCard::DONE, L"Завершено: " + std::to_wstring(n) + L" из " + std::to_wstring(who.size()),
                        n == (int)who.size() ? L"Файл свободен." : L"Кого-то завершить не удалось (нужны права).",
                        Theme());
        });
        return;
    }
}

void OnJob(LPARAM lp) {
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
    // размеры кэшей: первый раз через минуту после старта, дальше раз в полчаса
    if (now - g_started > 60000 && (!g_junkAt || now - g_junkAt > 30 * 60 * 1000)) RefreshJunk();
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
                L". Кэши npm и pip, временные файлы - в кольце «Утилиты», раздел «Очистка».",
            L"Что занимает", L"Позже",
            [root] {
                if (g_openStudio) g_openStudio(1, root);
            },
            nullptr, 30000);
        break;
    }
}

void Shutdown() { g_card.Destroy(); }

}  // namespace toolsui
