// Кольцо «Утилиты»: из последнего подсчёта и слежки - пункты дисков, очистки, процессов, «Защиты», системы.
#include "app/toolsui_internal.h"

namespace toolsui {

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
std::wstring FtText(ULONGLONG ft, bool withTime) {
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

void TurnOffStart(const tools::StartItem& it) {
    // выключаем ровно показанную запись: пока висела карточка, её могли поменять в студии или другой программой
    std::vector<tools::StartItem> now = tools::StartupItems();
    const tools::StartItem* cur = tools::FindStart(now, it);
    if (!cur) {
        g_card.Show(BoostCard::DONE, it.name + L": запись изменилась",
                    L"Её уже убрали или поменяли команду. Открой список заново и проверь, что выключать.", Theme(),
                    9000, Toast::G_INFO);
        return;
    }
    std::wstring msg;
    bool ok = tools::SetStartup(*cur, false, [&](const std::wstring& t) { msg = t; });
    g_card.Show(BoostCard::DONE, ok ? L"Автозагрузка" : L"Не получилось",
                msg + (ok ? L" Включить обратно - в студии или Диспетчере задач." : L""), Theme());
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
    if (s.isStart) {
        tools::StartItem it = s.startItem;
        std::wstring path = s.path;
        Ask(
            Level(s.score) + it.name + L" при входе", it.where + L": " + Short(it.command, 120) + L". " + s.why,
            file ? L"VirusTotal" : L"Понятно", it.canToggle ? L"Выключить" : L"Закрыть",
            [path, file] {
                if (file) OpenVirusTotal(path);
            },
            [it] {
                if (it.canToggle) TurnOffStart(it);
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

std::vector<Item> RingItems(std::wstring& status) {
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
    status.clear();
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
    // прогноз места (И10): только если место заметно убывает
    for (const tools::Forecast& f : tools::SpaceForecast()) {
        if (f.days < 0 || f.days > 180) continue;
        std::wstring fast;
        for (const tools::Growth& g : scan.growth)
            if (g.path.size() >= 2 && _wcsnicmp(g.path.c_str(), f.root.c_str(), 2) == 0 && g.delta > 0) {
                fast = L" Быстрее всего растёт " + g.name + L": +" + tools::Bytes((ULONGLONG)g.delta) + L" за неделю.";
                break;
            }
        disks.children.push_back(Act(f.root.substr(0, 2) + L" на " + std::to_wstring(f.days) + L" дн.",
                                     Have(L"tools-forecast.png", L"tools-disk.png"), L"scan:" + f.root,
                                     tools::ForecastText(f) + L"." + fast + L" 2 клика - что занимает место"));
    }
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
                                     L"Кэши и временные файлы (свежее и занятое не трогаю), кроме кэшей шейдеров; "
                                     L"занятое пропускается"));
    if (scan.ddcOld > 256ull * 1024 * 1024)
        clean.children.push_back(Act(L"Unreal: старое " + tools::Bytes(scan.ddcOld),
                                     Have(L"tools-archive.png", L"tools-clean.png"), L"ddc-trim",
                                     L"Из кэша Unreal - то, что не открывалось " + Days(kDdcDays) +
                                         L". Шейдеры текущих проектов остаются, пересборки не будет"));
    for (size_t i = 0; i < g_junk.size(); ++i)
        if (!sized || g_junk[i].bytes)  // только свежее (TEMP) - удалять пока нечего
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
    clean.children.push_back(Act(L"Остатки обновлений", Have(L"tools-update.png", L"tools-clean.png"), L"sys-clean",
                                 L"Кэш Delivery Optimization и старые версии компонентов Windows (DISM). С правами "
                                 L"администратора, спрошу"));
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
    // большие файлы томов (И9) - из списка, что кладёт задача с правами при подсчёте по MFT
    std::vector<tools::BigFile> big = tools::BigFiles();
    if (!big.empty())
        groups.back().children.insert(groups.back().children.end() - 1,
                                      Act(L"Большие файлы", Have(L"tools-bigfiles.png", L"tools-disk.png"), L"studio:1",
                                          L"Самый большой: " + Leaf(big[0].path) + L", " + tools::Bytes(big[0].bytes) +
                                              L". Щелчок - список в студии с «Показать» и «В Корзину»"));
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
    // системные гиганты (И11): что занимает Windows сама и что с этим честно можно сделать
    std::vector<tools::Giant> giants = Giants();
    if (!giants.empty()) {
        ULONGLONG sum = 0;
        for (const tools::Giant& g : giants) sum += g.bytes;
        Item gi = Act(L"Гиганты", Have(L"tools-giants.png", L"tools-wmi.png"), L"",
                      L"Гибернация, подкачка, прошлая Windows, точки восстановления: " + tools::Bytes(sum) +
                          L". Щелчок - открыть");
        for (const tools::Giant& g : giants)
            gi.children.push_back(Act(g.name + (g.bytes ? L" " + tools::Bytes(g.bytes) : L""),
                                      Have(L"tools-giants.png", L"tools-wmi.png"), L"giant:" + g.key, g.why));
        gi.children.push_back(
            Act(L"Точно, с правами", Have(L"tools-giants.png", L"tools-wmi.png"), L"giants-measure",
                L"Размер точек восстановления и закрытых папок прошлой Windows видно только с правами"));
        groups.back().children.push_back(std::move(gi));
    }
    ring = std::move(groups);
    if (sized && quickTotal + scan.ddcOld + bin > 0)
        status += L"   |   можно освободить " + tools::Bytes(quickTotal + scan.ddcOld + bin);
    if (projTotal) status += L"   |   мусор проектов " + tools::Bytes(projTotal);
    status += L"   |   память свободна " + tools::Bytes(mem.avail);
    return ring;
}

void ShowRing(RadialMenu& menu) {
    std::wstring status;
    std::vector<Item> ring = RingItems(status);
    menu.onNet = [](const std::wstring& key) { Run(key); };
    menu.netStatus = [status] { return status; };
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForWindow(g_app);
    menu.ShowNet(g_cfg, std::move(ring), pt, mi.rcWork, dpi ? dpi : 96);
}

}  // namespace toolsui
