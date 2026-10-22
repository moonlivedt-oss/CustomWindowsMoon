// Система: устройства с ошибками, температура и троттлинг видеокарты, история износа батареи, обновления Windows,
// отчёт о системе одним файлом. Только чтение.
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <shlobj.h>
#include <wuapi.h>

#include <algorithm>
#include <map>

#include "core/files.h"
#include "core/text.h"
#include "tools/tools.h"
#include "tools/weekdiff.h"

namespace tools {

// ---------------- устройства ----------------

namespace {
// Частые коды проблем Диспетчера устройств - человеческими словами.
const wchar_t* ProblemText(ULONG code) {
    switch (code) {
        case CM_PROB_NOT_CONFIGURED:
            return L"не настроено";
        case CM_PROB_FAILED_START:
            return L"не запускается (код 10)";
        case CM_PROB_DRIVER_FAILED_LOAD:
            return L"драйвер не загрузился";
        case CM_PROB_FAILED_INSTALL:
            return L"драйвер не установлен (код 28)";
        case CM_PROB_DISABLED:
            return L"выключено вручную";
        case CM_PROB_HARDWARE_DISABLED:
            return L"выключено в BIOS или самим устройством";
        case CM_PROB_FAILED_POST_START:
            return L"Windows остановила его из-за ошибок (код 43)";
        case CM_PROB_DRIVER_BLOCKED:
            return L"драйвер заблокирован Windows";
        case CM_PROB_UNSIGNED_DRIVER:
            return L"драйвер без подписи (код 52)";
        case CM_PROB_NEED_RESTART:
            return L"нужна перезагрузка";
        case CM_PROB_FAILED_ADD:
            return L"не удалось добавить (код 31)";
        default:
            return nullptr;
    }
}
}  // namespace

std::vector<DeviceProblem> DeviceProblems() {
    std::vector<DeviceProblem> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) != CR_SUCCESS || !(status & DN_HAS_PROBLEM))
            continue;
        wchar_t name[256] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)name, sizeof(name),
                                               nullptr))
            SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, nullptr, (BYTE*)name, sizeof(name), nullptr);
        DeviceProblem p;
        p.name = name;
        p.code = problem;
        const wchar_t* t = ProblemText(problem);
        p.text = t ? t : L"ошибка, код " + std::to_wstring(problem);
        p.disabled = problem == CM_PROB_DISABLED;
        out.push_back(p);
    }
    SetupDiDestroyDeviceInfoList(set);
    std::sort(out.begin(), out.end(),
              [](const DeviceProblem& a, const DeviceProblem& b) { return a.disabled < b.disabled; });
    return out;
}

// ---------------- видеокарта ----------------

GpuSample GpuNow() {
    GpuSample g;
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = std::wstring(sys) + L"\\nvidia-smi.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return g;
    std::wstring out;
    if (RunCapture(L"\"" + exe +
                       L"\" --query-gpu=temperature.gpu,clocks_event_reasons.active,utilization.gpu,power.draw "
                       L"--format=csv,noheader,nounits",
                   out, 8000) != 0)
        return g;
    std::vector<std::wstring> f = Split(Trim(out), L',');
    if (f.size() < 4) return g;
    g.ok = true;
    g.temp = _wtoi(f[0].c_str());
    ULONGLONG reasons = wcstoull(f[1].c_str(), nullptr, 16);
    // 0x20 - программный перегрев, 0x40 - аппаратный перегрев, 0x80 - аппаратное торможение по питанию
    g.thermal = (reasons & (0x20 | 0x40)) != 0;
    g.powerCap = (reasons & (0x04 | 0x80)) != 0;
    g.load = _wtoi(f[2].c_str());
    g.watts = _wtof(f[3].c_str());
    return g;
}

// ---------------- батарея по неделям ----------------

namespace {
std::wstring BatteryFile() { return DataDir() + L"\\battery.tsv"; }
int DayNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / (24ull * 3600 * 10000000));
}
}  // namespace

void RecordBattery() {
    BatteryInfo b = Battery();
    if (!b.present || !b.full || !b.designed) return;
    // день -> ёмкость, минут на зарядке, минут наблюдения (зовётся раз в минуту, пока программа работает)
    struct Day {
        ULONGLONG cap = 0;
        int ac = 0, seen = 0;
    };
    std::map<int, Day> days;
    HANDLE f = CreateFileW(BatteryFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::string s(256 * 1024, '\0');
        DWORD got = 0;
        ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
        CloseHandle(f);
        s.resize(got);
        for (const std::wstring& l : Split(Widen(s), L'\n')) {
            std::vector<std::wstring> p = Split(l, L'\t');
            if (p.size() >= 4)
                days[_wtoi(p[0].c_str())] = {_wcstoui64(p[1].c_str(), nullptr, 10), _wtoi(p[2].c_str()),
                                             _wtoi(p[3].c_str())};
        }
    }
    Day& today = days[DayNow()];
    today.cap = b.full;
    today.seen += 1;
    if (b.ac) today.ac += 1;
    while (days.size() > 400) days.erase(days.begin());
    std::wstring text;
    for (const auto& [d, v] : days)
        text += std::to_wstring(d) + L"\t" + std::to_wstring(v.cap) + L"\t" + std::to_wstring(v.ac) + L"\t" +
                std::to_wstring(v.seen) + L"\n";
    std::string bytes = Narrow(text);
    f = CreateFileW(BatteryFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    CloseHandle(f);
}

BatteryTrend BatteryHistory() {
    BatteryTrend t;
    BatteryInfo b = Battery();
    t.designed = b.designed;
    t.now = b.full;
    HANDLE f = CreateFileW(BatteryFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return t;
    std::string s(256 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    int today = DayNow(), ac = 0, seen = 0;
    for (const std::wstring& l : Split(Widen(s), L'\n')) {
        std::vector<std::wstring> p = Split(l, L'\t');
        if (p.size() < 4) continue;
        int d = _wtoi(p[0].c_str());
        ULONGLONG cap = _wcstoui64(p[1].c_str(), nullptr, 10);
        t.points.push_back({d, cap});
        if (today - d <= 35 && !t.monthAgo) t.monthAgo = cap, t.monthDays = today - d;
        if (today - d <= 14) ac += _wtoi(p[2].c_str()), seen += _wtoi(p[3].c_str());
    }
    // последние две недели больше 80% времени на зарядке (и наблюдали хотя бы сутки) - батарее лучше ограничение
    t.mostlyAc = seen >= 24 * 60 && ac * 10 > seen * 8;
    return t;
}

// ---------------- обновления Windows ----------------

namespace {
const CLSID kUpdateSession = {0x4CB43D7F, 0x7EEE, 0x4906, {0x86, 0x98, 0x60, 0xDA, 0x1C, 0x38, 0xF2, 0xFE}};
const IID kIUpdateSession = {0x816858A4, 0x260D, 0x4260, {0x93, 0x3A, 0x25, 0x85, 0xF1, 0xAB, 0xC7, 0x6B}};
}  // namespace

UpdateInfo WindowsUpdates() {
    UpdateInfo u;
    IUpdateSession* s = nullptr;
    if (FAILED(CoCreateInstance(kUpdateSession, nullptr, CLSCTX_INPROC_SERVER, kIUpdateSession, (void**)&s))) return u;
    IUpdateSearcher* se = nullptr;
    if (SUCCEEDED(s->CreateUpdateSearcher(&se))) {
        LONG total = 0;
        se->GetTotalHistoryCount(&total);
        IUpdateHistoryEntryCollection* h = nullptr;
        if (total > 0 && SUCCEEDED(se->QueryHistory(0, std::min<LONG>(total, 60), &h))) {
            LONG n = 0;
            h->get_Count(&n);
            for (LONG i = 0; i < n; ++i) {
                IUpdateHistoryEntry* e = nullptr;
                if (FAILED(h->get_Item(i, &e))) continue;
                OperationResultCode rc = orcNotStarted;
                DATE when = 0;
                BSTR title = nullptr;
                e->get_ResultCode(&rc);
                e->get_Date(&when);
                e->get_Title(&title);
                SYSTEMTIME st;
                VariantTimeToSystemTime(when, &st);
                FILETIME ft;
                SystemTimeToFileTime(&st, &ft);
                ULONGLONG stamp = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
                std::wstring t = title ? title : L"";
                if ((rc == orcSucceeded || rc == orcSucceededWithErrors) && stamp > u.lastOk) {
                    u.lastOk = stamp;
                    u.lastTitle = t;
                }
                if (rc == orcFailed || rc == orcAborted) {
                    FILETIME now;
                    GetSystemTimeAsFileTime(&now);
                    ULONGLONG n100 = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
                    if (n100 - stamp < 30ull * 24 * 3600 * 10000000) u.failed.push_back(t);
                }
                SysFreeString(title);
                e->Release();
            }
            h->Release();
        }
        se->Release();
    }
    s->Release();
    u.ok = true;
    return u;
}

// ---------------- отчёт о системе ----------------

std::wstring SystemReport() {
    std::wstring r;
    const std::wstring nl = L"\r\n";
    auto line = [&](const std::wstring& s) { r += s + nl; };
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t now[32];
    swprintf(now, 32, L"%02d.%02d.%04d %02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    line(L"Отчёт о системе (RadialMenu), " + std::wstring(now));
    line(L"Пути и имена в отчёте - ваши: перед отправкой кому-то посмотрите, нет ли лишнего.");
    line(L"");
    // Windows и железо
    HKEY k;
    wchar_t buf[256];
    DWORD size;
    auto reg = [&](HKEY root, const wchar_t* key, const wchar_t* name) {
        size = sizeof(buf);
        buf[0] = 0;
        RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &size);
        return std::wstring(buf);
    };
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    line(L"Windows: " + reg(HKEY_LOCAL_MACHINE, cv, L"ProductName") + L" " +
         reg(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion") + L", сборка " + reg(HKEY_LOCAL_MACHINE, cv, L"CurrentBuild"));
    line(L"Процессор: " +
         Trim(reg(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")));
    MemoryState m = Memory();
    line(L"Память: " + Bytes(m.total) + L", занято " + std::to_wstring(m.loadPercent) + L"%");
    Driver d = NvidiaDriver();
    if (!d.gpu.empty()) line(L"Видеокарта: " + d.gpu + L", драйвер " + d.installed);
    GpuSample g = GpuNow();
    if (g.ok)
        line(L"Видеокарта сейчас: " + std::to_wstring(g.temp) + L" °C, нагрузка " + std::to_wstring(g.load) + L"%");
    (void)k;
    BatteryInfo b = Battery();
    if (b.present)
        line(L"Батарея: " + std::to_wstring(b.percent) + L"%, ёмкость " + std::to_wstring(b.full / 1000) + L" из " +
             std::to_wstring(b.designed / 1000) + L" Вт*ч, циклов " + std::to_wstring(b.cycles));
    line(L"");
    line(L"Диски:");
    for (const Volume& v : Volumes())
        line(L"  " + v.root + L" " + v.label + L" (" + v.fs + L"): свободно " + Bytes(v.free) + L" из " +
             Bytes(v.total));
    for (const Physical& p : PhysicalDisks())
        line(L"  " + p.name + (p.ssd ? L", SSD" : L", HDD") +
             (p.health == 0   ? L", исправен"
              : p.health == 1 ? L", ПРЕДУПРЕЖДЕНИЕ"
                              : L", НЕИСПРАВЕН"));
    line(L"");
    std::vector<std::wstring> reboot = PendingReboot();
    line(L"Ждёт перезагрузки: " + (reboot.empty() ? std::wstring(L"нет") : reboot[0]));
    UpdateInfo u = WindowsUpdates();
    if (u.ok && u.lastOk) {
        FILETIME ft{(DWORD)u.lastOk, (DWORD)(u.lastOk >> 32)};
        SYSTEMTIME t;
        FileTimeToSystemTime(&ft, &t);
        wchar_t day[16];
        swprintf(day, 16, L"%02d.%02d.%04d", t.wDay, t.wMonth, t.wYear);
        line(L"Последнее обновление Windows: " + std::wstring(day) + L" - " + u.lastTitle);
    }
    for (const std::wstring& f : u.failed) line(L"  Не установилось: " + f);
    line(L"");
    line(L"Устройства с ошибками:");
    std::vector<DeviceProblem> dev = DeviceProblems();
    if (dev.empty()) line(L"  нет");
    for (const DeviceProblem& p : dev) line(L"  " + p.name + L": " + p.text);
    line(L"");
    line(L"Автозагрузка (включено):");
    for (const StartItem& it : StartupItems())
        if (it.enabled) line(L"  " + it.where + L": " + it.name);
    line(L"");
    line(L"Находки «Защиты»:");
    SecurityScan sec = ScanSecurity();
    size_t n = 0;
    for (const auto* v : {&sec.hijacks, &sec.procs, &sec.autos, &sec.tasks, &sec.exts, &sec.nets})
        for (const Suspect& s : *v) {
            line(L"  [" + std::to_wstring(s.score) + L"] " + s.name + L" - " + s.why);
            ++n;
        }
    if (!n) line(L"  ничего странного");
    line(L"");
    line(L"Конфликты версий в PATH:");
    std::vector<Conflict> cf = PathConflicts();
    if (cf.empty()) line(L"  нет");
    for (const Conflict& c : cf) line(L"  " + c.cmd + L": " + c.winnerVer + L" затеняет " + c.loserVer);
    line(L"");
    WeekReport week = WeekRecordAndCompare();
    line(L"Изменилось за " + std::to_wstring(week.days) + L" дн. (программы, автозапуск, службы, драйверы, задачи):");
    if (!week.days) line(L"  сравнивать пока не с чем - снимки начаты сегодня");
    for (const WeekChange& c : week.changes)
        line(std::wstring(c.what == WeekChange::ADDED     ? L"  + "
                          : c.what == WeekChange::REMOVED ? L"  - "
                                                          : L"  ~ ") +
             WeekKindName(c.entry.kind) + L": " + c.entry.label + (c.was.empty() ? L"" : L" (было " + c.was + L")"));
    return r;
}

}  // namespace tools
