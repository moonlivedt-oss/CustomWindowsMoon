#include "app/autos.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <set>

#include "app/autostart.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "tools/journal.h"
#include "app/toolsui.h"
#include "app/weekreport.h"
#include "core/cfgdiff.h"
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
ULONGLONG g_started = 0, g_cleanCheck = 0, g_lowCheck = 0, g_repairCheck = 0, g_dlCheck = 0, g_reportCheck = 0,
          g_backupCheck = 0;
std::atomic<bool> g_reporting{false};
std::atomic<bool> g_dlScanning{false};
bool g_game = false, g_dev = false, g_toolsWarned = false;
std::vector<tools::DownloadJunk> g_downloads;
unsigned g_downloadsGen = 0;    // растёт под g_lock при каждой правке g_downloads (DownloadsIfChanged)
SRWLOCK g_lock = SRWLOCK_INIT;  // список Загрузок пишется из потоков
std::wstring g_power, g_repair;
bool g_powerHeld = false;  // режим питания вернули отменой (Л3): сами не трогаем до смены источника питания

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
    enum Kind { CLEAN_LOG, CLEAN_DONE, DOWNLOADS, RECYCLED, REPORT, UNDONE } kind;
    std::wstring text;
    ULONGLONG bytes = 0;
    int count = 0;
    bool silent = false;                                             // по расписанию: итог карточкой, но без «работаю»
    std::wstring lockers;                                            // чистка: кто не дал удалить занятое
    std::function<void(bool, const std::wstring&)>* undo = nullptr;  // UNDONE: итог отмены (Л3)
};

void Post(Msg* m) {
    if (!PostMessageW(g_app, WM_AUTO_JOB, 0, (LPARAM)m)) delete m;
}

// ---------------- отчёт за неделю и копия radial.cfg ----------------

std::wstring ReadUtf8(const std::wstring& path) {
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string raw(1024 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, raw.data(), (DWORD)raw.size(), &got, nullptr);
    CloseHandle(f);
    raw.resize(got);
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    std::wstring s(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), s.data(), n);
    return s;
}

// Отчёт за неделю: собрать в фоне (снимок того, что уже знают подсчёты), итог - сообщением в поток окна
void MakeReport() {
    if (g_reporting.exchange(true)) return;
    UiColors colors = UiPalette(*g_cfg);
    if (!bg::Run(
            [colors] {
                std::vector<std::wstring> deltas;
                std::wstring png = weekreport::Make(colors, &deltas);
                auto* m = new Msg{Msg::REPORT, png};
                for (size_t i = 0; i < deltas.size() && i < 3; ++i) m->lockers += (i ? L"; " : L"") + deltas[i];
                Post(m);
                g_reporting = false;
            },
            THREAD_PRIORITY_LOWEST))
        g_reporting = false;
}

// Копия radial.cfg раз в сутки - только если он отличается от самой свежей копии (правка руками, импорт, правила)
void DailyBackup() {
    std::wstring dir = ExeDir() + L"\\backups";
    std::wstring newest;
    WIN32_FIND_DATAW fd;
    std::vector<std::wstring> all;
    HANDLE h = FindFirstFileW((dir + L"\\radial-*.cfg").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do all.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(all.rbegin(), all.rend());  // имена с датой: новые первыми
    for (const std::wstring& n : all)
        if (n.size() >= 23 && iswdigit(n[7])) {  // radial-ГГГГ-..., не «radial-before-...»
            newest = n;
            break;
        }
    std::wstring cur = ReadUtf8(g_cfgPath);
    Put(L"backup_day", std::to_wstring(Today()));
    if (cur.empty()) return;
    std::vector<cfgdiff::Change> d = newest.empty() ? std::vector<cfgdiff::Change>{{}}  // копий нет - первая
                                                    : cfgdiff::Diff(ReadUtf8(dir + L"\\" + newest), cur);
    if (d.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"radial-%04d-%02d-%02d_%02d-%02d-%02d.cfg", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    if (!CopyFileW(g_cfgPath.c_str(), (dir + L"\\" + name).c_str(), FALSE)) return;
    // как у студии: 10 последних с датой в имени
    all.insert(all.begin(), name);
    int kept = 0;
    for (const std::wstring& n : all)
        if (n.size() >= 23 && iswdigit(n[7]) && ++kept > 10) DeleteFileW((dir + L"\\" + n).c_str());
    std::wstring sum = newest.empty() ? L"первая копия" : cfgdiff::Summary(d);
    Put(L"backup_what", sum);
    // отмена - вернуть настройки, какими они были в прошлой копии
    Note(L"Копия radial.cfg за сутки: " + sum, newest.empty() ? L"" : tools::UndoPack(L"cfg", {dir + L"\\" + newest}));
}

// ---------------- тихая чистка ----------------

void RunClean(const std::wstring& why, bool silent) {
    if (!toolsui::CleanClaim()) return;  // уже чистит кольцо, студия или прошлый запуск
    // что чистить - сразу, размеры - из последнего фонового подсчёта: по ним сцена рисует полоски. Если подсчёт
    // был, пустые кэши и TEMP, где только свежее, пропускаем - запускать ради них RadialTools незачем.
    toolsui::State snap = toolsui::Snapshot();
    std::vector<tools::Junk> list;
    std::vector<scene::CleanItem> items;
    for (tools::Junk& j : tools::FindJunk(false)) {
        if (j.slow) continue;
        auto s =
            std::find_if(snap.junk.begin(), snap.junk.end(), [&](const tools::Junk& x) { return x.path == j.path; });
        if (s != snap.junk.end()) j.bytes = s->bytes;
        if (snap.at && !j.bytes) continue;
        items.push_back({j.name, j.bytes});
        list.push_back(j);
    }
    if (list.empty()) {
        toolsui::CleanRelease();
        Post(new Msg{Msg::CLEAN_DONE, why, 0, 0, true});
        if (!silent)
            g_card.Show(BoostCard::DONE, L"Чистить нечего",
                        L"Кэши пустые, а временные файлы свежие или заняты программами - их не трогаю, дойдёт очередь "
                        L"в следующий раз.",
                        Theme(), 6000, Toast::G_OK);
        return;
    }
    scene::CleanBegin(items);
    if (!silent) SceneCard::Get().Show(SceneCard::CLEAN, L"Тихая чистка", Theme());
    bool started = bg::Run(
        [why, silent, list] {
            std::wstring who;
            ULONGLONG freed = toolsui::CleanList(list, nullptr, &who);  // отпускает чистку сама
            Msg* m = new Msg{Msg::CLEAN_DONE, why, freed, 0, silent};
            m->lockers = who;
            Post(m);
        },
        THREAD_PRIORITY_BELOW_NORMAL);
    if (!started) {
        scene::CleanEnd();
        toolsui::CleanRelease();
    }
}

// ---------------- Загрузки ----------------

void ScanDl(bool offer) {
    if (g_dlScanning || !g_cfg) return;
    g_dlScanning = true;
    int days = g_cfg->downloadsDays;
    bool started = bg::Run(
        [days, offer] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::vector<tools::DownloadJunk> found = tools::FindDownloadJunk(days);
            CoUninitialize();
            AcquireSRWLockExclusive(&g_lock);
            g_downloads = found;
            ++g_downloadsGen;
            ReleaseSRWLockExclusive(&g_lock);
            ULONGLONG total = 0;
            for (const tools::DownloadJunk& d : found) total += d.bytes;
            Post(new Msg{Msg::DOWNLOADS, L"", total, (int)found.size(), !offer});
            g_dlScanning = false;
        },
        THREAD_PRIORITY_LOWEST);
    if (!started) g_dlScanning = false;
}

void Recycle(std::vector<std::wstring> paths) {
    bg::Run([list = std::move(paths)] {
        ULONGLONG bytes = 0;
        for (const std::wstring& f : list) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a))
                bytes += ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        }
        int moved = tools::RecycleDownloads(list, nullptr);
        std::vector<std::wstring> gone;  // что на деле ушло в Корзину - его и вернёт отмена
        for (const std::wstring& f : list)
            if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) gone.push_back(f);
        Post(new Msg{Msg::RECYCLED, gone.empty() ? L"" : tools::UndoPack(L"recycle", gone), moved ? bytes : 0, moved});
    });
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
    if (g_powerHeld) {
        g_power = L"как вернули вручную - до смены питания";
        return;
    }
    tools::PowerMode was = tools::GetPowerMode();
    if (was == want) return;
    if (tools::SetPowerMode(want))
        Note(L"Питание: " + std::wstring(tools::PowerModeName(want)) + L" - " + reason + (why ? why : L""),
             was == tools::POWER_UNKNOWN ? L"" : tools::UndoPack(L"power", {std::to_wstring((int)was)}));
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
    // выключили в Диспетчере задач или планировщике - это решение пользователя, а не поломка: не спорить с ним
    // (программа, которая сама включает себя обратно, ведёт себя как вредонос - и антивирус так и решит)
    if (wanted == 1 && !AutostartOn() && AutostartDisabledByUser()) {
        SetAutostartWanted(false), wanted = 0;
        Note(L"Автозапуск выключен в Диспетчере задач или планировщике - не возвращаю");
    }
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
        if (!Digest(L"Восстановлено: " + list, L""))
            g_card.Show(BoostCard::DONE, L"Восстановлено", list + L". Подробности - в журнале автоматики.", Theme(),
                        9000, Toast::G_OK);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t t[16];
    swprintf(t, 16, L"%02d:%02d", st.wHour, st.wMinute);
    g_repair = std::wstring(toolsOk ? L"всё на месте" : L"нет RadialTools.exe") + L", проверено в " + t;
}

}  // namespace

// ---------------- журнал ----------------
// Два файла: journal.tsv дописывается, а перевалив за 64 КБ, становится
// journal.old.tsv (прежний старый уходит). Раньше при переполнении весь файл
// перечитывался и переписывался под замком - теперь только переименование.

std::wstring DigestFile() { return tools::DataDir() + L"\\digest.tsv"; }

std::vector<std::pair<std::wstring, std::wstring>> DigestRead() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    HANDLE f = CreateFileW(DigestFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::string bytes;
    char buf[8192];
    DWORD got = 0;
    while (bytes.size() < (1 << 20) && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
    CloseHandle(f);
    for (const std::wstring& line : Split(Widen(bytes), L'\n')) {
        size_t tab = line.find(L'\t');
        if (tab != std::wstring::npos) out.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
    return out;
}

bool Digest(const std::wstring& title, const std::wstring& text) {
    if (!g_cfg || !g_cfg->digest) return false;
    auto flat = [](std::wstring s) {
        for (wchar_t& ch : s)
            if (ch == L'\t' || ch == L'\n' || ch == L'\r') ch = L' ';
        return s;
    };
    std::string line = Narrow(flat(title) + L"\t" + flat(text) + L"\n");
    HANDLE f = CreateFileW(DigestFile().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, line.data(), (DWORD)line.size(), &put, nullptr);
    CloseHandle(f);
    return ok;
}

int DigestCount() { return (int)DigestRead().size(); }

// Вечером (digest_hour и позже), не в игре и не поверх другой карточки - всё накопленное одной карточкой.
void DigestTick() {
    static ULONGLONG at = 0;
    ULONGLONG now = GetTickCount64();
    if (now - at < 60000 || !g_cfg || !g_cfg->digest || g_game || g_card.Visible()) return;
    at = now;
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (st.wHour < g_cfg->digestHour || Get(L"digest_day") == std::to_wstring(Today())) return;
    std::vector<std::pair<std::wstring, std::wstring>> items = DigestRead();
    if (items.empty()) return;
    Put(L"digest_day", std::to_wstring(Today()));
    DeleteFileW(DigestFile().c_str());
    std::wstring sub;
    for (size_t i = 0; i < items.size() && i < 4; ++i) sub += (i ? L"; " : L"") + items[i].first;
    if (items.size() > 4) sub += L" и ещё " + std::to_wstring(items.size() - 4);
    g_card.yesLabel = L"Журнал";
    g_card.noLabel = L"Понятно";
    g_card.onYes = [] {
        if (g_openStudio) g_openStudio(11, L"");  // «Автоматика»: журнал целиком
    };
    g_card.onNo = nullptr;
    g_card.kind = L"-";  // сам дайджест не глушится и в пропущенные не идёт
    g_card.Show(BoostCard::OFFER, L"За день: " + Plural((int)items.size(), L"событие", L"события", L"событий"), sub,
                Theme(), 30000, Toast::G_INFO);
}

void Note(const std::wstring& text, const std::wstring& undo) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t when[32];
    swprintf(when, 32, L"%02d.%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
    // больше 64 КБ - уходит в journal.old.tsv (tools/journal.cpp)
    tools::JournalAdd(tools::DataDir(), when, text, 64 * 1024, undo);
}

// ---------------- отмена записи (Л3) ----------------

std::wstring UndoneFile() { return tools::DataDir() + L"\\undone.tsv"; }

// отменённое - в памяти (студия спрашивает на каждой строке журнала), файл читается один раз
std::set<std::wstring> g_undone;
bool g_undoneRead = false;

bool Undone(const std::wstring& undo) {
    if (undo.empty()) return false;
    if (!g_undoneRead) {
        g_undoneRead = true;
        std::wstring all = ReadUtf8(UndoneFile());
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find(L'\n', at);
            if (end == std::wstring::npos) end = all.size();
            std::wstring line = all.substr(at, end - at);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (!line.empty()) g_undone.insert(line);
            at = end + 1;
        }
    }
    return g_undone.count(undo) > 0;
}

void UndoEntry(const Entry& e) {
    tools::Undo u;
    if (!tools::UndoParse(e.undo, u) || Undone(e.undo)) return;
    auto done = [undo = e.undo, text = e.text](bool ok, const std::wstring& what) {
        if (ok) {
            g_undone.insert(undo);
            std::string line = Narrow(undo + L"\n");
            HANDLE f =
                CreateFileW(UndoneFile().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD put = 0;
                WriteFile(f, line.data(), (DWORD)line.size(), &put, nullptr);
                CloseHandle(f);
            }
            Note(L"Отменено: " + text + L" - " + what);
        }
        g_card.Show(BoostCard::DONE, ok ? L"Отменено" : L"Не получилось отменить", what, Theme(), 6000);
    };
    if (u.kind == L"power") {
        auto mode = (tools::PowerMode)_wtoi(u.args[0].c_str());
        bool ok = tools::SetPowerMode(mode);
        if (ok) g_powerHeld = true;
        done(ok, ok ? L"режим питания снова «" + std::wstring(tools::PowerModeName(mode)) +
                          L"», сам не переключу до смены питания"
                    : L"Windows не дала сменить режим питания");
    } else if (u.kind == L"cfg") {
        // нынешние настройки - сперва в копию (её тоже можно вернуть из «Журнала» студии), потом прежние на место
        std::wstring prev = u.args[0], dir = ExeDir() + L"\\backups";
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[80];
        swprintf(name, 80, L"\\radial-before-undo-%04d-%02d-%02d_%02d-%02d-%02d.cfg", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
        CreateDirectoryW(dir.c_str(), nullptr);
        bool ok = GetFileAttributesW(prev.c_str()) != INVALID_FILE_ATTRIBUTES &&
                  CopyFileW(g_cfgPath.c_str(), (dir + name).c_str(), FALSE) &&
                  CopyFileW(prev.c_str(), g_cfgPath.c_str(), FALSE);
        if (ok && g_reload) g_reload();
        done(ok, ok ? L"настройки - как в копии " + prev.substr(prev.find_last_of(L'\\') + 1) +
                          L"; нынешние сохранены в " + std::wstring(name + 1)
                    : L"прежней копии уже нет");
    } else if (u.kind == L"recycle") {
        g_card.Show(BoostCard::WORKING, L"Возвращаю из Корзины", std::to_wstring(u.args.size()) + L" файлов", Theme());
        auto* box = new std::function<void(bool, const std::wstring&)>(done);
        if (!bg::Run([paths = u.args, box] {
                int n = tools::RestoreRecycled(paths);
                auto* m =
                    new Msg{Msg::UNDONE,
                            n ? L"вернул в Загрузки " + std::to_wstring(n) + L" из " + std::to_wstring(paths.size())
                              : L"в Корзине их уже нет",
                            0, n};
                m->undo = box;
                Post(m);
            }))
            delete box;
    }
}

std::vector<Entry> Journal(size_t max) { return tools::JournalRead(tools::DataDir(), max); }

// ---------------- для студии ----------------

void CleanNow() { RunClean(L"Вручную из студии", false); }
bool Cleaning() { return toolsui::CleanBusy(); }  // любая чистка кэшей, не только своя

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

bool DownloadsIfChanged(std::vector<tools::DownloadJunk>& out, unsigned& gen) {
    AcquireSRWLockShared(&g_lock);
    bool changed = gen != g_downloadsGen;
    if (changed) out = g_downloads, gen = g_downloadsGen;
    ReleaseSRWLockShared(&g_lock);
    return changed;
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

void OpenState() {
    if (g_openStudio) g_openStudio(-1, L"");
}

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
    g_powerHeld = false;             // сменился источник питания - снова решаем сами
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
    DigestTick();
    ULONGLONG now = GetTickCount64();
    // самовосстановление: через 45 с после старта, дальше раз в 15 минут
    if (now - g_repairCheck > (g_repairCheck == g_started ? 45000ull : 15 * 60 * 1000ull)) {
        g_repairCheck = now;
        Repair();
    }
    // копия radial.cfg: раз в сутки, если поменялся (проверяем раз в час, первый раз через 2 минуты)
    if (g_cfg->autoBackup && now - g_backupCheck > (g_backupCheck == g_started ? 2 * 60 * 1000ull : 3600 * 1000ull)) {
        g_backupCheck = now;
        if (_wtoi(Get(L"backup_day").c_str()) != Today()) DailyBackup();
    }
    if (g_game) return;  // в игре ничего не чистим и не спрашиваем
    // отчёт за неделю: раз в 7 дней, вечером (с часа дайджеста), проверяем раз в час
    if (g_cfg->autoReport && now - g_reportCheck > (g_reportCheck == g_started ? 10 * 60 * 1000ull : 3600 * 1000ull)) {
        g_reportCheck = now;
        SYSTEMTIME t;
        GetLocalTime(&t);
        int last = _wtoi(Get(L"report_day").c_str());
        if ((!last || Today() - last >= 7) && t.wHour >= g_cfg->digestHour && !g_dev) MakeReport();
    }
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
            for (const tools::Volume& v : tools::VolumesCached())
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
            Put(L"clean_day", std::to_wstring(Today()));
            Put(L"clean_freed", std::to_wstring(m->bytes));
            Note(L"Тихая чистка (" + m->text + L"): освобождено " + tools::Bytes(m->bytes) +
                 (m->lockers.empty() ? L"" : L"; не дали удалить: " + m->lockers));
            // по расписанию мелочь не показываем - только в журнал
            if (m->silent && m->bytes >= 200ull * 1024 * 1024 &&  // по просьбе итог уже в сцене
                !Digest(L"Тихая чистка: " + tools::Bytes(m->bytes), m->text))
                g_card.Show(BoostCard::DONE, L"Тихая чистка: " + tools::Bytes(m->bytes),
                            L"Временные файлы (старые и не занятые) и кэши npm, pip и других; " + m->text +
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
        case Msg::REPORT: {
            if (m->text.empty()) {
                Note(L"Отчёт за неделю не собрался");
                return;
            }
            Put(L"report_day", std::to_wstring(Today()));
            Note(L"Отчёт за неделю: " + m->text);
            std::wstring png = m->text,
                         text = (m->lockers.empty() ? std::wstring(L"Место, сеть, что поставилось - одной картинкой")
                                                    : L"К прошлой неделе: " + m->lockers) +
                                L". Файл: " + png;
            if (Digest(L"Отчёт за неделю", text)) return;
            g_card.yesLabel = L"Открыть";
            g_card.noLabel = L"Понятно";
            g_card.onYes = [png] {
                g_card.yesLabel.clear();
                g_card.noLabel.clear();
                ShellExecuteW(nullptr, L"open", png.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            };
            g_card.onNo = [] {
                g_card.yesLabel.clear();
                g_card.noLabel.clear();
            };
            g_card.Show(BoostCard::OFFER, L"Отчёт за неделю", text, Theme(), 20000, Toast::G_OK);
            return;
        }
        case Msg::UNDONE:
            if (m->undo) {
                (*m->undo)(m->count > 0, m->text);
                delete m->undo;
            }
            return;
        case Msg::RECYCLED:
            Note(L"Загрузки: в Корзину " + std::to_wstring(m->count) + L" файлов, " + tools::Bytes(m->bytes), m->text);
            AcquireSRWLockExclusive(&g_lock);
            g_downloads.clear();
            ++g_downloadsGen;
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

std::wstring ReportStatus() {
    int last = _wtoi(Get(L"report_day").c_str());
    if (!last) return L"ещё не было";
    int d = Today() - last;
    return d == 0 ? L"был сегодня" : d == 1 ? L"был вчера" : L"был " + Plural(d, L"день", L"дня", L"дней") + L" назад";
}

std::wstring BackupStatus() {
    int last = _wtoi(Get(L"backup_day").c_str());
    if (!last) return L"ещё не проверялось";
    std::wstring what = Get(L"backup_what", L"");
    return (Today() == last ? std::wstring(L"проверено сегодня")
                            : L"проверено " + std::to_wstring(Today() - last) + L" дн. назад") +
           (what.empty() ? L"" : L", последняя копия: " + what);
}

void Shutdown() { g_card.Destroy(); }

}  // namespace autos
