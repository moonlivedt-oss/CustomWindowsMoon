// Раз в неделю: рост папок, здоровье SSD, драйвер, версии; «новое за неделю» в дайджест.
#include "system/ui/toolsui_internal.h"

#include <map>

#include "core/perf.h"
#include "shutup/catalog.h"

namespace toolsui {

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

// сборка Windows («26200.6899»): сменилась - обновление могло вернуть настройки приватности
std::wstring WindowsBuild() {
    wchar_t build[32] = L"";
    DWORD n = sizeof(build), ubr = 0, un = sizeof(ubr);
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    RegGetValueW(HKEY_LOCAL_MACHINE, key, L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &n);
    RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &un);
    return std::wstring(build) + L"." + std::to_wstring(ubr);
}

std::wstring SavedBuild() {
    wchar_t v[64] = L"";
    AcquireSRWLockShared(&g_dayLock);
    GetPrivateProfileStringW(L"weekly", L"privacy-build", L"", v, 64, (tools::DataDir() + L"\\weekly.ini").c_str());
    ReleaseSRWLockShared(&g_dayLock);
    return v;
}

void Weekly() {
    if (g_weeklyRunning) return;
    int today = Today();
    bool growth = today - LastDay(L"growth") >= 7, health = today - LastDay(L"health") >= 7,
         driver = today - LastDay(L"driver") >= 3, versions = today - LastDay(L"versions") >= 7;
    // приватность: раз в неделю и сразу после смены сборки Windows (крупные обновления возвращают настройки)
    std::wstring build = WindowsBuild();
    bool privacy = today - LastDay(L"privacy") >= 7 || build != SavedBuild();
    if (!growth && !health && !driver && !versions && !privacy) return;
    g_weeklyRunning = true;
    struct Want {
        bool growth, health, driver, versions, privacy;
        std::wstring build;
    };
    Want wanted{growth, health, driver, versions, privacy, build};
    bool started = bg::Run(
        [wanted] {
            const Want* want = &wanted;
            PERF_CPU("cpu.weekly");
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
                if (net::AdminReadyNow()) tools::DiskHealth(d, [](const std::wstring&) {});
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
            if (want->privacy) {
                shutup::Backup user = shutup::LoadBackup(HKEY_CURRENT_USER),
                               machine = shutup::LoadBackup(HKEY_LOCAL_MACHINE);
                for (const shutup::Setting& s : shutup::Catalog())
                    if (shutup::StatusOf(s, user, machine).Drifted()) m->privacyDrift.push_back(s.id);
                m->privacyChecked = true;
                SetDay(L"privacy", today);
                AcquireSRWLockExclusive(&g_dayLock);
                WritePrivateProfileStringW(L"weekly", L"privacy-build", want->build.c_str(),
                                           (tools::DataDir() + L"\\weekly.ini").c_str());
                ReleaseSRWLockExclusive(&g_dayLock);
            }
            CoUninitialize();
            HeavyDone(L"weekly");
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
    if (!m->privacyDrift.empty()) {  // своя карточка: про приватность молчать неделю нельзя
        std::vector<std::wstring> ids = m->privacyDrift;
        std::wstring names;
        int admin = 0;
        for (const std::wstring& id : ids)
            if (const shutup::Setting* s = shutup::Find(id)) {
                admin += s->Admin();
                if (names.size() < 120) names += (names.empty() ? L"" : L", ") + std::wstring(s->title);
            }
        g_askMore =
            L"Эти настройки приватности включали в RadialShutUp или во вкладке студии, а теперь они снова как в "
            L"Windows - "
            L"чаще всего их возвращает крупное обновление. «Включить снова» запишет те же значения ещё раз: свои - "
            L"сразу, "
            L"для компьютера - заданием с правами (если оно создано; иначе откроется RadialShutUp). Прежние значения "
            L"из "
            L"бэкапа не меняются - «как было» вернёт то, что было до первой правки.";
        Ask(
            L"Windows вернула " + Plural((int)ids.size(), L"настройку", L"настройки", L"настроек") + L" приватности",
            names + (ids.size() > 3 ? L" и др." : L"."), L"Включить снова", L"Открыть RadialShutUp",
            [ids, admin] {
                RunJob(L"Приватность", [ids, admin](const tools::Log& log, std::wstring& res) {
                    shutup::SetSource(L"RadialMenu");
                    int done = 0;
                    for (const std::wstring& id : ids)
                        if (const shutup::Setting* s = shutup::Find(id))
                            if (!s->Admin()) done += shutup::Apply(*s, true, log);
                    if (done) shutup::NotifyChanged(true);
                    bool adminOk = true;
                    if (admin && net::AdminReadyNow()) {
                        elevate::Put(L"PrivacyJob", L"reapply|*");
                        adminOk = net::RunAdmin(net::JOB_PRIVACY, log);
                        if (adminOk) done += admin;
                    } else if (admin) {
                        bool opened = tools::OwnExeShell(L"RadialShutUp.exe");
                        adminOk = false;
                        if (!opened) {
                            res = L"Снова включено: " + std::to_wstring(done) +
                                  L". RadialShutUp не открыт: файл не сверился (журнал).";
                            return false;
                        }
                    }
                    res = L"Снова включено: " + std::to_wstring(done) +
                          (adminOk ? L"." : L". Для компьютера нужны права - открыт RadialShutUp.");
                    return adminOk;
                });
            },
            [] { tools::OwnExeShell(L"RadialShutUp.exe"); }, 30000, Toast::G_WARN);
        return;
    }
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

// Что появилось за неделю (И36) - раз в неделю одной строкой: в вечерний дайджест, а без него - карточкой.
// Только новое: убранное и обновлённое - в списке кольца («Система»), о них не напоминаем.
void WeekNews() {
    static unsigned seen = ~0u;
    if (seen == g_scanGen) return;
    seen = g_scanGen;
    AcquireSRWLockShared(&g_scanLock);
    tools::WeekReport w = g_scan.week;
    ReleaseSRWLockShared(&g_scanLock);
    if (w.days < 7 || Today() - LastDay(L"week-said") < 7) return;
    SetDay(L"week-said", Today());
    std::map<std::wstring, std::vector<std::wstring>> added;  // вид -> подписи
    for (const tools::WeekChange& c : w.changes)
        if (c.what == tools::WeekChange::ADDED) added[c.entry.kind].push_back(c.entry.label);
    if (added.empty()) return;
    std::wstring text;
    for (const auto& [kind, names] : added) {
        std::wstring list;
        for (size_t i = 0; i < names.size() && i < 3; ++i) list += (i ? L", " : L"") + Short(names[i], 28);
        if (names.size() > 3) list += L" и ещё " + std::to_wstring(names.size() - 3);
        text += (text.empty() ? L"" : L"; ") + std::wstring(tools::WeekKindName(kind)) + L": " + list;
    }
    text += L". Весь список - кольцо утилит, «Система».";
    const std::wstring title = L"Новое за неделю";
    if (autos::Digest(title, text) || g_game || g_busy || g_card.Visible()) return;
    g_card.Show(BoostCard::DONE, title, text, Theme(), 20000, Toast::G_INFO);
}

}  // namespace toolsui
