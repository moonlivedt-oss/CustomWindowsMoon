#include "core/usage.h"
// Обновление программ ночью: когда можно, сам проход winget в фоне, итог в файл и утренняя карточка.
#include <shlobj.h>

#include <atomic>
#include <ctime>

#include "system/auto/autos.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/text.h"
#include "core/themes.h"
#include "cards/boostcard.h"
#include "system/auto/nightupd.h"
#include "system/tools.h"
#include "system/winget.h"

namespace nightupd {

namespace {

const Config* g_cfg = nullptr;
bool (*g_busy)() = nullptr;
BoostCard g_card;
std::atomic<bool> g_running{false};
ULONGLONG g_checkAt = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_summary;              // итог прохода, ждёт, когда вы вернётесь
std::wstring g_lastDay, g_lastText;  // из файла: когда был последний проход и его итог
bool g_force = false;

std::wstring File() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir.empty() ? L"" : dir + L"\\MoonDesk\\winget-night.tsv";
}

std::wstring Today() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t d[16];
    swprintf(d, 16, L"%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return d;
}

void LoadLast() {
    HANDLE f = CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string u(std::min<DWORD>(GetFileSize(f, nullptr), 1 << 16), '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    std::wstring t = Widen(u);
    size_t tab = t.find(L'\t');
    if (tab == std::wstring::npos) return;
    g_lastDay = t.substr(0, tab);
    g_lastText = Trim(t.substr(tab + 1));
}

void SaveLast(const std::wstring& text) {
    std::string u = Narrow(Today() + L"\t" + text + L"\n");
    HANDLE f = CreateFileW(File().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
    CloseHandle(f);
}

DWORD IdleMs() {
    LASTINPUTINFO li{sizeof(li)};
    return GetLastInputInfo(&li) ? GetTickCount() - li.dwTime : 0;
}

bool OnMains() {
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && s.ACLineStatus == 1;
}

void Pass(std::wstring skip, bool accept) {
    std::vector<tools::Upgrade> list;
    if (!tools::WingetUpgrades(list)) {
        autos::Note(L"Обновление ночью: winget не ответил");
        g_running = false;
        return;
    }
    std::wstring exe = tools::WingetExe();
    std::vector<Result> res;
    for (const tools::Upgrade& u : list) {
        if (bg::Quitting()) break;
        if (Skipped(u.id, u.name, skip) || u.id.find_first_of(L"\"&|<>^ ") != std::wstring::npos) continue;
        if (IdleMs() < 60000) break;  // вернулись к компьютеру - не мешаем, остальное в следующую ночь
        std::wstring out;
        std::wstring cmd = L"\"" + exe + L"\" upgrade --id " + u.id + L" -e --silent --disable-interactivity" +
                           (accept ? L" --accept-package-agreements" : L"");
        ULONGLONG t0 = GetTickCount64();
        int code = tools::RunCapture(cmd, out, 15 * 60000);
        if (GetTickCount64() - t0 >= 15 * 60000ull - 1000) code = -1;
        if (code == 0) {
            usage::Hit(L"winget_night");
            if (accept) usage::Hit(L"winget_accept");
        }
        Result r{u.name, u.id, code, L""};
        r.why = Why(code, out);
        res.push_back(r);
        autos::Note(L"Обновление ночью: " + u.name + L" " + u.have + L" -> " + u.avail +
                    (code == 0 ? L" - обновлено" : L" - " + r.why));
    }
    std::wstring sum = Summary(res);
    SaveLast(sum);
    AcquireSRWLockExclusive(&g_lock);
    g_summary = res.empty() ? L"" : sum;
    g_lastDay = Today();
    g_lastText = sum;
    ReleaseSRWLockExclusive(&g_lock);
    autos::Note(L"Обновление ночью: " + sum);
    g_running = false;
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg, bool (*busy)()) {
    g_cfg = cfg;
    g_busy = busy;
    g_card.Create(inst);
    LoadLast();
}

void Tick() {
    g_card.Tick();
    ULONGLONG now = GetTickCount64();
    if (now - g_checkAt < 60000 || !g_cfg) return;
    g_checkAt = now;
    // итог - когда вы вернулись (последний ввод меньше минуты назад)
    std::wstring sum;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_summary.empty() && !g_running && IdleMs() < 60000) sum.swap(g_summary);
    ReleaseSRWLockExclusive(&g_lock);
    if (!sum.empty()) {
        g_card.kind = L"-";
        g_card.Show(BoostCard::DONE, L"Ночью обновлялись программы", sum, CurrentTheme(*g_cfg), 15000, Toast::G_OK);
    }
    if (!g_cfg->wingetNight || g_running) return;
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    bool due = g_force || (InWindow(lt.tm_hour, lt.tm_min) && g_lastDay != Today());
    if (!due || !OnMains() || IdleMs() < (g_force ? 60000u : 30 * 60000u) || (g_busy && g_busy())) return;
    if (tools::WingetExe().empty()) return;
    g_force = false;
    g_running = true;
    autos::Note(L"Обновление ночью: начинаю");
    std::wstring skip = g_cfg->wingetSkip;
    bool accept = g_cfg->wingetAccept;
    if (!bg::Run([skip, accept] { Pass(skip, accept); }, THREAD_PRIORITY_BELOW_NORMAL)) g_running = false;
}

void Shutdown() { g_card.Destroy(); }

std::wstring Status() {
    if (g_running) return L"идёт сейчас";
    AcquireSRWLockExclusive(&g_lock);
    std::wstring s = g_lastDay.empty() ? L"ещё не было" : L"последний раз " + g_lastDay + L": " + g_lastText;
    ReleaseSRWLockExclusive(&g_lock);
    return s;
}

void RunNow() { g_force = true, g_checkAt = 0; }

}  // namespace nightupd
