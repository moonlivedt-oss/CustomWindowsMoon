#include "core/usage.h"
#include "desk/devmode.h"

#include <tlhelp32.h>

#include <algorithm>

#include "core/procsnap.h"
#include "core/config.h"
#include "core/text.h"
#include "desk/gamemode.h"

namespace {

constexpr ULONGLONG kCheckMs = 3000;

std::wstring StateFile() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", ad, MAX_PATH);
    CreateDirectoryW(ad, nullptr);
    return std::wstring(ad) + L"\\devmode.ini";
}

// «UnrealEditor.exe» -> «Unreal Editor»: для трея и волны
std::wstring Pretty(std::wstring exe) {
    if (EndsWithI(exe, L".exe")) exe.resize(exe.size() - 4);
    if (!_wcsicmp(exe.c_str(), L"UnrealEditor") || !_wcsicmp(exe.c_str(), L"UE4Editor")) return L"Unreal Editor";
    if (!exe.empty() && exe[0] >= L'a' && exe[0] <= L'z') exe[0] = (wchar_t)(exe[0] - L'a' + L'A');  // blender
    return exe;
}

}  // namespace

void DevMode::Recover() {
    std::wstring file = StateFile();
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string text(GetFileSize(f, nullptr), '\0');
    DWORD got = 0;
    ReadFile(f, text.data(), (DWORD)text.size(), &got, nullptr);
    CloseHandle(f);
    for (const std::wstring& line : Split(Widen(text), L'\n')) {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if (k == L"wallpaper" && !v.empty()) gm::WallpaperControl(v, L"play");
    }
    DeleteFileW(file.c_str());
}

bool DevMode::Detect(const Config& cfg, std::wstring& name) {
    auto procs = procsnap::Get();
    std::wstring masks = cfg.devApps + L"\x1" + cfg.devBusy;
    if (masks != masks_) {  // маски поменяли - открытые процессы проверяются заново
        masks_ = masks;
        seen_.clear();
    }
    // путь спрашиваем, только если маска с папкой
    bool byPath = masks_.find(L'\\') != std::wstring::npos;
    std::map<DWORD, Seen> seen;
    bool found = false, foundBusy = false;
    std::wstring busyName;
    std::map<DWORD, ULONGLONG> cpuNow;  // процессор занятых редакторов - только если такие запущены
    for (const PROCESSENTRY32W& pe : *procs) {
        DWORD pid = pe.th32ProcessID;
        if (pid <= 4) continue;
        auto it = seen_.find(pid);
        Seen s{pe.szExeFile, L"", false};
        if (it != seen_.end() && it->second.exe == pe.szExeFile) {
            s = it->second;
        } else {
            std::wstring path = byPath ? gm::ProcessPath(pid) : L"";
            const std::wstring& subject = path.empty() ? std::wstring(pe.szExeFile) : path;
            if (gm::MatchAny(subject, cfg.devApps))
                s.app = Pretty(pe.szExeFile);
            else if (gm::MatchAny(subject, cfg.devBusy))
                s.app = Pretty(pe.szExeFile), s.busyApp = true;
        }
        seen[pid] = s;
        if (s.app.empty()) continue;
        if (!s.busyApp) {
            if (!found) name = s.app;
            found = true;
        } else {
            cpuNow[pid] = 0;
        }
    }
    seen_.swap(seen);
    // занятые редакторы (рендер Blender): доля всех ядер за прошлый промежуток
    if (!cpuNow.empty()) {
        for (const procsnap::Info& p : procsnap::Session())
            if (cpuNow.count(p.pid)) cpuNow[p.pid] = p.cpu;
        ULONGLONG now = GetTickCount64();
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        double cores = std::max<DWORD>(1, si.dwNumberOfProcessors);
        std::map<DWORD, Cpu> next;
        for (auto& [pid, cpu] : cpuNow) {
            Cpu c = cpu_.count(pid) ? cpu_[pid] : Cpu{};
            double share = c.at && now > c.at && cpu >= c.cpu ? (cpu - c.cpu) / 10000.0 / (now - c.at) / cores : 0;
            c.cpu = cpu;
            c.at = now;
            if (c.hold.Want(now, share) && !foundBusy) {
                foundBusy = true;
                busyName = seen_[pid].app + L" (рендер)";
            }
            next[pid] = c;
        }
        cpu_.swap(next);
    } else {
        cpu_.clear();
    }
    if (!found && foundBusy) name = busyName;
    if (!on_) busy_ = !found && foundBusy;
    return found || foundBusy;
}

std::vector<DWORD> DevMode::Pids() const {
    std::vector<DWORD> out;
    for (const auto& [pid, s] : seen_)
        if (!s.app.empty() && !s.busyApp) out.push_back(pid);
    return out;
}

std::wstring DevMode::ExePath() const {
    std::vector<DWORD> pids = Pids();
    return pids.empty() ? L"" : gm::ProcessPath(pids[0]);
}

void DevMode::Tick(const Config& cfg) {
    ULONGLONG now = GetTickCount64();
    if (now - lastCheck_ < kCheckMs) return;
    lastCheck_ = now;
    std::wstring name;
    bool dev = cfg.devAuto && Detect(cfg, name);
    detected_ = dev;
    if (dev && !sw_.manual) reason_ = name;
    Update(cfg, sw_.Want(now, dev));
}

void DevMode::SetManual(bool on, const Config& cfg) {
    sw_.SetManual(on, GetTickCount64(), detected_);
    if (on) reason_ = L"вручную";
    if (on && !on_) busy_ = false;
    lastCheck_ = 0;
    Update(cfg, on);
}

void DevMode::Update(const Config& cfg, bool want) {
    if (want == on_) return;
    on_ = sw_.on = want;
    if (want && !sw_.manual && cfg.devAuto) usage::Hit(L"dev_auto");
    if (on_)
        Enter(cfg);
    else
        Leave();
    if (onChange) onChange(on_);
}

void DevMode::Enter(const Config& cfg) {
    pausedWallpaper_ = cfg.devWallpaper ? gm::WallpaperEngine() : L"";
    if (pausedWallpaper_.empty()) return;
    std::string bytes = Narrow(L"wallpaper=" + pausedWallpaper_ + L"\r\n");
    HANDLE f = CreateFileW(StateFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
        CloseHandle(f);
    }
    gm::WallpaperControl(pausedWallpaper_, L"pause");
    usage::Hit(L"dev_wallpaper");
}

void DevMode::Reapply(const Config& cfg) {
    if (!on_) return;
    if (pausedWallpaper_.empty()) {
        Enter(cfg);
    } else {
        gm::WallpaperControl(pausedWallpaper_, L"pause");
    }
}

void DevMode::Leave() {
    if (!pausedWallpaper_.empty() && !(gameOn && gameOn())) gm::WallpaperControl(pausedWallpaper_, L"play");
    pausedWallpaper_.clear();
    DeleteFileW(StateFile().c_str());
}

void DevMode::Shutdown() {
    if (on_) Leave();
    on_ = false;
}
