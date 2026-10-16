#include "app/autos.h"

#include <algorithm>
#include <memory>

#include "app/autostart.h"
#include "core/config.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "app/toolsui.h"
#include "desk/boostcard.h"
#include "desk/scenecard.h"
#include "gfx/scene.h"

namespace autos {

namespace {

HWND g_app = nullptr;
const Config* g_cfg = nullptr;
std::wstring g_cfgPath;
std::function<void()> g_reload;
std::function<void(int, const std::wstring&)> g_openStudio;
BoostCard g_card;
ULONGLONG g_started = 0, g_cleanCheck = 0, g_lowCheck = 0, g_repairCheck = 0, g_dlCheck = 0;
volatile bool g_cleaning = false, g_dlScanning = false;
bool g_game = false, g_dev = false, g_toolsWarned = false;
std::vector<tools::DownloadJunk> g_downloads;
SRWLOCK g_lock = SRWLOCK_INIT;  // журнал и список Загрузок пишутся из потоков
std::wstring g_power, g_repair;

ThemeInfo Theme() { return CurrentTheme(*g_cfg); }

std::wstring Ini() { return tools::DataDir() + L"\\weekly.ini"; }

int Today() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / (24ull * 3600 * 10000000));
}

std::wstring Get(const wchar_t* key, const wchar_t* def = L"0") {
    wchar_t v[64] = L"";
    GetPrivateProfileStringW(L"auto", key, def, v, 64, Ini().c_str());
    return v;
}

void Put(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"auto", key, v.c_str(), Ini().c_str());
}

// сообщения из рабочих потоков
struct Msg {
    enum Kind { CLEAN_LOG, CLEAN_DONE, DOWNLOADS, RECYCLED } kind;
    std::wstring text;
    ULONGLONG bytes = 0;
    int count = 0;
    bool silent = false;  // по расписанию: итог карточкой, но без «работаю»
};

void Post(Msg* m) {
    if (!PostMessageW(g_app, WM_AUTO_JOB, 0, (LPARAM)m)) delete m;
}

// ---------------- тихая чистка ----------------

void RunClean(const std::wstring& why, bool silent) {
    if (g_cleaning) return;
    g_cleaning = true;
    // что чистить - сразу, размеры - из последнего фонового подсчёта: по ним сцена рисует полоски
    std::vector<tools::Junk> list;
    std::vector<tools::Junk> sized = toolsui::Snapshot().junk;
    std::vector<scene::CleanItem> items;
    for (tools::Junk& j : tools::FindJunk(false)) {
        if (j.slow) continue;
        for (const tools::Junk& s : sized)
            if (s.path == j.path) j.bytes = s.bytes;
        items.push_back({j.name, j.bytes});
        list.push_back(j);
    }
    scene::CleanBegin(items);
    if (!silent) SceneCard::Get().Show(SceneCard::CLEAN, L"Тихая чистка", Theme());
    struct Box {
        std::wstring why;
        bool silent;
        std::vector<tools::Junk> list;
    };
    auto* box = new Box{why, silent, list};
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<Box> b((Box*)p);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            ULONGLONG freed = 0;
            for (size_t i = 0; i < b->list.size(); ++i) {
                scene::CleanMark(i, 1);
                ULONGLONG f = tools::CleanJunk(b->list[i], [](const std::wstring&) {});
                scene::CleanMark(i, 2, f);
                freed += f;
            }
            scene::CleanEnd();
            Post(new Msg{Msg::CLEAN_DONE, b->why, freed, 0, b->silent});
            return 0;
        },
        box, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete box;
        g_cleaning = false;
    }
}

// ---------------- Загрузки ----------------

void ScanDl(bool offer) {
    if (g_dlScanning || !g_cfg) return;
    g_dlScanning = true;
    auto* days = new std::pair<int, bool>(g_cfg->downloadsDays, offer);
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<std::pair<int, bool>> a((std::pair<int, bool>*)p);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::vector<tools::DownloadJunk> found = tools::FindDownloadJunk(a->first);
            CoUninitialize();
            AcquireSRWLockExclusive(&g_lock);
            g_downloads = found;
            ReleaseSRWLockExclusive(&g_lock);
            ULONGLONG total = 0;
            for (const tools::DownloadJunk& d : found) total += d.bytes;
            Post(new Msg{Msg::DOWNLOADS, L"", total, (int)found.size(), !a->second});
            g_dlScanning = false;
            return 0;
        },
        days, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete days;
        g_dlScanning = false;
    }
}

void Recycle(std::vector<std::wstring> paths) {
    auto* box = new std::vector<std::wstring>(std::move(paths));
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<std::vector<std::wstring>> list((std::vector<std::wstring>*)p);
            ULONGLONG bytes = 0;
            for (const std::wstring& f : *list) {
                WIN32_FILE_ATTRIBUTE_DATA a;
                if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a))
                    bytes += ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
            }
            int moved = tools::RecycleDownloads(*list, nullptr);
            Post(new Msg{Msg::RECYCLED, L"", moved ? bytes : 0, moved});
            return 0;
        },
        box, 0, nullptr);
    if (th)
        CloseHandle(th);
    else
        delete box;
}

// ---------------- питание ----------------

void ApplyPower(const wchar_t* why) {
    if (!g_cfg->autoPower) {
        g_power.clear();
        return;
    }
    bool battery = tools::OnBattery(), heavy = g_game || g_dev;
    tools::PowerMode want = battery ? (heavy ? tools::POWER_BALANCED : tools::POWER_SAVER)
                                    : (heavy ? tools::POWER_BEST : tools::POWER_BALANCED);
    std::wstring reason = std::wstring(battery ? L"от батареи" : L"на зарядке") + (g_game  ? L", игра"
                                                                                   : g_dev ? L", Unreal"
                                                                                           : L"");
    g_power = std::wstring(tools::PowerModeName(want)) + L" (" + reason + L")";
    if (tools::GetPowerMode() == want) return;
    if (tools::SetPowerMode(want))
        Note(L"Питание: " + std::wstring(tools::PowerModeName(want)) + L" - " + reason + (why ? why : L""));
}

// ---------------- самовосстановление ----------------

std::wstring NewestBackup() {
    std::wstring dir = ExeDir() + L"\\backups", best;
    FILETIME bt{};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\radial-*.cfg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    do {
        if (fd.nFileSizeLow < 64) continue;  // пустая копия не спасёт
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bt) > 0) {
            best = dir + L"\\" + fd.cFileName;
            bt = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

void Repair() {
    if (!g_cfg->autoRepair) {
        g_repair = L"выключено";
        return;
    }
    std::vector<std::wstring> done;
    // автозапуск: возвращаем, только если его хотели (последнее включение из меню или студии)
    int wanted = AutostartWanted();
    if (wanted < 0 && AutostartOn()) SetAutostartWanted(true), wanted = 1;  // включён до появления этой отметки
    if (wanted == 1 && !AutostartOn()) {
        SetAutostart(true);
        if (AutostartOn()) {
            done.push_back(L"автозапуск");
            Note(L"Автозапуск пропал (антивирус или чистильщик?) - вернул задачу планировщика");
        }
    }
    // настройки колец пропали или обнулились - достать последнюю копию
    WIN32_FILE_ATTRIBUTE_DATA a;
    bool cfgOk = GetFileAttributesExW(g_cfgPath.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow >= 64;
    if (!cfgOk) {
        std::wstring b = NewestBackup();
        if (!b.empty() && CopyFileW(b.c_str(), g_cfgPath.c_str(), FALSE)) {
            done.push_back(L"radial.cfg");
            Note(L"radial.cfg пропал или пустой - вернул из копии " + b.substr(b.find_last_of(L'\\') + 1));
            if (g_reload) g_reload();
        }
    }
    // RadialTools.exe пересоздать нельзя - только сказать (один раз за запуск)
    bool toolsOk = GetFileAttributesW((ExeDir() + L"\\RadialTools.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!toolsOk && !g_toolsWarned) {
        g_toolsWarned = true;
        Note(L"RadialTools.exe пропал - очистка и закрытие программ не работают, пока его не пересобрать");
        g_card.Show(BoostCard::DONE, L"Пропал RadialTools.exe",
                    L"Скорее всего, его убрал антивирус. Очистка и «Закрыть» не работают; пересобери проект "
                    L"(build.cmd).",
                    Theme(), 20000, Toast::G_WARN);
    }
    if (!done.empty()) {
        std::wstring list;
        for (const std::wstring& d : done) list += (list.empty() ? L"" : L", ") + d;
        g_card.Show(BoostCard::DONE, L"Восстановлено", list + L". Подробности - в журнале автоматики.", Theme(), 9000,
                    Toast::G_OK);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t t[16];
    swprintf(t, 16, L"%02d:%02d", st.wHour, st.wMinute);
    g_repair = std::wstring(toolsOk ? L"всё на месте" : L"нет RadialTools.exe") + L", проверено в " + t;
}

}  // namespace

// ---------------- журнал ----------------

void Note(const std::wstring& text) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t when[32];
    swprintf(when, 32, L"%02d.%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
    std::wstring line = std::wstring(when) + L"\t" + text + L"\n";
    std::string u = Narrow(line);
    AcquireSRWLockExclusive(&g_lock);
    std::wstring file = tools::DataDir() + L"\\journal.tsv";
    HANDLE f = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        CloseHandle(f);
        // журнал не растёт бесконечно: больше 64 КБ - оставить вторую половину
        if (size.QuadPart > 64 * 1024) {
            HANDLE r = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            std::string all((size_t)size.QuadPart, '\0');
            DWORD got = 0;
            if (r != INVALID_HANDLE_VALUE) {
                ReadFile(r, all.data(), (DWORD)all.size(), &got, nullptr);
                CloseHandle(r);
                all.resize(got);
                size_t cut = all.find('\n', all.size() / 2);
                if (cut != std::string::npos) {
                    HANDLE w = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                    if (w != INVALID_HANDLE_VALUE) {
                        WriteFile(w, all.data() + cut + 1, (DWORD)(all.size() - cut - 1), &put, nullptr);
                        CloseHandle(w);
                    }
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::vector<Entry> Journal(size_t max) {
    std::vector<Entry> out;
    AcquireSRWLockShared(&g_lock);
    HANDLE f = CreateFileW((tools::DataDir() + L"\\journal.tsv").c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string s;
    if (f != INVALID_HANDLE_VALUE) {
        char buf[16384];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) s.append(buf, got);
        CloseHandle(f);
    }
    ReleaseSRWLockShared(&g_lock);
    std::wstring w = Widen(s);
    size_t end = w.size();
    while (end > 0 && out.size() < max) {
        size_t start = w.rfind(L'\n', end - 1);
        std::wstring line =
            w.substr(start == std::wstring::npos ? 0 : start + 1, end - (start == std::wstring::npos ? 0 : start + 1));
        end = start == std::wstring::npos ? 0 : start;
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        out.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
    return out;
}

// ---------------- для студии ----------------

void CleanNow() { RunClean(L"Вручную из студии", false); }
bool Cleaning() { return g_cleaning; }

int LastCleanDays() {
    int d = _wtoi(Get(L"clean_day").c_str());
    return d ? Today() - d : -1;
}

ULONGLONG LastCleanFreed() { return _wcstoui64(Get(L"clean_freed").c_str(), nullptr, 10); }

std::vector<tools::DownloadJunk> Downloads() {
    AcquireSRWLockShared(&g_lock);
    std::vector<tools::DownloadJunk> d = g_downloads;
    ReleaseSRWLockShared(&g_lock);
    return d;
}

void ScanDownloads() { ScanDl(false); }
void RecycleDownloads(const std::vector<std::wstring>& paths) { Recycle(paths); }

std::wstring PowerStatus() {
    if (!g_cfg) return tools::PowerModeName(tools::GetPowerMode());  // студия в режиме снимка
    if (!g_cfg->autoPower)
        return L"сейчас: " + std::wstring(tools::PowerModeName(tools::GetPowerMode())) + L", вручную";
    return g_power.empty() ? std::wstring(tools::PowerModeName(tools::GetPowerMode())) : g_power;
}

std::wstring RepairStatus() { return g_repair.empty() ? L"ещё не проверялось" : g_repair; }

// ---------------- жизненный цикл ----------------

void Init(HINSTANCE inst, HWND app, const Config* cfg, const std::wstring& cfgPath, std::function<void()> reload,
          std::function<void(int, const std::wstring&)> openStudio) {
    g_app = app;
    g_cfg = cfg;
    g_cfgPath = cfgPath;
    g_reload = std::move(reload);
    g_openStudio = std::move(openStudio);
    g_card.Create(inst);
    g_started = g_cleanCheck = g_lowCheck = g_repairCheck = g_dlCheck = GetTickCount64();
    ApplyPower(nullptr);
}

void OnPower() {
    if (g_cfg) ApplyPower(nullptr);  // уведомление может прийти раньше Init
}

void OnModes(bool game, bool dev) {
    if (!g_cfg) return;
    if (game == g_game && dev == g_dev) return;
    g_game = game;
    g_dev = dev;
    ApplyPower(nullptr);
}

void Tick() {
    if (!g_cfg) return;
    g_card.Tick();
    ULONGLONG now = GetTickCount64();
    // самовосстановление: через 45 с после старта, дальше раз в 15 минут
    if (now - g_repairCheck > (g_repairCheck == g_started ? 45000ull : 15 * 60 * 1000ull)) {
        g_repairCheck = now;
        Repair();
    }
    if (g_game) return;  // в игре ничего не чистим и не спрашиваем
    // тихая чистка по расписанию: раз в неделю (проверяем раз в час, первый раз через 3 минуты)
    if (g_cfg->autoClean && now - g_cleanCheck > (g_cleanCheck == g_started ? 3 * 60 * 1000ull : 3600 * 1000ull)) {
        g_cleanCheck = now;
        int last = _wtoi(Get(L"clean_day").c_str());
        if (!last || Today() - last >= 7) RunClean(L"раз в неделю", true);
    }
    // мало места: чистим сразу, не чаще раза в 6 часов
    if (g_cfg->autoClean && now - g_lowCheck > 10 * 60 * 1000) {
        g_lowCheck = now;
        ULONGLONG lowAt = _wcstoui64(Get(L"low_at").c_str(), nullptr, 10);
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULONGLONG wall = (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000;
        if (wall - lowAt > 6 * 3600)
            for (const tools::Volume& v : tools::Volumes())
                if (v.type == DRIVE_FIXED && tools::DiskWarn(v) && Lower(v.root.substr(0, 1)) == L"c") {
                    Put(L"low_at", std::to_wstring(wall));
                    RunClean(L"мало места на " + v.root.substr(0, 2), false);
                    break;
                }
    }
    // Загрузки: раз в неделю разобрать и спросить (проверяем раз в час, первый раз через 5 минут)
    if (g_cfg->autoDownloads && now - g_dlCheck > (g_dlCheck == g_started ? 5 * 60 * 1000ull : 3600 * 1000ull)) {
        g_dlCheck = now;
        int last = _wtoi(Get(L"downloads_day").c_str());
        if (!last || Today() - last >= 7) ScanDl(true);
    }
}

void OnJob(WPARAM, LPARAM lp) {
    std::unique_ptr<Msg> m((Msg*)lp);
    switch (m->kind) {
        case Msg::CLEAN_LOG:  // ход показывает сцена
            return;
        case Msg::CLEAN_DONE:
            g_cleaning = false;
            Put(L"clean_day", std::to_wstring(Today()));
            Put(L"clean_freed", std::to_wstring(m->bytes));
            Note(L"Тихая чистка (" + m->text + L"): освобождено " + tools::Bytes(m->bytes));
            // по расписанию мелочь не показываем - только в журнал
            if (m->silent && m->bytes >= 200ull * 1024 * 1024)  // по просьбе итог уже в сцене
                g_card.Show(BoostCard::DONE, L"Тихая чистка: " + tools::Bytes(m->bytes),
                            L"Временные файлы старше суток и кэши npm, pip и других; " + m->text +
                                L". Кэши шейдеров не трогал.",
                            Theme(), 7000, Toast::G_OK);
            return;
        case Msg::DOWNLOADS: {
            if (m->silent) return;  // студия попросила просто обновить список
            Put(L"downloads_day", std::to_wstring(Today()));
            // мелочь не стоит вопроса
            if (m->count < 3 && m->bytes < 100ull * 1024 * 1024) return;
            std::vector<tools::DownloadJunk> list = Downloads();
            std::wstring kinds;
            int inst = 0, arch = 0, copies = 0, other = 0;
            for (const tools::DownloadJunk& d : list)
                (d.why == L"установщик"       ? inst
                 : d.why == L"уже распакован" ? arch
                 : d.why == L"копия"          ? copies
                                              : other)++;
            if (inst) kinds += Plural(inst, L"установщик", L"установщика", L"установщиков");
            if (arch)
                kinds += (kinds.empty() ? L"" : L", ") +
                         Plural(arch, L"распакованный архив", L"распакованных архива", L"распакованных архивов");
            if (copies) kinds += (kinds.empty() ? L"" : L", ") + Plural(copies, L"копия", L"копии", L"копий");
            if (other) kinds += (kinds.empty() ? L"" : L", ") + std::to_wstring(other) + L" прочих";
            g_card.yesLabel = L"В Корзину";
            g_card.noLabel = L"Показать";
            g_card.onYes = [] {
                g_card.yesLabel.clear();
                g_card.noLabel.clear();
                std::vector<std::wstring> paths;
                for (const tools::DownloadJunk& d : Downloads()) paths.push_back(d.path);
                Recycle(paths);
            };
            g_card.onNo = [] {
                g_card.yesLabel.clear();
                g_card.noLabel.clear();
                if (g_openStudio) g_openStudio(-1, L"downloads");  // студия: «Состояние», Загрузки
            };
            g_card.Show(BoostCard::OFFER, L"Загрузки: " + tools::Bytes(m->bytes) + L" лишнего",
                        kinds + L". В Корзину - вернуть можно оттуда.", Theme(), 30000, Toast::G_ASK);
            return;
        }
        case Msg::RECYCLED:
            Note(L"Загрузки: в Корзину " + std::to_wstring(m->count) + L" файлов, " + tools::Bytes(m->bytes));
            AcquireSRWLockExclusive(&g_lock);
            g_downloads.clear();
            ReleaseSRWLockExclusive(&g_lock);
            g_card.Show(BoostCard::DONE, m->count ? L"Загрузки разобраны" : L"Не получилось",
                        m->count ? std::to_wstring(m->count) + L" файлов, " + tools::Bytes(m->bytes) +
                                       L" - в Корзине. Вернуть можно оттуда."
                                 : L"Корзина не приняла файлы.",
                        Theme(), 7000);
            ScanDl(false);
            return;
    }
}

void Shutdown() { g_card.Destroy(); }

}  // namespace autos
