#pragma once
// Внутреннее устройство утилит, общее для toolsui.cpp (ядро: фоновые подсчёты, слежка, чистка, недельные
// проверки), toolsui_ring.cpp (сборка кольца «Утилиты») и toolsui_run.cpp (что делает каждый пункт).
#include "app/toolsui.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <vector>
#include <shellapi.h>
#include <shlobj.h>
#include "core/bg.h"
#include "core/config.h"
#include "core/elevate.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "desk/scenecard.h"
#include "gfx/scene.h"
#include "menu/menu.h"
#include "net/net.h"
#include "app/autos.h"
#include "tools/selfwatch.h"
#include "tools/tools.h"
#include "tools/weekdiff.h"

namespace toolsui {

constexpr int kDangerScore = 6;
constexpr UINT kSecTag = 0x5E;  // wParam у WM_TOOLS_JOB: проверка защиты закончилась
constexpr int kDdcDays = 30, kIdleDays = 60;
constexpr UINT kWatchTag = 0x5F;   // wParam у WM_TOOLS_JOB: новый снимок слежки
constexpr UINT kWeeklyTag = 0x57;  // wParam у WM_TOOLS_JOB: итог недельной проверки
constexpr UINT kLeftTag = 0x5B;    // wParam у WM_TOOLS_JOB: задание, брошенное по «Стоп», доработало (LeftMsg)
constexpr UINT kSelfTag = 0x5C;    // wParam у WM_TOOLS_JOB: часовой снимок своей памяти (SelfMsg)
constexpr UINT kDiskTag = 0x5D;    // wParam у WM_TOOLS_JOB: карточка «Мало места» собрана

// Тяжёлое (обход тысяч файлов, версии программ) считается в фоне раз в полчаса: кольцо открывается сразу
// и показывает последнее известное.
struct Scan {
    std::vector<tools::Junk> junk;
    std::vector<tools::ProjectJunk> projects;
    std::vector<tools::Conflict> conflicts;
    std::vector<tools::Growth> growth;
    ULONGLONG ddcOld = 0;  // кэш Unreal, не открывавшийся 30 дней
    tools::Driver driver;
    std::vector<tools::ToolVersion> versions;  // раз в неделю отдельно, как драйвер
    // система и разработка: всё это секунды, поэтому тоже в фоне
    std::vector<tools::GitRepo> git;
    std::vector<tools::VirtualDisk> vhd;
    std::vector<tools::Leftover2> leftovers;
    std::vector<tools::DeviceProblem> devices;
    tools::UpdateInfo updates;
    tools::WeekReport week;  // что изменилось за неделю (И36): снимок раз в сутки
};

struct JobMsg {
    std::wstring title, text;
    bool done = false, ok = false;
    bool quiet = false;  // ход и итог показывает сцена, не текстовая карточка
};

// ---- общие данные (определены в toolsui.cpp) ----
// Потоки: что ниже без atomic и без замка - только поток окна (кольцо, Tick, OnJob, OnModes, студия, карточки).
// Слежка (WatchLoop) пишет под g_watchLock, фоновый подсчёт и поиск дублей - под g_scanLock, «Защита» - под
// g_secLock; задачи RunJob меняют только atomic (g_scanAt, g_secAt) и данные под этими замками.
extern HWND g_app;
extern std::function<void(int, const std::wstring&)> g_openStudio;
extern const Config* g_cfg;
extern BoostCard g_card;
extern std::atomic<bool> g_busy;
extern std::atomic<bool> g_jobCancel;
extern std::atomic<bool> g_cleanBusy;
extern ULONGLONG g_diskCheck, g_weeklyCheck;
extern std::set<std::wstring> g_warned;
extern std::vector<tools::StartItem> g_starts;
extern std::vector<tools::Junk> g_junk;
extern std::vector<tools::HeavyApp> g_apps;
extern std::vector<tools::Listener> g_ports;
extern std::vector<tools::Which> g_which;
extern std::vector<tools::ProjectJunk> g_projects;
extern std::vector<tools::Conflict> g_conflicts;
extern std::vector<tools::Listener> g_oldPorts;
extern std::vector<tools::Leftover> g_tails;
extern std::vector<tools::ProcHistory::Leak> g_leakList;
extern std::vector<tools::ProcHistory::Hog> g_hogs;
extern std::vector<std::wstring> g_wake;
extern std::vector<tools::ToolVersion> g_vers;
extern std::vector<tools::DupGroup> g_dups;
extern ULONGLONG g_dupsAt;
extern std::vector<tools::DupGroup> g_dupList;
extern std::vector<tools::Suspect> g_finds;
extern std::vector<tools::SecurityApp> g_secApps;
extern std::vector<tools::BrowserExt> g_exts;  // расширения с заметными правами - как в кольце (sec-ext:i)
extern std::vector<tools::VirtualDisk> g_vhd;
extern std::vector<tools::Leftover2> g_leftovers;
extern std::vector<tools::GitRepo> g_git;
extern std::vector<tools::DeviceProblem> g_devs;
extern tools::UpdateInfo g_updates;
extern ULONGLONG g_rebootAt;
extern SRWLOCK g_secLock;
extern tools::SecurityScan g_sec;
extern std::atomic<ULONGLONG> g_secAt;
extern std::atomic<bool> g_secRunning;
extern std::set<std::wstring> g_secWarned;
extern bool g_secAsked;
extern SRWLOCK g_scanLock;
extern Scan g_scan;
extern std::atomic<ULONGLONG> g_scanAt;
extern ULONGLONG g_started;
extern std::atomic<bool> g_scanRunning, g_weeklyRunning;
extern std::atomic<unsigned> g_junkGen;
extern std::vector<std::wstring> g_roots;
extern std::atomic<ULONGLONG> g_projAt, g_sysAt;
extern std::set<std::wstring> g_ignored;
extern bool g_ignoredLoaded;
extern SRWLOCK g_watchLock;
extern tools::ProcHistory g_hist;
extern std::vector<tools::ProcInfo> g_procs;
extern std::vector<tools::ProcHistory::Leak> g_leaks;
extern std::vector<tools::StartItem> g_newStarts;
extern std::vector<std::wstring> g_wakeCache;
extern tools::CommitInfo g_commit;
extern double g_netDown, g_netUp;
extern HANDLE g_watchStop, g_watchThread;
extern std::atomic<bool> g_game;
extern bool g_dev;
extern bool g_ddcAsk;
extern ULONGLONG g_devEnded;
extern std::set<std::wstring> g_leakWarned;
extern ULONGLONG g_commitWarned, g_proactiveAt;
// сообщения фоновых потоков основному (WM_TOOLS_JOB, lParam): разбирает OnJob
struct SelfMsg {  // kSelfTag: часовой снимок своей памяти
    std::wstring line, leak;
};
struct LeftMsg {  // kLeftTag: задание, брошенное по «Стоп», доработало
    net::Job job;
    bool ok;
    std::wstring last;
};
struct WeeklyMsg {  // kWeeklyTag: итог недельной проверки
    std::wstring growth, health;
    tools::Driver driver;
    std::vector<tools::ToolVersion> versions;
    bool versionsChecked = false;
};
// видеокарта за игру: собирает поток слежки, итог - карточкой после игры (toolsui_watch.cpp)
struct GpuStats {
    int max = 0, samples = 0, thermal = 0, power = 0;
};
extern GpuStats g_gpu;
std::wstring JobTitle(net::Job j);
void OnWeekly(WeeklyMsg* m);
void WeekNews();            // раз в неделю - новое за неделю в дайджест (toolsui_weekly.cpp)
extern unsigned g_scanGen;  // номер фонового подсчёта: растёт с каждым
extern std::vector<tools::Suspect> g_netAlerts;
extern std::set<std::wstring> g_netKnown;

// ---- общие помощники ----
ThemeInfo Theme();
const wchar_t* Have(const wchar_t* want, const wchar_t* now);
Item Act(const std::wstring& name, const wchar_t* icon, const std::wstring& key, const std::wstring& note);
// stoppable - в карточке кнопка «Стоп», работа смотрит g_jobCancel (долгие задания с правами: RunAdmin(...,
// &g_jobCancel))
void RunJob(const std::wstring& title, std::function<bool(const tools::Log&, std::wstring&)> work, bool quiet = false,
            bool stoppable = false);
std::wstring Short(std::wstring s, size_t n);
void RunClean(const std::wstring& title, const std::vector<tools::Junk>& list);
std::wstring NoExe(const std::wstring& s);
std::wstring Leaf(const std::wstring& p);
std::wstring Days(int d);
std::wstring JunkShort(const std::wstring& n);
std::wstring KnownFolder(REFKNOWNFOLDERID id);
std::wstring Sys32(const wchar_t* name);
void ScanPass(const std::vector<std::wstring>& roots, bool proj, bool sys);
void RefreshScan(bool full = false);
void RefreshSecurity();
std::wstring FindKey(const tools::Suspect& s);
std::wstring IgnoredFile();
std::string ReadSmall(const std::wstring& path);
void WriteSmall(const std::wstring& path, const std::string& s);
const std::set<std::wstring>& Ignored();
bool IsIgnored(const tools::Suspect& s);
void SaveIgnored();
std::vector<tools::Suspect> AllFinds(const tools::SecurityScan& sec);
void OnSecurity();
void Ask(const std::wstring& title, const std::wstring& sub, const std::wstring& yes, const std::wstring& no,
         std::function<void()> onYes, std::function<void()> onNo = nullptr, int hold = 20000,
         Toast::Glyph glyph = Toast::G_NONE);
void SysClean();
std::wstring NetKey(const tools::Suspect& s);
void QuickPass();
DWORD WINAPI WatchLoop(void*);
Watched Watch();
std::vector<tools::ToolVersion> Versions();
void SetVersions(const std::vector<tools::ToolVersion>& v);
tools::HeavyApp AppByName(const std::wstring& name);
void KillLeftovers(const std::vector<tools::Leftover>& tails);
void OpenPagefile();
void RebootSoon();
int BootMinute();
void TryProactive();
int Today();
int LastDay(const std::wstring& what);
void SetDay(const std::wstring& what, int day);
void Weekly();
std::vector<std::wstring> ProjectRoots(const Config& cfg);
void Init(HINSTANCE inst, HWND app, const Config* cfg, const BoostCard* below,
          std::function<void(int, const std::wstring&)> openStudio);
void OnModes(bool game, bool dev);
void CloseHeavy(const tools::HeavyApp& a);
bool MemoryOffer(const std::wstring& why, const std::vector<std::wstring>& keep, bool force);
void OnJob(WPARAM wp, LPARAM lp);
void Tick();
void Shutdown();
State Snapshot();
void Refresh();
void JunkCleaned(const std::vector<std::pair<std::wstring, ULONGLONG>>& freed);
void SystemClean();
std::wstring BusyNames(const std::vector<std::wstring>& files);
bool CleanClaim();
void CleanRelease();
bool CleanBusy();
ULONGLONG CleanList(const std::vector<tools::Junk>& list, const tools::Log& log, std::wstring* lockers);
void PrimeForSnapshot(const Config* cfg);
bool Busy();
void Demo(int what);
void TrimDdc();
SecurityView SecurityFinds();
void SecurityRescan();
void PrimeSecurityForSnapshot();
void SecurityAct(const tools::Suspect& s);
void IgnoreFind(const tools::Suspect& s);
void ClearIgnored();
const wchar_t* FindKindName(tools::FindKind k);
const wchar_t* FindKindIcon(tools::FindKind k);
void CleanProjectList(const std::vector<tools::ProjectJunk>& list);
void GameGpuSummary();
std::wstring Level(int score);
const wchar_t* FindWhat(tools::FindKind k);
const wchar_t* FindIcon(tools::FindKind k);
std::wstring FtText(ULONGLONG ft, bool withTime = true);
ULONGLONG FtNow();
void CopyText(const std::wstring& s);
void OpenInSandbox(const std::wstring& file);
void OpenVirusTotal(const std::wstring& path);
void SecurityAction(const tools::Suspect& s);
void TurnOffStart(const tools::StartItem& it);  // выключить запись, если она всё та же (сверка со свежим списком)
void GuardStart();                              // сторожа процессов (toolsui_guard.cpp): поток раз в 5 с
void GuardStop();
void GuardTick();  // из Tick: настройки для потока, журнал и карточки из очереди
Item SecurityItem();
Item RecoveryItem();
Item SystemItem(const Scan& scan);
Item GitItem(const Scan& scan);
void ShowRing(RadialMenu& menu);

}  // namespace toolsui
