// Сон после работы (Л6): раз в 5 с - идёт ли ещё работа, затихло - отсчёт с карточкой, потом сон или выключение.
#include <windows.h>

#include <map>

#include "app/afterwork.h"
#include "app/autos.h"
#include "core/config.h"
#include "core/fx.h"
#include "core/procsnap.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "tools/guard.h"
#include "tools/tools.h"

namespace afterwork {

namespace {

const Config* g_cfg = nullptr;
BoostCard g_card;
Mode g_mode = OFF;
Countdown g_count;
ULONGLONG g_checkAt = 0, g_lastAt = 0;
std::wstring g_status;
bool g_counting = false;
std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> g_prev;  // pid -> время создания, процессор на прошлом проходе

const wchar_t* Verb() { return g_mode == SHUTDOWN ? L"выключу" : L"усыплю"; }

std::wstring Masks() {
    // как «не спать, пока работает», но без steamwebhelper: он живёт, пока открыт Steam (загрузку видно по папке)
    std::wstring m = g_cfg ? g_cfg->noSleepApps : L"";
    std::wstring out;
    size_t at = 0;
    while (at <= m.size()) {
        size_t bar = m.find(L'|', at);
        std::wstring one = m.substr(at, bar == std::wstring::npos ? std::wstring::npos : bar - at);
        if (!one.empty() && _wcsicmp(one.c_str(), L"steamwebhelper.exe") != 0) out += (out.empty() ? L"" : L"|") + one;
        if (bar == std::wstring::npos) break;
        at = bar + 1;
    }
    return out + L"|UnrealPak.exe|UnrealEditor-Cmd.exe|ffmpeg.exe|HandBrakeCLI.exe|7z.exe|7zG.exe|WinRAR.exe|xcopy.exe";
}

// counting - отсчёт уже идёт: мышь его не сбивает (вы подошли к карточке - на ней «Отмена»), сбивает только работа
std::wstring Busy(bool counting) {
    if (fx::Game()) return L"игра";
    LASTINPUTINFO li{sizeof(li)};
    if (!counting && GetLastInputInfo(&li) && GetTickCount() - li.dwTime < 120000) return L"вы за компьютером";
    ULONGLONG now = GetTickCount64();
    double dt = g_lastAt ? (now - g_lastAt) / 1000.0 : 0;
    g_lastAt = now;
    int cores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    std::vector<ProcNow> procs;
    std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> next;
    for (const procsnap::Info& p : procsnap::Session()) {
        next[p.pid] = {p.created, p.cpu};
        auto it = g_prev.find(p.pid);
        double share = 0;
        if (dt > 0 && it != g_prev.end() && it->second.first == p.created && p.cpu >= it->second.second)
            share = (p.cpu - it->second.second) / 1e7 / dt / std::max(1, cores);
        procs.push_back({p.name, share});
    }
    g_prev.swap(next);
    return BusyReason(procs, Masks(), FreshPartials(), SteamDownloading());
}

bool Privilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

void Act() {
    Mode m = g_mode;
    g_mode = OFF;
    g_counting = false;
    g_card.Hide();
    autos::Note(m == SHUTDOWN ? L"Сон после работы: всё закончилось - выключаю компьютер"
                              : L"Сон после работы: всё закончилось - усыпляю компьютер");
    Privilege();
    if (m == SHUTDOWN) {
        ExitWindowsEx(EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        return;
    }
    using Suspend = BOOLEAN(WINAPI*)(BOOLEAN, BOOLEAN, BOOLEAN);
    if (HMODULE pp = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        if (auto f = (Suspend)(void*)GetProcAddress(pp, "SetSuspendState")) f(FALSE, FALSE, FALSE);
}

void ShowCount(int secs) {
    g_card.kind = L"-";  // отсчёт в пропущенные не идёт
    g_card.yesLabel = g_mode == SHUTDOWN ? L"Выключить сейчас" : L"Усыпить сейчас";
    g_card.noLabel = L"Отмена";
    g_card.onYes = [] { Act(); };
    g_card.onNo = [] {
        Arm(OFF);
        autos::Note(L"Сон после работы отменён с карточки");
    };
    g_card.Show(BoostCard::OFFER,
                std::wstring(g_mode == SHUTDOWN ? L"Выключу" : L"Усыплю") + L" через " + std::to_wstring(secs) + L" с",
                L"Всё, чего ждали, закончилось. Отмена - если ещё нужно.", CurrentTheme(*g_cfg), secs * 1000 + 2000,
                Toast::G_INFO);
}

}  // namespace

// свежий недокачанный файл в Загрузках (браузер или торрент пишет его прямо сейчас)
int FreshPartials() {
    std::wstring dir = tools::DownloadsDir();
    if (dir.empty()) return 0;
    int n = 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    for (const wchar_t* mask : {L"*.crdownload", L"*.part", L"*.partial", L"*.download", L"*.!ut", L"*.opdownload"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + mask).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            ULONGLONG wt = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            if (now - wt < 2ull * 60 * 10000000) ++n;  // писали за последние 2 минуты - качается
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}

// Steam качает: в steamapps\downloading есть папки игр
bool SteamDownloading() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%\\Steam\\steamapps\\downloading\\*", p, MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') any = true;
    } while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

void Init(HINSTANCE inst, const Config* cfg) {
    g_cfg = cfg;
    g_card.Create(inst);
}

void Arm(Mode m) {
    g_mode = m;
    g_count.Reset();
    g_counting = false;
    g_checkAt = 0;
    g_lastAt = 0;
    g_prev.clear();
    if (m == OFF) {
        g_status.clear();
        if (g_card.Visible()) g_card.Hide();
        return;
    }
    g_status = L"смотрю, что ещё работает";
    autos::Note(m == SHUTDOWN ? L"Сон после работы: выключить, когда всё закончится"
                              : L"Сон после работы: усыпить, когда всё закончится");
}

Mode Armed() { return g_mode; }

std::wstring Status() { return g_mode == OFF ? L"" : g_status; }

void Tick() {
    if (!g_cfg) return;
    g_card.Tick();
    if (g_mode == OFF) return;
    ULONGLONG now = GetTickCount64();
    if (now < g_checkAt) return;
    g_checkAt = now + 5000;
    std::wstring why = Busy(g_counting);
    Countdown::Out o = g_count.Feed(!why.empty(), now);
    if (o == Countdown::WAIT) {
        g_status = why.empty() ? L"всё затихло, жду 2 минуты" : L"жду: " + why;
        if (g_counting) g_card.Hide();  // работа снова пошла - отсчёт с начала
        g_counting = false;
    } else if (o == Countdown::COUNT) {
        int secs = g_count.SecondsLeft(now);
        g_status = std::wstring(Verb()) + L" через " + std::to_wstring(secs) + L" с";
        if (!g_counting) ShowCount(secs);
        g_counting = true;
    } else {
        Act();
    }
}

void Shutdown() { g_card.Destroy(); }

}  // namespace afterwork
