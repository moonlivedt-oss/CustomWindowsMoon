// Что делает пункт кольца «Утилиты»: Run(ключ сектора).
#include "app/toolsui_internal.h"

namespace toolsui {

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
            if (!j.slow && j.bytes) quick.push_back(j);
        return RunClean(L"Очистка", quick);
    }
    if (key == L"ddc-trim")
        return RunJob(L"Кэш Unreal", [](const tools::Log& log, std::wstring& res) {
            ULONGLONG freed = tools::TrimDdc(kDdcDays, log);
            res = L"Освобождено " + tools::Bytes(freed) + L". Нужное недавно осталось на месте.";
            g_scanAt = 0;
            return true;
        });
    if (key == L"sys-clean") return SysClean();
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
            L"Выключить", L"Не надо", [s] { TurnOffStart(s); });
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
    if (key.rfind(L"giant:", 0) == 0) return GiantAct(key.substr(6));
    if (key == L"giants-measure") return GiantsMeasure();
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
                RunJob(
                    L"Сжатие дисков",
                    [disks, total](const tools::Log& log, std::wstring& res) {
                        log(L"Закрываю WSL и Docker (wsl --shutdown)...");
                        std::wstring out;
                        tools::RunCapture(L"\"" + Sys32(L"wsl.exe") + L"\" --shutdown", out, 60000);
                        log(L"Сжимаю диски (минуты, зависит от размера)...");
                        bool ok = net::RunAdmin(net::JOB_VHD_COMPACT, log, 60 * 60 * 1000, &g_jobCancel);
                        if (g_jobCancel) return res = L"Остановлено, сжатие доделается в фоне.", false;
                        ULONGLONG after = 0;
                        for (const tools::VirtualDisk& v : tools::VirtualDisks()) after += v.bytes;
                        res = after < total ? L"Освобождено " + tools::Bytes(total - after) + L"."
                                            : L"Сжимать было нечего.";
                        g_scanAt = 0;
                        return ok;
                    },
                    false, true);
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
                RunJob(
                    L"Починка Windows",
                    [](const tools::Log& log, std::wstring&) {
                        return net::RunAdmin(net::JOB_REPAIR, log, 90 * 60 * 1000, &g_jobCancel);
                    },
                    false, true);
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

}  // namespace toolsui
