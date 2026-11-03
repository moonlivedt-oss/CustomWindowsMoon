// Забота о батарее: учёт часов на 100% от сети, карточка, предел заряда ASUS через задачу с правами.
#include <shlobj.h>

#include <atomic>
#include <ctime>

#include "system/auto/autos.h"
#include "core/bg.h"
#include "core/config.h"
#include "core/elevate.h"
#include "core/text.h"
#include "core/themes.h"
#include "cards/boostcard.h"
#include "system/auto/batcare.h"
#include "net/net.h"

namespace batcare {

namespace {

const Config* g_cfg = nullptr;
BoostCard g_card;
std::map<std::wstring, Day> g_days;
int g_limit = 0;           // поставленный предел (0 - нет)
long long g_askAfter = 0;  // отказались - не спрашивать до (unix)
ULONGLONG g_at = 0, g_reapplyAt = 0;
bool g_asus = false;
std::atomic<bool> g_busy{false};

std::wstring Dir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir.empty() ? L"" : dir + L"\\MoonDesk";
}

std::wstring ReadFileText(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string u(std::min<DWORD>(GetFileSize(f, nullptr), 1 << 16), '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    return Widen(u);
}

void WriteFileText(const std::wstring& path, const std::wstring& t) {
    std::string u = Narrow(t);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
    CloseHandle(f);
}

void SaveState() {
    WriteFileText(Dir() + L"\\batcare.tsv", Save(g_days));
    WriteFileText(Dir() + L"\\batcare.ini",
                  L"limit=" + std::to_wstring(g_limit) + L"\nask_after=" + std::to_wstring(g_askAfter) + L"\n");
}

void LoadState() {
    g_days = Parse(ReadFileText(Dir() + L"\\batcare.tsv"));
    for (const std::wstring& line : Split(ReadFileText(Dir() + L"\\batcare.ini"), L'\n')) {
        if (line.rfind(L"limit=", 0) == 0) g_limit = _wtoi(line.c_str() + 6);
        if (line.rfind(L"ask_after=", 0) == 0) g_askAfter = _wtoi64(line.c_str() + 10);
    }
}

std::wstring Today() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t d[16];
    swprintf(d, 16, L"%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return d;
}

bool IsAsus() {
    wchar_t v[128] = {};
    DWORD n = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer", RRF_RT_REG_SZ,
                     nullptr, v, &n) != ERROR_SUCCESS)
        return false;
    return Lower(v).find(L"asus") != std::wstring::npos;
}

// предел через задачу с правами; ask - можно спросить UAC (только по нажатию)
void Apply(int limit, bool ask) {
    if (g_busy.exchange(true)) return;
    bg::Run([limit, ask] {
        std::wstring last;
        tools::Log log = [&last](const std::wstring& s) { last = s; };
        bool ready = net::AdminReadyNow();
        if (!ready && ask) ready = net::SetupAdmin();
        bool ok = false;
        if (ready) {
            elevate::Put(L"ChargeLimit", std::to_wstring(limit));
            ok = net::RunAdmin(net::JOB_CHARGE_LIMIT, log, 40000);
            if (!ok && ask && net::SetupAdmin()) {  // помощник старый и не знает задания - обновить и ещё раз
                elevate::Put(L"ChargeLimit", std::to_wstring(limit));
                ok = net::RunAdmin(net::JOB_CHARGE_LIMIT, log, 40000);
            }
        }
        autos::Note(L"Батарея: предел " + std::to_wstring(limit) + L"% - " +
                    (ok ? std::wstring(L"поставлен") : L"не вышло" + (last.empty() ? L"" : L" (" + last + L")")));
        g_busy = false;
    });
}

void Offer() {
    g_card.kind = L"Забота о батарее";
    g_card.yesLabel = g_asus ? L"Держать 80%" : L"Настройки";
    g_card.noLabel = L"Не надо";
    g_card.onYes = [] {
        if (g_asus) {
            g_limit = 80;
            SaveState();
            Apply(80, true);
        } else {
            ShellExecuteW(nullptr, L"open", L"ms-settings:batterysaver", nullptr, nullptr, SW_SHOWNORMAL);
        }
    };
    g_card.onNo = [] {
        g_askAfter = (long long)time(nullptr) + 30ll * 86400;
        SaveState();
    };
    g_card.more =
        L"Литий-ионная батарея быстрее стареет, когда долго стоит на 100%. Предел 80% - ноутбук перестаёт "
        L"заряжаться на 80%, работает от сети. Нужно в дорогу полной - limit=0 в %APPDATA%\\MoonDesk\\batcare.ini и "
        L"перезагрузка. " +
        std::wstring(g_asus ? L"ASUS сбрасывает предел при перезагрузке - RadialMenu ставит его заново "
                              L"после запуска. Первый раз Windows спросит разрешение для задачи с правами."
                            : L"Этот ноутбук не ASUS: предел ставится в программе производителя.");
    g_card.Show(BoostCard::OFFER, L"Весь день на зарядке - держать 80%?",
                L"Последние дни ноутбук часами стоит от сети на 100% - батарея от этого изнашивается.",
                CurrentTheme(*g_cfg), 60000, Toast::G_ASK);
    g_askAfter = (long long)time(nullptr) + 7ll * 86400;  // не чаще раза в неделю, даже если карточку пропустили
    SaveState();
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg) {
    g_cfg = cfg;
    g_card.Create(inst);
    CreateDirectoryW(Dir().c_str(), nullptr);
    LoadState();
    g_asus = IsAsus();
    g_at = GetTickCount64();
    g_reapplyAt = g_limit > 0 && g_asus ? GetTickCount64() + 60000 : 0;  // ASUS сбрасывает предел - вернуть
}

void Tick() {
    g_card.Tick();
    ULONGLONG now = GetTickCount64();
    if (g_reapplyAt && now >= g_reapplyAt) {
        g_reapplyAt = 0;
        Apply(g_limit, false);  // без вопроса: нет задачи с правами - тихо пропустить
    }
    if (now - g_at < 5 * 60000) return;
    double h = (now - g_at) / 3600000.0;
    g_at = now;
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s) || s.ACLineStatus != 1 || s.BatteryFlag == 128 || s.BatteryLifePercent > 100) return;
    Day& d = g_days[Today()];
    if (s.BatteryLifePercent >= 95) d.fullHours += h;
    if (g_limit > 0 && g_limit < 95 && s.BatteryLifePercent + 5 >= g_limit && s.BatteryLifePercent <= g_limit + 1)
        d.limitHours += h;
    SaveState();
    if (g_limit == 0 && (long long)time(nullptr) >= g_askAfter && ShouldOffer(g_days) && !g_card.Visible()) Offer();
}

void Shutdown() { g_card.Destroy(); }

std::wstring Status() {
    auto it = g_days.find(Today());
    wchar_t b[160];
    swprintf(b, 160, L"сегодня на 100%% от сети: %.1f ч; ", it == g_days.end() ? 0.0 : it->second.fullHours);
    std::wstring s = b;
    if (g_limit > 0) {
        swprintf(b, 160, L"предел %d%%, за неделю у предела вместо 100%%: %.0f ч", g_limit, SavedHours(g_days));
        s += b;
    } else {
        s += L"предела нет";
    }
    return s;
}

}  // namespace batcare
