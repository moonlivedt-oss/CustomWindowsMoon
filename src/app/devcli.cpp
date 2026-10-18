#include "app/devcli.h"
#include "app/autostart.h"
#include "app/toolsui.h"

#include <atomic>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "core/bg.h"
#include "tools/mft_detail.h"
#include "core/config.h"
#include "core/elevate.h"
#include "core/filedlg.h"
#include "core/files.h"
#include "core/text.h"
#include "core/themes.h"
#include "desk/boost.h"
#include "desk/gamemode.h"
#include "desk/lockscreen.h"
#include "desk/gameboost.h"
#include "desk/minecraft.h"
#include "desk/modefx.h"
#include "desk/ocr.h"
#include "desk/startbg.h"
#include "desk/whreg.h"
#include "gfx/color.h"
#include "core/perf.h"
#include "gfx/scene.h"
#include "menu/menu.h"
#include "net/net.h"
#include "studio/studio.h"
#include "tools/tools.h"

namespace devcli {

namespace {

std::vector<std::wstring> Args() {
    std::vector<std::wstring> out;
    int n = 0;
    if (LPWSTR* v = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; ++i) out.push_back(v[i]);  // [0] - сам exe
        LocalFree(v);
    }
    return out;
}

// Значение после флага: «--ocr картинка.png» -> «картинка.png».
bool Value(const std::vector<std::wstring>& args, const wchar_t* flag, std::wstring& out) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag) {
            out = args[i + 1];
            return true;
        }
    return false;
}

bool Has(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args)
        if (a == flag) return true;
    return false;
}

// Итог проверки - в файл рядом с exe (ocr_out.txt, lock_out.txt, game_out.txt).
void WriteOut(const wchar_t* name, const std::wstring& text) {
    std::string bytes = Narrow(text);
    HANDLE f = CreateFileW((ExeDir() + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    CloseHandle(f);
}

Config LoadOwnConfig() {
    Config c;
    LoadConfig(ExeDir() + L"\\radial.cfg", c);
    return c;
}

// GDI+ на время одного режима.
struct Gdip {
    ULONG_PTR token = 0;
    Gdip() {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
    }
    ~Gdip() { Gdiplus::GdiplusShutdown(token); }
};

// --ocr картинка.png - распознать текст в ocr_out.txt
int Ocr(const std::wstring& path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> png;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        png.resize(GetFileSize(f, nullptr));
        DWORD got = 0;
        ReadFile(f, png.data(), (DWORD)png.size(), &got, nullptr);
        png.resize(got);
        CloseHandle(f);
    }
    std::wstring err, text = ocr::Recognize(png, &err);
    WriteOut(L"ocr_out.txt", L"error=[" + err + L"]\r\n" + text);
    return 0;
}

// --dlg путь - перевести открытый диалог «Открыть/Сохранить» в путь
int Dialog(const std::wstring& path) {
    HWND dlg = nullptr;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(h) || !filedlg::Is(h)) return TRUE;
            *reinterpret_cast<HWND*>(lp) = h;
            return FALSE;
        },
        (LPARAM)&dlg);
    if (!dlg || !filedlg::Go(dlg, path, (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0)) return 1;
    ULONGLONG until = GetTickCount64() + 1500;  // таймер возврата имени
    MSG m;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(10);
    }
    return 0;
}

// --explorer-reg / --notify-reg / --settings-reg / --search-reg / --all-reg - собрать .reg по теме (не применяя)
int Reg(const std::vector<std::wstring>& args) {
    ThemeInfo t = CurrentTheme(LoadOwnConfig());
    std::wstring out;
    if (Has(args, L"--explorer-reg")) out = whreg::ExplorerReg(t.bg, t.ac);
    if (Has(args, L"--notify-reg")) out = whreg::NotificationReg(t.bg, t.ac);
    if (Has(args, L"--settings-reg")) out = whreg::SettingsReg(t.bg, t.panel, t.ac);
    if (Has(args, L"--search-reg")) out = whreg::SearchReg(t.bg, t.ac);
    if (Has(args, L"--all-reg")) out = whreg::AllReg(t.bg, t.panel, t.ac, nullptr);
    return out.empty() ? 1 : 0;
}

// --lock-apply - собрать экран блокировки по теме и поставить, итог в lock_out.txt
int LockApply() {
    Gdip gdip;
    auto job = lockscreen::ApplyAsync(LoadOwnConfig(), nullptr);
    while (!job->done) Sleep(20);
    WriteOut(L"lock_out.txt", job->message);
    return job->ok ? 0 : 1;
}

// --game-test - игровой режим на 3 секунды, состояние системы до, во время и после - в game_out.txt
int GameTest() {
    auto state = [] {
        BOOL anim = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
        DWORD tr = 1, sz = sizeof(tr);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &tr, &sz);
        return L"anim=" + std::to_wstring(anim) + L" transparency=" + std::to_wstring(tr) + L"\r\n";
    };
    Config c = LoadOwnConfig();
    GameMode gm;
    std::wstring log = L"before: " + state();
    gm.SetManual(true, c);
    log += L"during: " + state();
    Sleep(3000);
    gm.SetManual(false, c);
    log += L"after: " + state();
    WriteOut(L"game_out.txt", log);
    return 0;
}

// Проверка сети без окна: отчёт в net_out.txt (то же, что раздел «Сеть» в студии)
int NetDiag() {
    net::Report r = net::Diagnose(true);
    static const wchar_t* kLevel[] = {L"ok  ", L"info", L"WARN", L"BAD "};
    const std::wstring nl = L"\r\n";
    std::wstring s = net::Summary(r) + nl + nl;
    for (const net::Check& c : r.checks) s += std::wstring(kLevel[c.level]) + L"  " + c.title + L": " + c.detail + nl;
    s += nl + L"Совет: " + (r.fixLabel.empty() ? std::wstring(L"нет") : r.fixLabel + L" - " + r.fixReason) + nl;
    s += nl + L"Прокси-процессы:" + nl;
    for (const net::ProxyProc& p : net::ProxyProcesses()) {
        s += L"  " + p.name + L" pid " + std::to_wstring(p.pid) + L" порты";
        for (int port : p.ports) s += L" " + std::to_wstring(port);
        s += nl;
    }
    s += nl + L"DNS:" + nl;
    for (const net::DnsResult& d : net::TestDns())
        s += L"  " + d.name + L" " + d.ip + L": " + std::to_wstring(d.ms) + L" мс" + nl;
    s += nl + L"Задача с правами: " + std::wstring(net::AdminReady() ? L"есть" : L"нет") + nl;
    WriteOut(L"net_out.txt", s);
    return 0;
}

// Утилиты без окна: отчёт в tools_out.txt
int ToolsDiag() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // Проводник и ярлыки - через COM
    const std::wstring nl = L"\r\n";
    std::wstring s = L"PATH:" + nl;
    for (const tools::PathEntry& e : tools::ReadPath())
        s += std::wstring(e.system ? L"  sys " : L"  usr ") +
             (e.dead  ? L"DEAD "
              : e.dup ? L"DUP  "
                      : L"     ") +
             e.raw + (e.provides.empty() ? L"" : L"   <- " + e.provides) + nl;
    s += nl + L"Что найдётся первым:" + nl;
    for (const tools::Which& w : tools::WhichAll()) {
        s += L"  " + w.cmd + L" = " + w.path;
        for (const std::wstring& o : w.other) s += L" | затенён: " + o;
        s += nl;
    }
    s += nl + L"Тома:" + nl;
    for (const tools::Volume& v : tools::Volumes())
        s += L"  " + v.root + L" " + v.label + L" " + v.fs + L": свободно " + tools::Bytes(v.free) + L" из " +
             tools::Bytes(v.total) + nl;
    s += nl + L"Диски:" + nl;
    for (const tools::Physical& d : tools::PhysicalDisks())
        s += L"  " + d.name + (d.ssd ? L" SSD" : L" HDD") + L" здоровье " + std::to_wstring(d.health) + L" " +
             tools::Bytes(d.size) + nl;
    s += nl + L"Кэши:" + nl;
    for (const tools::Junk& j : tools::FindJunk())
        s += L"  " + j.name + L" " + tools::Bytes(j.bytes) + L" " + j.path + nl;
    s += nl + L"Автозагрузка:" + nl;
    for (const tools::StartItem& it : tools::StartupItems())
        s += std::wstring(it.enabled ? L"  on  " : L"  off ") + (it.canToggle ? L"   " : L"[A]") + it.where + L": " +
             it.name + L" = " + it.command + nl;
    s += nl + L"Порты:" + nl;
    for (const tools::Listener& l : tools::Listeners())
        if (l.dev || !l.udp)
            s += std::wstring(l.dev ? L"  * " : L"    ") + (l.udp ? L"udp " : L"tcp ") + std::to_wstring(l.port) +
                 L" " + l.process + L" pid " + std::to_wstring(l.pid) + nl;
    s += nl + L"Кто держит RadialMenu.exe:" + nl;
    for (const tools::Locker& k : tools::WhoLocks(ExeDir() + L"\\RadialMenu.exe"))
        s += L"  " + k.name + L" pid " + std::to_wstring(k.pid) + nl;
    s += nl + L"Кто держит папку exe:" + nl;
    for (const tools::Locker& k : tools::WhoLocks(ExeDir()))
        s += L"  " + k.name + L" pid " + std::to_wstring(k.pid) + nl;
    s += nl + L"Тяжёлые программы (личная память):" + nl;
    for (const tools::HeavyApp& a : tools::HeavyApps(10))
        s += L"  " + a.name + L" " + tools::Bytes(a.bytes) + L", процессов " + std::to_wstring(a.pids.size()) + nl;
    s += nl + L"Кэши:" + nl;
    for (const tools::Junk& j : tools::FindJunk(true))
        s += std::wstring(j.slow ? L"  slow " : L"       ") + j.name + L" " + tools::Bytes(j.bytes) + nl;
    s += L"  Unreal DDC, не открывалось 30 дней: " + tools::Bytes(tools::OldBytes(tools::DdcDir(), 30)) + nl;
    Config cfg = LoadOwnConfig();
    std::vector<std::wstring> roots = toolsui::ProjectRoots(cfg);
    s += nl + L"Корни проектов:" + nl;
    for (const std::wstring& r : roots) s += L"  " + r + nl;
    s += nl + L"Мусор проектов:" + nl;
    for (const tools::ProjectJunk& p : tools::FindProjectJunk(roots))
        s += L"  " + tools::Bytes(p.bytes) + L"  " + p.kind + L"  " + p.path + L"  (правок " +
             std::to_wstring(p.idleDays) + L" дн. назад)" + nl;
    s += nl + L"Конфликты версий в PATH:" + nl;
    for (const tools::Conflict& c : tools::PathConflicts())
        s += L"  " + c.cmd + L": " + c.winnerVer + L" " + c.winner + L"  затеняет  " + c.loserVer + L" " + c.loser +
             (c.fixable ? L"  [можно поднять]" : L"  [системный]") + nl;
    s += nl + L"Порты разработки и проекты:" + nl;
    std::vector<tools::Listener> ls = tools::Listeners();
    tools::ListenerProjects(ls);
    for (const tools::Listener& l : ls)
        if (l.dev) s += L"  " + std::to_wstring(l.port) + L" " + l.process + L" [" + l.project + L"] " + l.command + nl;
    s += nl + L"Автозагрузка с весом:" + nl;
    std::vector<tools::StartItem> st = tools::StartupItems();
    tools::StartupWeight(st);
    for (const tools::StartItem& it : st)
        s += std::wstring(it.enabled ? L"  on  " : L"  off ") + it.name + (it.running ? L"  работает, " : L"  -, ") +
             tools::Bytes(it.memory) + nl;
    s += nl + L"Память: свободно " + tools::Bytes(tools::Memory().avail) + L" из " +
         tools::Bytes(tools::Memory().total) + nl;
    tools::TermEnv env = tools::DetectEnv(ExeDir());
    if (env.what.empty()) env = tools::DetectEnv(ExeDir().substr(0, ExeDir().find_last_of(L'\\')));  // из build-check
    s += nl + L"Окружение для " + env.dir + L":";
    for (const std::wstring& w : env.what) s += L" " + w + L";";
    s += nl + L"  " + env.setup + nl;
    s += nl + L"Проводник сейчас: " + tools::ExplorerFolder() + nl;
    tools::Driver d = tools::NvidiaDriver();
    s += nl + L"Драйвер: " + d.gpu + L" установлен " + d.installed + L", новейший " + d.latest + L" (" + d.date + L")" +
         (d.newer ? L" - есть новее" : L"") + nl;
    s += nl + L"Рост за неделю:" + nl;
    for (const tools::Growth& g : tools::WeekGrowth(7))
        s += L"  " + g.name + L" " + std::to_wstring(g.delta / 1048576) + L" МБ" + nl;
    WriteOut(L"tools_out.txt", s);
    return 0;
}

// «Защита» без окна: отчёт в sec_out.txt - все находки со счётом, время проверки (первый и второй раз - с кэшем).
int SecDiag() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring nl = L"\r\n";
    ULONGLONG t0 = GetTickCount64();
    tools::SecurityScan r = tools::ScanSecurity();
    ULONGLONG t1 = GetTickCount64();
    tools::ScanSecurity();
    ULONGLONG t2 = GetTickCount64();
    std::wstring s = L"Проверено процессов " + std::to_wstring(r.processes) + L", записей автозапуска " +
                     std::to_wstring(r.autoruns) + L", без подписи (разных файлов) " +
                     std::to_wstring(r.unsignedCount) + L"; первый раз " + std::to_wstring(t1 - t0) + L" мс, из кэша " +
                     std::to_wstring(t2 - t1) + L" мс" + nl;
    auto dump = [&](const wchar_t* title, const std::vector<tools::Suspect>& v) {
        s += nl + title + L" (" + std::to_wstring(v.size()) + L"):" + nl;
        for (const tools::Suspect& x : v)
            s += L"  [" + std::to_wstring(x.score) + L"] " + x.name +
                 (x.pid ? L" pid " + std::to_wstring(x.pid) : L"") + L" | " + x.path + L" | " + x.why +
                 (x.signer.empty() ? L"" : L" | подпись " + x.signer) + (x.where.empty() ? L"" : L" | " + x.where) + nl;
    };
    dump(L"Реестр и система: PATH, службы, WMI, свои exe", r.hijacks);
    dump(L"Процессы", r.procs);
    dump(L"Автозапуск", r.autos);
    dump((L"Задачи планировщика, проверено " + std::to_wstring(r.tasksChecked)).c_str(), r.tasks);
    dump((L"Расширения браузеров, проверено " + std::to_wstring(r.extsChecked)).c_str(), r.exts);
    dump(L"Сеть: hosts, DNS, прокси", r.nets);
    // подписи всего запущенного: что признано подписанным и кем (проверка ложных срабатываний)
    s += nl + L"Подписи запущенных программ:" + nl;
    std::set<std::wstring> seen;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t p[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        bool got = QueryFullProcessImageNameW(h, 0, p, &n);
        CloseHandle(h);
        if (!got || !seen.insert(Lower(std::wstring(p, n))).second) continue;
        std::wstring signer;
        tools::SignState st = tools::CheckSign(std::wstring(p, n), &signer);
        static const wchar_t* kState[] = {L"нет файла", L"ok", L"НЕТ ПОДПИСИ", L"недоверенная", L"ИСПОРЧЕНА"};
        s += L"  " + std::wstring(kState[st]) + L"  " + std::wstring(p, n) +
             (signer.empty() ? L"" : L"  <" + signer + L">") +
             (tools::SystemImposter(std::wstring(p, n)) ? L"  САМОЗВАНЕЦ" : L"") + nl;
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    s += nl + L"Программы из папки защиты:" + nl;
    for (const tools::SecurityApp& a : tools::SecurityApps(LoadOwnConfig().securityDir))
        s += L"  " + a.name + L" = " + a.exe + nl;
    s += L"Антивирус: " + tools::AntivirusUi() + nl;
    s += L"SHA-256 RadialMenu.exe: " + tools::FileSha256(ExeDir() + L"\\RadialMenu.exe") + nl;
    WriteOut(L"sec_out.txt", s);
    return 0;
}

// Слежка, батарея, память, версии, дубли без окна: отчёт в watch_out.txt (версии и дубли - с --slow)
int WatchDiag(bool slow) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring nl = L"\r\n";
    std::wstring s;
    ULONGLONG t0 = GetTickCount64();
    std::vector<tools::ProcInfo> ps = tools::Processes();
    s += L"Процессов этого входа: " + std::to_wstring(ps.size()) + L" за " + std::to_wstring(GetTickCount64() - t0) +
         L" мс" + nl;
    tools::ProcHistory hist;
    hist.Feed(ps, GetTickCount64());
    Sleep(3000);
    hist.Feed(tools::Processes(), GetTickCount64());
    s += nl + L"Процессор за 3 с (все программы):" + nl;
    for (const tools::ProcHistory::Hog& h : hist.CpuHogs(1, false))
        s += L"  " + std::to_wstring((int)(h.percent * 10) / 10.0).substr(0, 4) + L"%  " + h.name +
             (h.window ? L"" : L"  [фон]") + nl;
    s += nl + L"Хвосты сборки:" + nl;
    for (const tools::Leftover& l : tools::BuildLeftovers(ps))
        s += L"  " + l.name + L" pid " + std::to_wstring(l.pid) + L" " + tools::Bytes(l.mem) + L" - " + l.why + nl;
    tools::CommitInfo c = tools::Commit();
    s += nl + L"Выделено " + tools::Bytes(c.total) + L" из " + tools::Bytes(c.limit) + L", пик " +
         tools::Bytes(c.peak) + L", память " + tools::Bytes(c.phys) + L", подкачка ~" +
         tools::Bytes(tools::PagefileBytes()) + nl;
    tools::BatteryInfo b = tools::Battery();
    s += nl + L"Батарея: есть " + std::to_wstring(b.present) + L", сеть " + std::to_wstring(b.ac) + L", заряд " +
         std::to_wstring(b.percent) + L"%, осталось " + std::to_wstring(b.minutesLeft) + L" мин, паспорт " +
         std::to_wstring(b.designed) + L" мВт*ч, сейчас " + std::to_wstring(b.full) + L", циклов " +
         std::to_wstring(b.cycles) + nl;
    s += L"Будить могут:";
    for (const std::wstring& w : tools::WakeDevices()) s += L" [" + w + L"]";
    s += nl;
    ULONGLONG in0, out0, in1, out1;
    tools::NetCounters(in0, out0);
    Sleep(2000);
    tools::NetCounters(in1, out1);
    s += L"Сеть (физические адаптеры): приём " + tools::Bytes((in1 - in0) / 2) + L"/с, отдача " +
         tools::Bytes((out1 - out0) / 2) + L"/с, всего принято " + tools::Bytes(in1) + nl;
    s += nl + L"Новый автозапуск с прошлого раза:" + nl;
    for (const tools::StartItem& it : tools::NewStartupItems()) s += L"  " + it.where + L": " + it.name + nl;
    s += L"Право на задачу с правами: " + std::wstring(net::AdminReady() ? L"есть" : L"нет") + nl;
    if (slow) {
        t0 = GetTickCount64();
        s += nl + L"Версии:" + nl;
        for (const tools::ToolVersion& v : tools::ToolVersions())
            s += L"  " + v.name + L" " + v.installed + L" (" + v.path + L")" + (v.newer ? L"  -> " + v.note : L"") +
                 (v.train.empty() ? L"" : L"  поезд " + std::to_wstring(v.train.size())) +
                 (v.eol.empty() ? L"" : L"  " + v.eol) + nl;
        s += L"  за " + std::to_wstring(GetTickCount64() - t0) + L" мс" + nl;
        t0 = GetTickCount64();
        std::atomic<long> seen{0};
        std::vector<tools::DupGroup> d = tools::FindDuplicates(tools::DupRoots(), 20ull * 1024 * 1024, nullptr, &seen);
        s += nl + L"Дубли от 20 МБ (" + std::to_wstring(seen.load()) + L" файлов просмотрено, " +
             std::to_wstring(GetTickCount64() - t0) + L" мс):" + nl;
        for (const tools::DupGroup& g : d) {
            s += L"  " + tools::Bytes(g.size) + L" x" + std::to_wstring(g.paths.size()) + nl;
            for (const std::wstring& p : g.paths) s += L"      " + p + nl;
        }
    }
    WriteOut(L"watch_out.txt", s);
    return 0;
}

// Новые утилиты без окна: восстановление, приватность, система, разработка -> sys_out.txt
int SysDiag() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring nl = L"\r\n";
    std::wstring s;
    auto t0 = GetTickCount64();
    auto lap = [&](const wchar_t* what) {
        s += std::wstring(L"  (") + what + L": " + std::to_wstring(GetTickCount64() - t0) + L" мс)" + nl;
        t0 = GetTickCount64();
    };
    s += L"Сломано вирусами:" + nl;
    for (const tools::Broken& b : tools::FindBroken())
        s += L"  " + b.what + L" | " + b.where + L" = " + b.value + (b.admin ? L" [права]" : L"") + nl;
    lap(L"FindBroken");
    s += L"Ждёт перезагрузки:";
    for (const std::wstring& w : tools::PendingReboot()) s += L" " + w + L";";
    s += nl;
    s += nl + L"Камера, микрофон, геопозиция:" + nl;
    for (const tools::PrivacyUse& u : tools::PrivacyUses())
        s += L"  " + u.capability + L" | " + u.app + (u.inUse ? L" | СЕЙЧАС" : L"") +
             (u.unsignedApp ? L" | без подписи" : L"") + L" | " + u.path + nl;
    lap(L"PrivacyUses");
    std::vector<tools::Suspect> f;
    tools::CheckCerts(f);
    lap(L"CheckCerts");
    tools::CheckFirewall(f);
    lap(L"CheckFirewall");
    s += nl + L"Сертификаты и брандмауэр:" + nl;
    for (const tools::Suspect& x : f)
        s += L"  [" + std::to_wstring(x.score) + L"] " + x.name + L" | " + x.path + L" | " + x.why + nl;
    s += nl + L"Устройства с ошибками:" + nl;
    for (const tools::DeviceProblem& p : tools::DeviceProblems()) s += L"  " + p.name + L": " + p.text + nl;
    lap(L"DeviceProblems");
    tools::GpuSample g = tools::GpuNow();
    s += L"Видеокарта: " + std::wstring(g.ok ? L"" : L"нет данных ") + std::to_wstring(g.temp) + L" C, нагрузка " +
         std::to_wstring(g.load) + L"%, " + std::to_wstring((int)g.watts) + L" Вт" + (g.thermal ? L", ПЕРЕГРЕВ" : L"") +
         nl;
    lap(L"GpuNow");
    tools::BatteryTrend bt = tools::BatteryHistory();
    s += L"Батарея: паспорт " + std::to_wstring(bt.designed) + L", сейчас " + std::to_wstring(bt.now) + L", записей " +
         std::to_wstring(bt.points.size()) + nl;
    tools::UpdateInfo u = tools::WindowsUpdates();
    s += L"Обновления: прочитано " + std::to_wstring(u.ok) + L", последнее: " + u.lastTitle + L", ошибок " +
         std::to_wstring(u.failed.size()) + nl;
    lap(L"WindowsUpdates");
    s += nl + L"Git:" + nl;
    Config cfg = LoadOwnConfig();
    std::vector<std::wstring> roots = toolsui::ProjectRoots(cfg);
    for (const tools::GitRepo& r : tools::GitRepos(roots))
        s += L"  " + r.name + L" [" + r.branch + L"] изменений " + std::to_wstring(r.changed) + L", не отправлено " +
             std::to_wstring(r.ahead) + (r.noUpstream ? L", без удалённой ветки" : L"") + L" | " + r.path + nl;
    lap(L"GitRepos");
    s += nl + L"Диски WSL и Docker:" + nl;
    for (const tools::VirtualDisk& v : tools::VirtualDisks())
        s += L"  " + v.name + L" " + tools::Bytes(v.bytes) + L" | " + v.path + nl;
    s += nl + L"Хвосты удалённых программ:" + nl;
    for (const tools::Leftover2& l : tools::AppLeftovers())
        s += L"  " + tools::Bytes(l.bytes) + L" | " + l.name + L" | " + std::to_wstring(l.idleDays) + L" дн. | " +
             l.path + nl;
    lap(L"AppLeftovers");
    WriteOut(L"sys_out.txt", s);
    return 0;
}

// Самопроверка логики слежки на выдуманных данных: утечка, скачок, фоновый процессор, хвосты сборки.
// Итог в selftest_out.txt, код выхода - число провалов.
int WatchSelftest() {
    std::wstring s;
    int fails = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        s += (ok ? L"ok    " : L"FAIL  ") + what + L"\r\n";
        fails += !ok;
    };
    const ULONGLONG MB = 1024ull * 1024, MIN = 60000;
    tools::ProcHistory h;
    // 100: ровная утечка 400 МБ -> 2 ГБ за 80 минут; 200: стоит на 3 ГБ; 300: скачок в конце (загрузка уровня);
    // 400: процессор 25% одного ядра без окна; 500: с окном и 50%
    for (int m = 0; m <= 80; ++m) {
        std::vector<tools::ProcInfo> ps;
        tools::ProcInfo a;
        a.pid = 100, a.name = L"leaky.exe", a.created = 1, a.mem = 400 * MB + (ULONGLONG)m * 20 * MB;
        tools::ProcInfo b = a;
        b.pid = 200, b.name = L"steady.exe", b.mem = 3000 * MB;
        tools::ProcInfo c = a;
        c.pid = 300, c.name = L"jump.exe", c.mem = m < 78 ? 500 * MB + m * 3 * MB : 2500 * MB - (m - 78) * 200 * MB;
        tools::ProcInfo d = a;
        d.pid = 400, d.name = L"busy.exe", d.mem = 50 * MB, d.cpu = (ULONGLONG)m * 60 * 10000000ull / 4;
        tools::ProcInfo e = a;
        e.pid = 500, e.name = L"game.exe", e.mem = 50 * MB, e.cpu = (ULONGLONG)m * 60 * 10000000ull / 2,
        e.window = true;
        ps = {a, b, c, d, e};
        h.Feed(ps, 1000000 + m * MIN);
    }
    std::vector<tools::ProcHistory::Leak> leaks = h.Leaks();
    check(leaks.size() == 1 && leaks[0].pid == 100,
          L"утечка найдена только у leaky.exe (найдено " + std::to_wstring(leaks.size()) + L")");
    if (!leaks.empty())
        check(leaks[0].minutes >= 55 && leaks[0].minutes <= 90,
              L"окно сравнения 55-90 минут: " + std::to_wstring(leaks[0].minutes));
    check(h.Ready(10) && h.Ready(80), L"история готова");
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<tools::ProcHistory::Hog> bg = h.CpuHogs(10, true), all = h.CpuHogs(10, false);
    double want = 25.0 / si.dwNumberOfProcessors;
    check(bg.size() == 1 && bg[0].name == L"busy.exe", L"фоновый: только busy.exe");
    if (!bg.empty())
        check(bg[0].percent > want * 0.9 && bg[0].percent < want * 1.1,
              L"доля процессора " + std::to_wstring(bg[0].percent) + L" при ожидаемой " + std::to_wstring(want));
    check(all.size() == 2 && all[0].name == L"game.exe", L"все: game.exe первым");
    // процесс пропал - история по нему уходит
    h.Feed({}, 1000000 + 81 * MIN);
    check(h.Leaks().empty() && h.CpuHogs(10, false).empty(), L"закрытые процессы забываются");

    // хвосты: воркер Unreal без редактора, сирота-компилятор, node с живым родителем, node с окном
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime, hour = 36000000000ull;
    auto P = [&](DWORD pid, DWORD parent, const wchar_t* name, ULONGLONG created, bool win = false) {
        tools::ProcInfo p;
        p.pid = pid, p.parent = parent, p.name = name, p.created = created, p.window = win;
        return p;
    };
    std::vector<tools::ProcInfo> ps = {
        P(10, 1, L"explorer.exe", now - 5 * hour),
        P(11, 10, L"ShaderCompileWorker.exe", now - hour),
        P(12, 999, L"cl.exe", now - hour),          // родителя нет
        P(13, 10, L"node.exe", now - hour),         // родитель жив
        P(14, 999, L"node.exe", now - hour, true),  // сирота, но с окном
        P(15, 16, L"clangd.exe", now - hour),       // номер родителя достался более новому процессу
        P(16, 10, L"notepad.exe", now - 60000000ull),
        P(17, 999, L"ninja.exe", now - 60000000ull),  // сирота, но работает меньше 10 минут
    };
    std::vector<tools::Leftover> t = tools::BuildLeftovers(ps);
    std::wstring names;
    for (const tools::Leftover& l : t) names += l.name + L" ";
    check(t.size() == 3, L"хвостов 3: " + names);
    ps.push_back(P(18, 10, L"UnrealEditor.exe", now - hour, true));
    t = tools::BuildLeftovers(ps);
    names.clear();
    for (const tools::Leftover& l : t) names += l.name + L" ";
    check(t.size() == 2 && names.find(L"ShaderCompileWorker") == std::wstring::npos,
          L"с открытым редактором воркер не хвост: " + names);
    check(tools::SystemImposter(L"C:\\Users\\x\\AppData\\svchost.exe") &&
              !tools::SystemImposter(L"C:\\Windows\\System32\\svchost.exe") &&
              !tools::SystemImposter(L"C:\\Windows\\explorer.exe") && !tools::SystemImposter(L"D:\\x\\notepad.exe"),
          L"самозванцы по пути");
    WriteOut(L"selftest_out.txt", s);
    return fails;
}

// Выдуманная чистка для снимков студии (RADIAL_SCENE=clean): четыре места по очереди, как настоящая работа.
DWORD WINAPI FakeClean(void*) {
    const ULONGLONG GB = 1024ull * 1024 * 1024;
    std::vector<scene::CleanItem> items = {{L"Кэш npm", 7600 * GB / 1000},
                                           {L"Кэш pip", 5 * GB},
                                           {L"TEMP старше суток", 4600 * GB / 1000},
                                           {L"Кэш uv", 900 * GB / 1000}};
    Sleep(300);
    scene::CleanBegin(items);
    for (size_t i = 0; i < items.size(); ++i) {
        Sleep(150);
        scene::CleanMark(i, 1);
        Sleep(750);
        scene::CleanMark(i, 2, items[i].bytes * 97 / 100);
    }
    scene::CleanEnd();
    return 0;
}

// Лист кадров живой сцены на выдуманных данных (как в карточке, 384 dip): --scene-film clean|guard|guard0 out.png.
// Лента ведётся по времени, как настоящая работа; кадры снимаются в заданные мгновения.
int SceneFilm(const std::wstring& what, const std::wstring& out) {
    using namespace Gdiplus;
    ThemeInfo th = CurrentTheme(LoadOwnConfig());
    scene::Palette p;
    p.text = ToColor(th.fg);
    p.muted = ToColor(th.muted);
    p.faint = MixColor(p.muted, ToColor(th.bg), 0.45);
    p.accent = ToColor(th.ac);
    p.accent2 = ToColor(th.glow);
    p.gold = ToColor(th.gold);
    p.card = ToColor(th.panel);
    bool clean = what == L"clean";
    const ULONGLONG GB = 1024ull * 1024 * 1024;
    std::vector<scene::CleanItem> items = {{L"Кэш npm", 7600 * GB / 1000},
                                           {L"Кэш pip", 5 * GB},
                                           {L"TEMP старше суток", 4600 * GB / 1000},
                                           {L"Кэш uv", 900 * GB / 1000}};
    std::vector<scene::GuardFind> finds;
    if (what == L"guard")
        finds = {{L"scvhost.exe", L"имя как у svchost, а лежит в TEMP; без подписи", 7},
                 {L"wscript.exe", L"запуск скрипта при входе из Run", 4}};
    std::vector<double> at = clean ? std::vector<double>{0.35, 0.8, 1.3, 1.9, 2.6, 3.4, 4.1, 4.6, 5.2, 6.4}
                                   : std::vector<double>{0.3, 0.9, 1.6, 2.1, 2.4, 2.8, 3.3, 4.2};
    float k = 1.25f, W = 352 * k;
    float H = clean ? scene::CleanScene::Height(items.size(), k) : scene::GuardScene::Height(finds.size(), k);
    int cols = 2, rows = (int)(at.size() + 1) / 2;
    int cw = (int)(W + 40 * k), ch = (int)(H + 40 * k);
    Bitmap sheet(cw * cols, ch * rows, PixelFormat32bppARGB);
    Graphics sg(&sheet);
    sg.Clear(ToColor(th.bg));
    Bitmap frame((int)W, (int)H, PixelFormat32bppARGB);
    scene::CleanScene cs;
    scene::GuardScene gsn;
    if (clean)
        scene::CleanBegin(items);
    else
        scene::GuardBegin(140);
    ULONGLONG t0 = GetTickCount64();
    size_t next = 0;
    bool ended = false;
    while (next < at.size()) {
        double t = (GetTickCount64() - t0) / 1000.0;
        if (clean) {
            for (size_t i = 0; i < items.size(); ++i) {
                double s = 0.4 + i * 0.9;
                if (t > s + 0.75)
                    scene::CleanMark(i, 2, items[i].bytes * 97 / 100);
                else if (t > s)
                    scene::CleanMark(i, 1);
            }
            if (!ended && t > 0.4 + items.size() * 0.9) ended = true, scene::CleanEnd();
        } else if (!ended && t > 1.8) {
            ended = true;
            scene::GuardEnd(140, finds);
        }
        Graphics g(&frame);
        g.Clear(ToColor(th.bg));
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        POINT m{-100, -100};
        if (clean)
            cs.Draw(g, RectF(0, 0, W, H), k, p, m, false);
        else
            gsn.Draw(g, RectF(0, 0, W, H), k, p, m);
        if (t >= at[next]) {
            int x = (int)(next % cols) * cw, y = (int)(next / cols) * ch;
            sg.DrawImage(&frame, x + 20 * k, y + 28 * k);
            SolidBrush lb(p.faint);
            Font f(L"Segoe UI", 11 * k, FontStyleRegular, UnitPixel);
            wchar_t s[32];
            swprintf(s, 32, L"%.1f с", at[next]);
            sg.DrawString(s, -1, &f, PointF(x + 20 * k, y + 6 * k), &lb);
            ++next;
        }
        Sleep(15);
    }
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png"))
            return sheet.Save(out.c_str(), &codecs[i].Clsid, nullptr) == Ok ? 0 : 1;
    return 1;
}

// --ring-bench [мс]: кольцо левого края рисуется в скрытом окне, курсор водится по секторам; время по частям -
// в profile.log, итог - в bench_out.txt. Для сравнения до и после оптимизаций отрисовки.
int RingBench(int ms) {
    SetEnvironmentVariableW(L"RADIAL_PROFILE", L"1");
    OleInitialize(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int code = 1;
    {
        Gdip gdip;
        Config cfg = LoadOwnConfig();
        RadialMenu menu;
        if (menu.Create(GetModuleHandleW(nullptr))) {
            UINT dpi = GetDpiForSystem();
            while (!menu.Prewarm(&cfg, dpi, 1000)) {}
            ULONGLONG t0 = GetTickCount64();
            int frames = menu.Bench(&cfg, dpi, ms);
            WriteOut(L"bench_out.txt", L"кадров " + std::to_wstring(frames) + L" за " +
                                           std::to_wstring(GetTickCount64() - t0) + L" мс, расхождение кэша колец: " +
                                           std::to_wstring(menu.BenchDiff()) + L" пикс.\r\n");
            code = frames > 0 ? 0 : 1;
        }
        perf::Flush();
        // итог записан; окно с целью перетаскивания и кэши значков GDI+ разбирать незачем - замер окончен
        TerminateProcess(GetCurrentProcess(), (UINT)code);
    }
    return code;
}

// Очистка всего, что пересоздаётся быстро (как «Всё сразу» в кольце): кэши шейдеров и Unreal не трогаются.
// Сверка чтения MFT с обычным обходом: те же папки, байты и файлы. Нужна консоль администратора.
// Папки внутри root, куда обход не пускают даже с правами (FindFirstFile - «отказано в доступе»); ссылки не раскрываем.
std::vector<std::wstring> ClosedInside(const std::wstring& root) {
    std::vector<std::wstring> closed, stack{root};
    while (!stack.empty()) {
        std::wstring d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) {
            // папка только что была в списке родителя, а сама не открывается - закрыта. Ни код ошибки, ни атрибуты не
            // годятся: на LogFiles\WMI\RtBackup (только для SYSTEM) Windows не отдаёт даже атрибуты папки
            if (d != root) closed.push_back(d);
            continue;
        }
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            // папка-ссылка: обход её не раскрывает, а если внутри настоящие файлы - MFT их видит
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                closed.push_back(d + L"\\" + n);
            else
                stack.push_back(d + L"\\" + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return closed;
}

// Допуск сверки: открытые на запись файлы растут, пока считаем - 1% и мегабайт
bool Near(ULONGLONG a, ULONGLONG b) { return (a > b ? a - b : b - a) <= std::max(a, b) / 100 + 1024 * 1024; }

// Где именно MFT видит больше обхода: спуск по папкам, пока разница не окажется в файлах самой папки. По пути -
// сколько в папке ссылок (reparse): обход их пропускает, а в таблице MFT они есть.
void Drill(const std::wstring& path, int depth, std::wstring& s) {
    std::vector<tools::FolderSize> mft;
    if (depth > 8 || !tools::MftCached(path, mft, false)) return;
    const std::wstring nl = L"\r\n";
    std::wstring pad(4 + depth * 2, L' ');
    for (const tools::FolderSize& m : mft) {
        ULONGLONG wb = 0;
        int wf = 0, rFiles = 0, rDirs = 0;
        ULONGLONG rBytes = 0;
        bool loose = m.name.front() == L'(';
        if (loose) {  // файлы прямо в папке: обычные - как у обхода, ссылки - отдельно
            WIN32_FIND_DATAW fd;
            HANDLE h =
                FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    bool dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY,
                         rp = fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
                    if (dir && rp) ++rDirs;
                    if (dir) continue;
                    if (rp)
                        ++rFiles, rBytes += sz;
                    else
                        ++wf, wb += sz;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        } else {
            DWORD a = GetFileAttributesW(m.path.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) rDirs = 1;
            wb = tools::DirBytes(m.path, &wf);
        }
        if (m.bytes <= wb + 1024 * 1024 && m.files <= wf) continue;
        s += pad + (loose ? path + L"\\" + m.name : m.path) + L": обход " + tools::Bytes(wb) + L" / " +
             std::to_wstring(wf) + L", MFT " + tools::Bytes(m.bytes) + L" / " + std::to_wstring(m.files);
        if (rFiles) s += L", файлов-ссылок " + std::to_wstring(rFiles) + L" на " + tools::Bytes(rBytes);
        if (rDirs) s += loose ? L", папок-ссылок " + std::to_wstring(rDirs) : std::wstring(L", сама папка - ссылка");
        if (!loose && wf == 0 && m.files > 0) {
            // почему: ответ на список, атрибуты папки и как она выглядит в списке родителя
            WIN32_FIND_DATAW fd;
            HANDLE h =
                FindFirstFileExW((m.path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
            DWORD err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
            int listed = 0;  // сколько файлов отдал список (без папок и точек)
            ULONGLONG listedBytes = 0;
            if (h != INVALID_HANDLE_VALUE) {
                do
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        ++listed, listedBytes += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                while (FindNextFileW(h, &fd));
                err = GetLastError();  // ERROR_NO_MORE_FILES - дочитали
                FindClose(h);
            }
            DWORD attrs = GetFileAttributesW(m.path.c_str());
            DWORD inParent = INVALID_FILE_ATTRIBUTES;
            HANDLE p = FindFirstFileExW(m.path.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
            if (p != INVALID_HANDLE_VALUE) inParent = fd.dwFileAttributes, FindClose(p);
            wchar_t b[200];
            swprintf(b, 200, L" (список: %ls, код %lu, файлов %d на %ls; атрибуты %08lX, в списке родителя %08lX)",
                     h == INVALID_HANDLE_VALUE ? L"ошибка" : L"открылся", err, listed,
                     tools::Bytes(listedBytes).c_str(), attrs, inParent);
            s += L", обходу нет входа" + std::wstring(b);
        }
        s += nl;
        if (!loose) Drill(m.path, depth + 1, s);
    }
}

int MftTest(const std::wstring& dirIn) {
    std::wstring dir = dirIn;
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    std::wstring nl = L"\r\n", s;
    // таблица тома в кэш помощника - по ней и студия, и разбор закрытых папок ниже
    std::wstring admin = tools::MftAdminMain(dir.substr(0, 3));
    std::vector<tools::FolderSize> mft;
    std::wstring note;
    ULONGLONG t0 = GetTickCount64();
    bool ok = tools::MftSizes(dir, mft, note);
    ULONGLONG t1 = GetTickCount64();
    std::vector<tools::FolderSize> walk = tools::ScanSizes(dir, nullptr, nullptr);
    ULONGLONG t2 = GetTickCount64();
    s += L"MFT: " + std::wstring(ok ? L"ok, " : L"ошибка: ") + note + L", " + std::to_wstring(t1 - t0) + L" мс" + nl;
    s += L"обход: " + std::to_wstring(t2 - t1) + L" мс" + nl + nl;
    int bad = ok ? 0 : 1;
    std::map<std::wstring, std::vector<tools::FolderSize>> byParent;  // кэш MftCached по родителю
    for (const tools::FolderSize& w : walk) {
        auto m = std::find_if(mft.begin(), mft.end(), [&](const tools::FolderSize& x) { return x.name == w.name; });
        ULONGLONG mb = m == mft.end() ? 0 : m->bytes;
        int mf = m == mft.end() ? -1 : m->files;
        bool same = m != mft.end() && Near(mb, w.bytes);
        // обходу внутрь не пустили даже с правами (System Volume Information), а MFT видит - это не ошибка MFT
        bool closed = !same && !w.files && m != mft.end() && m->files > 0;
        // MFT больше обхода: ищем вложенные закрытые папки и их размер по таблице - сойдётся, значит дело в них
        std::wstring why;
        if (!same && !closed && m != mft.end() && mb > w.bytes && w.name.front() != L'(') {
            ULONGLONG hidden = 0;
            int hiddenFiles = 0;
            for (const std::wstring& c : ClosedInside(w.path)) {
                std::wstring parent = c.substr(0, c.find_last_of(L'\\')), leaf = c.substr(parent.size() + 1);
                // без сверки: родитель её может не пройти из-за растущих журналов (LogFiles\WMI), а размер закрытой
                // папки в таблице от этого не хуже
                if (!byParent.count(parent)) tools::MftCached(parent, byParent[parent], false);
                for (const tools::FolderSize& f : byParent[parent])
                    if (_wcsicmp(f.name.c_str(), leaf.c_str()) == 0) {
                        hidden += f.bytes, hiddenFiles += f.files;
                        if (why.size() < 160)
                            why += (why.empty() ? L"" : L", ") + c.substr(w.path.size() + 1) + L" " +
                                   tools::Bytes(f.bytes);
                    }
            }
            closed = hidden && Near(mb, w.bytes + hidden);
            if (!why.empty()) why = L"  [закрыто для обхода: " + why + L"]";
        }
        // файлы прямо в папке: число то же, а MFT больше - растущие открытые журналы, сверка студии это пропускает
        if (!same && !closed && m != mft.end() && w.name.front() == L'(' &&
            tools::mft_detail::LooseMatch(mf, mb, w.files, w.bytes)) {
            closed = true;
            why = L"  [растут открытые файлы: в MFT настоящий размер, сверка проходит]";
        }
        if (!same && !closed) ++bad;
        s += std::wstring(same     ? L"  "
                          : closed ? L"~ "
                                   : L"! ") +
             w.name + L": обход " + tools::Bytes(w.bytes) + L" / " + std::to_wstring(w.files) + L", MFT " +
             tools::Bytes(mb) + L" / " + std::to_wstring(mf) + why + nl;
        if (!same && !closed && mb > w.bytes && w.name.front() != L'(') Drill(w.path, 0, s);
    }
    s += nl + L"расхождений: " + std::to_wstring(bad) +
         L" (~ - обходу нет доступа внутрь или в папке растут открытые файлы, MFT видит; остаток в пределах 1% - "
         L"открытые файлы, которые растут: размер "
         L"в списке папки Windows обновляет с запозданием, в MFT он настоящий)" +
         nl;
    // тот же ответ через таблицу в кэше помощника - как его берёт студия; по именам и с тем же допуском
    std::vector<tools::FolderSize> cached;
    bool cacheOk = admin.rfind(L"@mftok", 0) == 0 && tools::MftCached(dir, cached);
    std::wstring diff;
    int diffs = cacheOk && cached.size() != mft.size() ? 1 : 0;
    for (const tools::FolderSize& m : mft) {
        if (!cacheOk) break;
        auto c =
            std::find_if(cached.begin(), cached.end(), [&](const tools::FolderSize& x) { return x.name == m.name; });
        if (c != cached.end() && Near(c->bytes, m.bytes)) continue;
        ++diffs;
        if (diff.size() < 200)
            diff += L"  " + m.name + L": напрямую " + tools::Bytes(m.bytes) + L", из кэша " +
                    (c == cached.end() ? std::wstring(L"нет") : tools::Bytes(c->bytes)) + nl;
    }
    s += L"кэш таблицы: " + Trim(admin) + L", " +
         (!cacheOk ? L"не прочитался"
          : diffs  ? L"НЕ совпадает (" + std::to_wstring(diffs) + L")"
                   : std::wstring(L"совпадает")) +
         nl + diff;
    for (const tools::FolderSize& f : cached)
        if (f.closed) s += L"  закрыта для обхода: " + f.name + L" " + tools::Bytes(f.bytes) + nl;
    if (!cacheOk || diffs) ++bad;
    WriteOut(L"mft_out.txt", s);
    return bad ? 1 : 0;
}

int Clean() {
    std::wstring s;
    const std::wstring nl = L"\r\n";
    ULONGLONG freed = 0;
    for (const tools::Junk& j : tools::FindJunk(true)) {
        if (j.slow) {
            s += L"пропущен (долго собирается заново): " + j.name + L" " + tools::Bytes(j.bytes) + nl;
            continue;
        }
        freed += tools::CleanJunk(j, [&](const std::wstring& line) { s += line + nl; });
    }
    s += L"Всего освобождено: " + tools::Bytes(freed) + nl;
    WriteOut(L"clean_out.txt", s);
    return 0;
}

}  // namespace

bool HasFlag(const wchar_t* flag) { return Has(Args(), flag); }

bool Run(int& exitCode) {
    std::vector<std::wstring> args = Args();
    std::wstring v;
    if (Value(args, L"--ocr", v)) {
        exitCode = Ocr(v);
    } else if (Value(args, L"--dlg", v)) {
        exitCode = Dialog(v);
    } else if (Has(args, L"--explorer-reg") || Has(args, L"--notify-reg") || Has(args, L"--settings-reg") ||
               Has(args, L"--search-reg") || Has(args, L"--all-reg")) {
        exitCode = Reg(args);
    } else if (Has(args, L"--start-apply")) {  // оформить Пуск Start11 по теме, как кнопка студии
        Gdip gdip;
        std::wstring msg;
        exitCode = startbg::Apply(CurrentTheme(LoadOwnConfig()), msg) ? 0 : 1;
    } else if (Has(args, L"--purge")) {  // очистка памяти; запускает задача планировщика с правами
        // какие области чистить - из запроса в HKCU; PurgeMain знает только свои имена, прочее пропускает
        boost::Settings s;
        s.areas = Split(elevate::Take(L"PurgeAreas"), L',');
        s.areas.erase(std::remove(s.areas.begin(), s.areas.end(), std::wstring()), s.areas.end());
        exitCode = boost::PurgeMain(s);
    } else if (Has(args, L"--crash-test")) {  // нарочно упасть: проверить, что дамп и строка ложатся в crash
        *(volatile int*)nullptr = 0;
        exitCode = 1;
    } else if (Has(args, L"--net-job")) {  // действие с правами для раздела «Сеть»; запускает задача планировщика
        exitCode = net::AdminMain();
    } else if (Has(args, L"--studio-snap")) {  // --studio-snap раздел вкладка файл.png [мс]: раздел студии в картинку
        Gdip gdip;
        size_t i = std::find(args.begin(), args.end(), L"--studio-snap") - args.begin();
        int page = i + 1 < args.size() ? _wtoi(args[i + 1].c_str()) : 0;
        int tab = i + 2 < args.size() ? _wtoi(args[i + 2].c_str()) : 0;
        std::wstring out = i + 3 < args.size() ? args[i + 3] : ExeDir() + L"\\studio_snap.png";
        int ms = i + 4 < args.size() ? _wtoi(args[i + 4].c_str()) : 2500;
        if (const char* sc = getenv("RADIAL_SCENE"))
            if (!strcmp(sc, "clean")) bg::Run([] { FakeClean(nullptr); });
        static Config snapCfg;
        if (page == 10 && !getenv("RADIAL_NOPRIME")) {  // RADIAL_NOPRIME - снять, как выглядит подсчёт  // «Состояние»:
                                                        // размеры считаются заранее, как в работающей программе
            snapCfg = LoadOwnConfig();
            toolsui::PrimeForSnapshot(&snapCfg);
        }
        if (page == 7 && tab == 6) {  // «Защита» в студии: находки - заранее, как после фоновой проверки
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            toolsui::PrimeSecurityForSnapshot();
        }
        exitCode = EditorSnapshot(GetModuleHandleW(nullptr), ExeDir() + L"\\radial.cfg", page, tab, out, ms);
    } else if (Has(args, L"--modefx-film")) {  // --modefx-film game|dev on|off out.png: кадры анимации режима
        Gdip gdip;
        size_t i = std::find(args.begin(), args.end(), L"--modefx-film") - args.begin();
        bool game = i + 1 >= args.size() || args[i + 1] != L"dev", on = i + 2 >= args.size() || args[i + 2] != L"off";
        std::vector<std::wstring> chips =
            game ? (on ? std::vector<std::wstring>{L"Красота выключена", L"Обои на паузе", L"Таймер 0.5 мс",
                                                   L"Игре высокий приоритет", L"Фон в экономии: 4",
                                                   L"Уведомления молчат"}
                       : std::vector<std::wstring>{L"Красота вернулась", L"Обои снова идут", L"Приоритеты как были"})
                 : (on ? std::vector<std::wstring>{L"Обои на паузе", L"Видеопамять", L"Шейдеры", L"Сборки UBT"}
                       : std::vector<std::wstring>{L"Обои снова идут", L"Фон как был"});
        std::wstring title = game ? (on ? L"Игровой режим: NFS14" : L"Игровой режим выключен")
                                  : (on ? L"Режим разработки: Unreal Editor" : L"Режим разработки выключен");
        exitCode = ModeFx::Film(game ? ModeFx::GAME : ModeFx::DEV, on, title, chips, CurrentTheme(LoadOwnConfig()),
                                i + 3 < args.size() ? args[i + 3] : ExeDir() + L"\\modefx_film.png");
    } else if (Has(args, L"--boost-diag")) {  // видеокарты, профили, история сессий -> boost_out.txt
        std::wstring o;
        for (const gboost::Adapter& a : gboost::Adapters())
            o += L"adapter: " + a.name + L" vram=" + std::to_wstring((int)a.vramMb) +
                 (a.integrated ? L" (встроенная)" : L"") + L"\n";
        for (const mc::Instance& in : mc::Instances())
            o += L"mc: " + in.id + L" | " + in.name + L" | " + std::to_wstring(in.maxMb) + L" МБ | " +
                 std::to_wstring(in.mods) + L" модов | pretouch=" + (in.preTouch ? L"1" : L"0") + L" | icon=" +
                 in.icon + L" | game=" + in.game + L"\n";
        o += std::wstring(L"prism: ") + mc::PrismExe() + L" | pagefile auto=" + (mc::PagefileAuto() ? L"1" : L"0") +
             L"\n";
        for (const gboost::Session& s : gboost::History(10))
            o += L"session: " + s.name + L" " + std::to_wstring(s.ms / 60000) + L" мин\n";
        WriteOut(L"boost_out.txt", o);
        exitCode = 0;
    } else if (Has(args, L"--scene-film")) {  // --scene-film clean|guard|guard0 out.png: лист кадров живой сцены
        Gdip gdip;
        size_t i = std::find(args.begin(), args.end(), L"--scene-film") - args.begin();
        exitCode = SceneFilm(i + 1 < args.size() ? args[i + 1] : L"clean",
                             i + 2 < args.size() ? args[i + 2] : ExeDir() + L"\\scene_film.png");
    } else if (Value(args, L"--sandbox-wsb", v)) {  // собрать .wsb для файла, не запуская песочницу
        // --check: режим проверки поведения с инструментами из папки защиты
        std::wstring tools = Has(args, L"--check") ? ExpandPath(LoadOwnConfig().securityDir) : L"";
        std::wstring err, path = tools::SandboxConfig(v, false, err, tools);
        WriteOut(L"sandbox_out.txt", path.empty() ? L"error: " + err : path);
        exitCode = path.empty() ? 1 : 0;
    } else if (Has(args, L"--ring-bench")) {
        size_t i = std::find(args.begin(), args.end(), L"--ring-bench") - args.begin();
        exitCode = RingBench(i + 1 < args.size() ? std::max(500, _wtoi(args[i + 1].c_str())) : 4000);
    } else if (Value(args, L"--mft-test", v)) {  // --mft-test <папка>, из консоли администратора -> mft_out.txt
        exitCode = MftTest(v);
    } else if (Has(args, L"--clean")) {  // очистить кэши и временные файлы, итог в clean_out.txt
        exitCode = Clean();
    } else if (Has(args, L"--autostart-on")) {  // включить запуск вместе с Windows, как пункт трея
        SetAutostart(true);
        exitCode = AutostartOn() ? 0 : 1;
    } else if (Has(args, L"--tools-diag")) {
        exitCode = ToolsDiag();
    } else if (Has(args, L"--sec-diag")) {  // «Защита»: находки и подписи всего запущенного -> sec_out.txt
        exitCode = SecDiag();
    } else if (Has(args, L"--watch-diag")) {  // слежка, батарея, память; --slow - ещё версии и дубли
        exitCode = WatchDiag(Has(args, L"--slow"));
    } else if (Has(args, L"--sys-diag")) {  // восстановление, приватность, система, разработка -> sys_out.txt
        exitCode = SysDiag();
    } else if (Has(args, L"--watch-selftest")) {  // логика слежки на выдуманных данных -> selftest_out.txt
        exitCode = WatchSelftest();
    } else if (Has(args, L"--net-diag")) {
        exitCode = NetDiag();
    } else if (Has(args, L"--lock-apply")) {
        exitCode = LockApply();
    } else if (Has(args, L"--game-test")) {
        exitCode = GameTest();
    } else if (Has(args, L"--avatar")) {  // нарисовать аватар в %APPDATA%\MoonDesk\avatar.png
        Gdip gdip;
        std::wstring out, msg;
        exitCode = lockscreen::BuildAvatar(LoadOwnConfig(), out, msg) ? 0 : 1;
    } else {
        return false;
    }
    return true;
}

}  // namespace devcli
