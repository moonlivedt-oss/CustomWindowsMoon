#include "app/toolsui.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

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

}  // namespace

void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below,
          std::function<void(int, const std::wstring&)> openStudio) {
    g_app = app;
    g_cfg = cfg;
    g_openStudio = std::move(openStudio);
    g_card.below = below;
    g_card.Create(inst);
    g_diskCheck = GetTickCount64() - 9 * 60 * 1000;  // первая проверка места - через минуту после старта
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
    // очистка
    g_junk = tools::FindJunk(false);
    Item clean = Act(L"Очистка", L"tools-clean.png", L"", L"Временные файлы и кэши; то, что занято, пропускается");
    for (size_t i = 0; i < g_junk.size(); ++i)
        clean.children.push_back(
            Act(g_junk[i].name, L"tools-clean.png", L"clean:" + std::to_wstring(i),
                L"Удалить содержимое " + g_junk[i].path + L"; программа скачает или создаст заново"));
    ring.push_back(clean);
    // PATH
    int dead = 0, dup = 0;
    for (const tools::PathEntry& e : tools::ReadPath()) {
        if (!e.system && e.dead) ++dead;
        if (!e.system && e.dup) ++dup;
    }
    Item path = Act(L"PATH", L"tools-path.png", L"", L"Мёртвые папки, дубли, что найдётся первым");
    path.children = {
        Act(L"Почистить", L"tools-clean.png", L"path-clean",
            L"PATH пользователя: мёртвых " + std::to_wstring(dead) + L", дублей " + std::to_wstring(dup) +
                L" (с бэкапом)"),
        Act(L"Что где", L"tools-env.png", L"which", L"Какой python, g++, node, cmake... найдётся первым"),
        Act(L"Добавить", L"tools-path.png", L"path-add", L"Добавить в PATH папку из буфера (скопируй её в Проводнике)")};
    ring.push_back(path);
    // порты
    Item ports = Act(L"Порты", L"tools-ports.png", L"", L"Кто занял порты разработки; 2 клика - завершить");
    int nports = 0;
    for (const tools::Listener& l : tools::Listeners())
        if (l.dev && nports++ < 12) {
            std::wstring proc = EndsWithI(l.process, L".exe") ? l.process.substr(0, l.process.size() - 4) : l.process;
            ports.children.push_back(Act(std::to_wstring(l.port) + L" " + Short(proc, 14), L"tools-ports.png",
                                         L"kill:" + std::to_wstring(l.pid),
                                         std::wstring(l.udp ? L"UDP " : L"TCP ") + std::to_wstring(l.port) + L", " +
                                             l.process + L" (PID " + std::to_wstring(l.pid) + L"). Завершить процесс"));
        }
    if (ports.children.empty())
        ports.children.push_back(Act(L"Свободно", L"tools-ports.png", L"studio:3", L"Порты разработки никто не занял"));
    ring.push_back(ports);
    ring.push_back(Act(L"Кто держит", L"tools-lock.png", L"lock-clip",
                       L"Кто не даёт удалить файл или папку: скопируй её в Проводнике и выбери это"));
    ring.push_back(Act(L"Все утилиты", L"tools-box.png", L"studio:0", L"PATH, диски, автозагрузка и порты в студии"));

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
    if (key.rfind(L"clean:", 0) == 0) {
        size_t i = (size_t)_wtoi(key.c_str() + 6);
        if (i >= g_junk.size()) return;
        tools::Junk j = g_junk[i];
        return RunJob(L"Очистка: " + j.name, [j](const tools::Log& log, std::wstring& res) {
            ULONGLONG freed = tools::CleanJunk(j, log);
            res = L"Освобождено " + tools::Bytes(freed) + L".";
            return true;
        });
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
        g_card.yesLabel = L"Завершить";
        g_card.noLabel = L"Не надо";
        g_card.onYes = [who] {
            g_card.yesLabel.clear();
            g_card.noLabel.clear();
            int n = 0;
            for (const tools::Locker& l : who) n += tools::KillPid(l.pid) ? 1 : 0;
            g_card.Show(BoostCard::DONE, L"Завершено: " + std::to_wstring(n) + L" из " + std::to_wstring(who.size()),
                        n == (int)who.size() ? L"Файл свободен." : L"Кого-то завершить не удалось (нужны права).",
                        Theme());
        };
        g_card.Show(BoostCard::OFFER, L"«" + Short(name, 28) + L"» держат", list + L".", Theme(), 20000);
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
        g_card.yesLabel = L"Что занимает";
        g_card.noLabel = L"Позже";
        g_card.onYes = [root] {
            g_card.yesLabel.clear();
            g_card.noLabel.clear();
            if (g_openStudio) g_openStudio(1, root);
        };
        g_card.Show(BoostCard::OFFER, L"Мало места на " + v.root.substr(0, 2),
                    L"Свободно " + tools::Bytes(v.free) + L" из " + tools::Bytes(v.total) +
                        L". Кэши npm и pip, временные файлы - в кольце «Утилиты», раздел «Очистка».",
                    Theme(), 30000);
        break;
    }
}

void Shutdown() { g_card.Destroy(); }

}  // namespace toolsui
