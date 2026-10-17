#include "desk/gamemode.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include "core/config.h"
#include "core/procsnap.h"
#include "core/text.h"

namespace {

constexpr ULONGLONG kCheckMs = 2000;
constexpr int kLeaveMisses = 2;  // игра пропала на две проверки подряд - выходим

std::wstring StateFile() {
    wchar_t ad[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", ad, MAX_PATH);
    CreateDirectoryW(ad, nullptr);
    return std::wstring(ad) + L"\\gamemode.ini";
}

std::wstring FileName(const std::wstring& path) {
    size_t s = path.find_last_of(L'\\');
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

// Маска с \ сравнивается с полным путём, без - только с именем файла.
bool MatchAny(const std::wstring& path, const std::wstring& masks) {
    for (const std::wstring& m : Split(masks, L'|')) {
        if (m.empty()) continue;
        const std::wstring& subject = m.find(L'\\') != std::wstring::npos ? path : FileName(path);
        if (PathMatchSpecW(subject.c_str(), m.c_str())) return true;
    }
    return false;
}

std::wstring ProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out = QueryFullProcessImageNameW(h, 0, buf, &n) ? std::wstring(buf, n) : L"";
    CloseHandle(h);
    return out;
}

void BroadcastColors() {
    // как это делают Параметры: панель задач, Пуск и окна перечитывают прозрачность
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet");
}

DWORD ReadTransparency() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}

void WriteTransparency(DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    L"EnableTransparency", REG_DWORD, &v, sizeof(v));
    BroadcastColors();
}

// Анимации Windows: появление и сворачивание окон, анимации внутри окон и Пуска.
void SetWindowsAnimations(bool on) {
    SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, (PVOID)(INT_PTR)(on ? TRUE : FALSE), SPIF_SENDCHANGE);
    ANIMATIONINFO ai{sizeof(ai), on ? 1 : 0};
    SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, SPIF_SENDCHANGE);
}

bool WindowsAnimationsOn() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

std::wstring WallpaperEngine() {
    std::wstring out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (!_wcsicmp(pe.szExeFile, L"wallpaper64.exe") || !_wcsicmp(pe.szExeFile, L"wallpaper32.exe")) {
            out = ProcessPath(pe.th32ProcessID);
            break;
        }
    }
    CloseHandle(snap);
    return out;
}

void WallpaperControl(const std::wstring& exe, const wchar_t* what) {
    // путь приходит и из gamemode.ini / devmode.ini в %APPDATA% (откат после падения): запускаем только сам
    // Wallpaper Engine, а не что угодно, что туда запишут
    std::wstring leaf = FileName(exe);
    if (_wcsicmp(leaf.c_str(), L"wallpaper64.exe") != 0 && _wcsicmp(leaf.c_str(), L"wallpaper32.exe") != 0) return;
    std::wstring cmd = L"\"" + exe + L"\" -control " + what;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void SaveState(bool anim, bool transp, const std::wstring& wallpaper) {
    std::wstring s = L"anim=" + std::to_wstring(anim) + L"\r\ntransparency=" + std::to_wstring(transp) +
                     L"\r\nwallpaper=" + wallpaper + L"\r\n";
    std::string bytes = Narrow(s);
    HANDLE f = CreateFileW(StateFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    CloseHandle(f);
}

}  // namespace

namespace gm {
std::wstring FileName(const std::wstring& path) { return ::FileName(path); }
bool MatchAny(const std::wstring& path, const std::wstring& masks) { return ::MatchAny(path, masks); }
std::wstring ProcessPath(DWORD pid) { return ::ProcessPath(pid); }
std::wstring WallpaperEngine() { return ::WallpaperEngine(); }
void WallpaperControl(const std::wstring& exe, const wchar_t* what) { ::WallpaperControl(exe, what); }
}  // namespace gm

void GameMode::Recover() {
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
        if (k == L"anim" && v == L"1") SetWindowsAnimations(true);
        if (k == L"transparency" && v == L"1") WriteTransparency(1);
        if (k == L"wallpaper" && !v.empty()) WallpaperControl(v, L"play");
    }
    DeleteFileW(file.c_str());
}

bool GameMode::Detect(const Config& cfg, std::wstring& name) {
    QUERY_USER_NOTIFICATION_STATE st{};
    if (SUCCEEDED(SHQueryUserNotificationState(&st)) && st == QUNS_RUNNING_D3D_FULL_SCREEN) {
        name = L"полноэкранная игра";
        return true;
    }
    auto procs = procsnap::Get();
    // маски поменяли в студии - уже открытые процессы проверяются заново
    std::wstring masks = cfg.games + L"\x1" + cfg.gamesSkip + L"\x1" + cfg.devApps;
    if (masks != masks_) {
        masks_ = masks;
        games_.clear();
    }
    std::map<DWORD, Seen> seen;
    DWORD self = GetCurrentProcessId();
    bool found = false;
    for (const PROCESSENTRY32W& pe : *procs) {
        DWORD pid = pe.th32ProcessID;
        if (pid == self || pid <= 4) continue;
        auto it = games_.find(pid);
        std::wstring game;
        if (it != games_.end() && it->second.exe == pe.szExeFile) {
            game = it->second.game;
        } else {
            // путь спрашиваем только у новых процессов: старые уже проверены
            std::wstring path = ProcessPath(pid);
            if (!path.empty() && MatchAny(path, cfg.games) && !MatchAny(path, cfg.gamesSkip) &&
                !MatchAny(path, cfg.devApps))  // редактор из папки Epic Games - не игра
                game = FileName(path);
            else if (!path.empty() && recognize && !MatchAny(path, cfg.gamesSkip))
                game = recognize(pid, path);
        }
        seen[pid] = {pe.szExeFile, game};
        if (!game.empty() && !found) {
            found = true;
            name = game;
        }
    }
    games_.swap(seen);
    return found;
}

std::vector<DWORD> GameMode::GamePids() const {
    std::vector<DWORD> out;
    for (const auto& [pid, s] : games_)
        if (!s.game.empty()) out.push_back(pid);
    if (out.empty()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid) out.push_back(pid);
    }
    return out;
}

void GameMode::Tick(const Config& cfg) {
    ULONGLONG now = GetTickCount64();
    if (now - lastCheck_ < kCheckMs) return;
    lastCheck_ = now;
    std::wstring name;
    bool game = cfg.gameAuto && Detect(cfg, name);
    detected_ = game;
    if (game) {
        misses_ = 0;
        if (!manual_) reason_ = name;
    } else if (on_ && !manual_ && ++misses_ < kLeaveMisses) {
        return;  // игра могла на миг пропасть из списка (перезапуск, загрузка) - ждём ещё
    }
    if (!game) suppress_ = false;  // игра закрылась: в следующий раз режим снова включится сам
    Update(cfg, manual_ || (game && !suppress_));
}

void GameMode::SetManual(bool on, const Config& cfg) {
    manual_ = on;
    suppress_ = !on && detected_;  // выключили посреди игры - не включать снова, пока она идёт
    if (on) reason_ = L"вручную";
    misses_ = kLeaveMisses;
    lastCheck_ = 0;
    Update(cfg, on);
}

void GameMode::Update(const Config& cfg, bool want) {
    if (want == on_) return;
    on_ = want;
    if (on_)
        Enter(cfg);
    else
        Leave();
    if (onChange) onChange(on_);
}

void GameMode::Enter(const Config& cfg) {
    changedAnim_ = cfg.gameWinAnim && WindowsAnimationsOn();
    changedTransp_ = cfg.gameTransparency && ReadTransparency() != 0;
    pausedWallpaper_ = cfg.gameWallpaper ? WallpaperEngine() : L"";
    // сначала записать, что меняем: если программа упадёт, при запуске всё вернётся
    SaveState(changedAnim_, changedTransp_, pausedWallpaper_);
    if (changedAnim_) SetWindowsAnimations(false);
    if (changedTransp_) WriteTransparency(0);
    WallpaperControl(pausedWallpaper_, L"pause");
}

void GameMode::Leave() {
    if (changedAnim_) SetWindowsAnimations(true);
    if (changedTransp_) WriteTransparency(1);
    WallpaperControl(pausedWallpaper_, L"play");
    changedAnim_ = changedTransp_ = false;
    pausedWallpaper_.clear();
    DeleteFileW(StateFile().c_str());
}

void GameMode::Shutdown() {
    if (on_) Leave();
    on_ = false;
}
