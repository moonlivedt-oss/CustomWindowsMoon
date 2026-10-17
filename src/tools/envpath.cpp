// PATH и переменные окружения: чтение, мёртвые папки и дубли, что найдётся первым, правка с бэкапом.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <userenv.h>

#include <algorithm>
#include <map>
#include <set>

#include "core/elevate.h"
#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

constexpr wchar_t kUserEnv[] = L"Environment";
constexpr wchar_t kSysEnv[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

std::wstring RegRaw(HKEY root, const wchar_t* key, const wchar_t* name, DWORD* type = nullptr) {
    DWORD size = 0, t = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &t, nullptr, &size) !=
        ERROR_SUCCESS)
        return {};
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &t, s.data(), &size) !=
        ERROR_SUCCESS)
        return {};
    s.resize(wcslen(s.c_str()));
    if (type) *type = t;
    return s;
}

// Подстановка по переменным из реестра, а не по окружению процесса: RadialMenu работает с момента входа, и
// переменную, заведённую позже (%JAVA_HOME%), его окружение не знает - такая папка выглядела бы мёртвой и
// «Почистить» убрала бы её из PATH. systemOnly - только системные переменные (для процесса с правами: окружение
// пользователя ему не указ).
std::wstring ExpandFresh(const std::wstring& s, bool systemOnly) {
    if (s.find(L'%') == std::wstring::npos) return s;
    HANDLE tok = nullptr;
    if (!systemOnly && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &tok)) tok = nullptr;
    wchar_t buf[4096];
    BOOL ok = (systemOnly || tok) && ExpandEnvironmentStringsForUserW(tok, s.c_str(), buf, 4096);
    if (tok) CloseHandle(tok);
    if (ok) return buf;
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 4096);
    return n && n <= 4096 ? std::wstring(buf) : s;
}

std::wstring Expand(const std::wstring& s) { return ExpandFresh(s, false); }

std::wstring Norm(std::wstring p) {
    p = Lower(Trim(p));
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> SplitPath(const std::wstring& s) {
    std::vector<std::wstring> out;
    for (const std::wstring& p : Split(s, L';'))
        if (!p.empty()) out.push_back(p);
    return out;
}

void Broadcast() {
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 3000, nullptr);
}

std::wstring Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

void SaveText(const std::wstring& file, const std::wstring& text) {
    std::string u = Narrow(text);
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
}

std::wstring LoadText(const std::wstring& file) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(64 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return Trim(Widen(s));
}

std::wstring BackupDir() {
    std::wstring d = DataDir() + L"\\path-backups";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

const wchar_t* const kCommands[] = {L"python", L"py",    L"pip",   L"node",   L"npm",   L"git",   L"gcc",    L"g++",
                                    L"clang",  L"cmake", L"ninja", L"make",   L"code",  L"java",  L"javac",  L"dotnet",
                                    L"cargo",  L"rustc", L"go",    L"deno",   L"php",   L"mysql", L"ffmpeg", L"pwsh",
                                    L"docker", L"gh",    L"swift", L"ollama", L"choco", L"yt-dlp"};

}  // namespace

std::vector<Which> WhichAll() {
    std::vector<std::wstring> dirs;
    for (const std::wstring& p : SplitPath(RegRaw(HKEY_LOCAL_MACHINE, kSysEnv, L"Path"))) dirs.push_back(Expand(p));
    for (const std::wstring& p : SplitPath(RegRaw(HKEY_CURRENT_USER, kUserEnv, L"Path"))) dirs.push_back(Expand(p));
    std::vector<Which> out;
    for (const wchar_t* cmd : kCommands) {
        Which w;
        w.cmd = cmd;
        std::set<std::wstring> seen;
        for (const std::wstring& d : dirs) {
            for (const wchar_t* ext : {L".exe", L".cmd", L".bat", L".com"}) {
                std::wstring f = d + (d.empty() || d.back() == L'\\' ? L"" : L"\\") + cmd + ext;
                if (!IsFile(f) || !seen.insert(Norm(f)).second) continue;
                if (w.path.empty())
                    w.path = f;
                else
                    w.other.push_back(f);
                break;
            }
        }
        if (!w.path.empty()) out.push_back(w);
    }
    return out;
}

std::vector<PathEntry> ReadPath() {
    std::vector<PathEntry> out;
    std::set<std::wstring> seen;
    auto add = [&](const std::wstring& raw, bool sys) {
        PathEntry e;
        e.raw = raw;
        e.full = Expand(raw);
        e.system = sys;
        e.dead = !IsDir(e.full);
        e.dup = !seen.insert(Norm(e.full)).second;
        out.push_back(e);
    };
    for (const std::wstring& p : SplitPath(RegRaw(HKEY_LOCAL_MACHINE, kSysEnv, L"Path"))) add(p, true);
    for (const std::wstring& p : SplitPath(RegRaw(HKEY_CURRENT_USER, kUserEnv, L"Path"))) add(p, false);
    // какие команды находятся в папке первыми
    for (const Which& w : WhichAll()) {
        std::wstring dir = Norm(w.path.substr(0, w.path.find_last_of(L'\\')));
        for (PathEntry& e : out)
            if (!e.dup && Norm(e.full) == dir) {
                e.provides += (e.provides.empty() ? L"" : L", ") + w.cmd;
                break;
            }
    }
    return out;
}

bool WriteUserPath(const std::vector<std::wstring>& entries, const Log& log) {
    DWORD type = REG_SZ;
    std::wstring old = RegRaw(HKEY_CURRENT_USER, kUserEnv, L"Path", &type);
    SaveText(BackupDir() + L"\\user-" + Stamp() + L".txt", old);
    std::wstring v;
    for (const std::wstring& e : entries)
        if (!Trim(e).empty()) v += (v.empty() ? L"" : L";") + Trim(e);
    if (v.find(L'%') != std::wstring::npos) type = REG_EXPAND_SZ;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUserEnv, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) {
        log(L"Не открылся раздел переменных пользователя.");
        return false;
    }
    LONG rc = RegSetValueExW(k, L"Path", 0, type, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        log(L"PATH не записался.");
        return false;
    }
    Broadcast();
    log(L"PATH пользователя записан (старый - в бэкапах). Новые окна терминала увидят его сразу.");
    return true;
}

int CleanUserPath(const Log& log) {
    std::vector<std::wstring> keep;
    int removed = 0;
    for (const PathEntry& e : ReadPath()) {
        if (e.system) continue;
        if (e.dead || e.dup) {
            ++removed;
            log((e.dead ? L"Нет папки: " : L"Дубль: ") + e.raw);
        } else {
            keep.push_back(e.raw);
        }
    }
    if (!removed) {
        log(L"В PATH пользователя мёртвых папок и дублей нет.");
        return 0;
    }
    return WriteUserPath(keep, log) ? removed : 0;
}

bool AddUserPath(const std::wstring& dir, const Log& log) {
    if (!IsDir(Expand(dir))) {
        log(L"Такой папки нет: " + dir);
        return false;
    }
    std::vector<std::wstring> cur;
    for (const PathEntry& e : ReadPath()) {
        if (Norm(e.full) == Norm(Expand(dir))) {
            log(L"Эта папка уже в PATH" + std::wstring(e.system ? L" (системном)." : L"."));
            return false;
        }
        if (!e.system) cur.push_back(e.raw);
    }
    cur.push_back(dir);
    return WriteUserPath(cur, log);
}

std::vector<std::wstring> PathBackups() {
    std::vector<std::pair<FILETIME, std::wstring>> list;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((BackupDir() + L"\\user-*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do list.push_back({fd.ftLastWriteTime, BackupDir() + L"\\" + fd.cFileName});
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(list.begin(), list.end(),
              [](const auto& a, const auto& b) { return CompareFileTime(&a.first, &b.first) > 0; });
    std::vector<std::wstring> out;
    for (auto& p : list) out.push_back(p.second);
    return out;
}

bool RestoreUserPath(const std::wstring& backupFile, const Log& log) {
    std::wstring v = LoadText(backupFile);
    if (v.empty()) {
        log(L"Бэкап пустой или не читается.");
        return false;
    }
    return WriteUserPath(SplitPath(v), log);
}

int CleanSystemPath(const Log& log) {
    DWORD type = REG_EXPAND_SZ;
    std::wstring old = RegRaw(HKEY_LOCAL_MACHINE, kSysEnv, L"Path", &type);
    if (old.empty()) {
        log(L"Системный PATH не прочитался.");
        return 0;
    }
    std::set<std::wstring> seen;
    std::wstring v;
    int removed = 0;
    for (const std::wstring& p : SplitPath(old)) {
        std::wstring full = ExpandFresh(p, true);
        // переменная не раскрылась без окружения пользователя (%USERPROFILE%) - не судим, оставляем как есть
        if (full.find(L'%') != std::wstring::npos) {
            v += (v.empty() ? L"" : L";") + p;
            continue;
        }
        if (!IsDir(full) || !seen.insert(Norm(full)).second) {
            ++removed;
            log((IsDir(full) ? L"Дубль: " : L"Нет папки: ") + p);
            continue;
        }
        v += (v.empty() ? L"" : L";") + p;
    }
    if (!removed) {
        log(L"В системном PATH мёртвых папок и дублей нет.");
        return 0;
    }
    // копия системного PATH - в папке помощника (Program Files\MoonRadial\data): сюда пишет процесс с правами,
    // а в папке пользователя подложенная ссылка перенаправила бы его запись
    std::wstring adminDir = elevate::DataDir() + L"\\path-backups";
    CreateDirectoryW(elevate::DataDir().c_str(), nullptr);
    CreateDirectoryW(adminDir.c_str(), nullptr);
    SaveText(adminDir + L"\\system-" + Stamp() + L".txt", old);
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSysEnv, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) {
        log(L"Нет прав на системный PATH.");
        return 0;
    }
    RegSetValueExW(k, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    Broadcast();
    log(L"Системный PATH почищен, убрано: " + std::to_wstring(removed) + L" (старый - в бэкапах).");
    return removed;
}

std::vector<EnvVar> ReadEnv() {
    std::vector<EnvVar> out;
    auto read = [&](HKEY root, const wchar_t* key, bool sys) {
        HKEY k;
        if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
        for (DWORD i = 0;; ++i) {
            wchar_t name[256];
            DWORD nlen = 256, type = 0, size = 0;
            if (RegEnumValueW(k, i, name, &nlen, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) break;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            if (_wcsicmp(name, L"Path") == 0) continue;
            EnvVar v;
            v.name = name;
            v.value = RegRaw(root, key, name);
            v.system = sys;
            std::wstring full = Expand(v.value);
            bool pathLike =
                full.size() > 3 && full[1] == L':' && full[2] == L'\\' && full.find(L';') == std::wstring::npos;
            v.deadPath = pathLike && GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES;
            out.push_back(v);
        }
        RegCloseKey(k);
    };
    read(HKEY_CURRENT_USER, kUserEnv, false);
    read(HKEY_LOCAL_MACHINE, kSysEnv, true);
    std::sort(out.begin(), out.end(), [](const EnvVar& a, const EnvVar& b) {
        if (a.system != b.system) return !a.system;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

bool SetUserVar(const std::wstring& name, const std::wstring& value, const Log& log) {
    if (name.empty() || _wcsicmp(name.c_str(), L"Path") == 0) return false;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUserEnv, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    LONG rc;
    if (value.empty()) {
        rc = RegDeleteValueW(k, name.c_str());
    } else {
        DWORD type = value.find(L'%') != std::wstring::npos ? REG_EXPAND_SZ : REG_SZ;
        rc = RegSetValueExW(k, name.c_str(), 0, type, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
    if (rc == ERROR_SUCCESS) Broadcast();
    log(rc == ERROR_SUCCESS ? (value.empty() ? L"Удалена " : L"Записана ") + name : L"Не получилось: " + name);
    return rc == ERROR_SUCCESS;
}

}  // namespace tools
