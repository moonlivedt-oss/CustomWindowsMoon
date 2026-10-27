#include "core/elevate.h"

#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <userenv.h>
#include <bcrypt.h>

#include <atomic>
#include <cwchar>
#include <vector>

#include "core/bg.h"

namespace elevate {

namespace {

constexpr wchar_t kKey[] = L"Software\\MoonRadial";

struct TaskInfo {
    const wchar_t* name;  // имя в планировщике
    const wchar_t* arg;   // что выполняет помощник
};
const TaskInfo kTasks[] = {
    {L"MoonRadial\\RadialMenuNet", L"--net-job"},
    {L"MoonRadial\\RadialMenuPurge", L"--purge"},
};

std::wstring ThisExe() {
    wchar_t b[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    return b;
}

std::wstring System32(const wchar_t* exe) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys) + L"\\" + exe;
}

// Программа из System32 с выводом в строку; код выхода или -1.
int Sys(const wchar_t* exe, const std::wstring& args, std::wstring* out, DWORD timeoutMs) {
    std::wstring path = System32(exe), cmd = L"\"" + path + L"\" " + args;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    std::string raw;
    char buf[4096];
    DWORD got = 0;
    // вывод читаем без блокировки: зависший schtasks не держит программу дольше timeoutMs
    ULONGLONG until = GetTickCount64() + timeoutMs;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail) {
            if (!ReadFile(rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, nullptr) || !got) break;
            raw.append(buf, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
                   ReadFile(rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, nullptr) && got)
                raw.append(buf, got);
            break;
        }
        if (GetTickCount64() > until) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    CloseHandle(rd);
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, 1000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (out) {
        out->clear();
        bool utf16 = raw.size() >= 2 && raw.size() % 2 == 0 && raw.find('\0') != std::string::npos;
        if (utf16) {
            out->assign((const wchar_t*)raw.data(), raw.size() / 2);
        } else if (!raw.empty()) {
            int n = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), nullptr, 0);
            out->resize(n);
            MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), out->data(), n);
        }
    }
    return (int)code;
}

// SHA-256 файла (hex строчными); пусто - не прочитать
std::wstring FileHash(const std::wstring& path) {
    std::wstring out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<unsigned char> buf(1 << 20);
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
        ok = BCryptHashData(h, buf.data(), got, 0) == 0;
    unsigned char hash[32];
    if (ok && BCryptFinishHash(h, hash, sizeof(hash), 0) == 0) {
        wchar_t hx[3];
        for (unsigned char b : hash) swprintf(hx, 3, L"%02x", b), out += hx;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// Что запускает задача: <Command> и <Arguments> из её описания.
bool TaskRunsHelper(const TaskInfo& t) {
    std::wstring xml;
    if (Sys(L"schtasks.exe", L"/Query /TN \"" + std::wstring(t.name) + L"\" /XML", &xml, 15000) != 0) return false;
    auto tag = [&](const wchar_t* name) {
        std::wstring open = std::wstring(L"<") + name + L">", close = std::wstring(L"</") + name + L">";
        size_t a = xml.find(open), b = a == std::wstring::npos ? a : xml.find(close, a);
        if (b == std::wstring::npos) return std::wstring();
        std::wstring v = xml.substr(a + open.size(), b - a - open.size());
        while (!v.empty() && (v.front() == L'"' || v.front() == L' ')) v.erase(v.begin());
        while (!v.empty() && (v.back() == L'"' || v.back() == L' ')) v.pop_back();
        return v;
    };
    return !_wcsicmp(tag(L"Command").c_str(), HelperExe().c_str()) && tag(L"Arguments") == t.arg;
}

// SID текущего пользователя строкой (S-1-5-21-...): ему - право запускать задачи
std::wstring UserSid() {
    std::wstring out;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
    DWORD size = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    LPWSTR str = nullptr;
    if (size && GetTokenInformation(tok, TokenUser, buf.data(), size, &size) &&
        ConvertSidToStringSidW(((TOKEN_USER*)buf.data())->User.Sid, &str)) {
        out = str;
        LocalFree(str);
    }
    CloseHandle(tok);
    return out;
}

// schtasks /Create для задачи: запускается только по требованию, дата старта - далёкое будущее
std::wstring CreateCmd(const TaskInfo& t) {
    return L"\"" + System32(L"schtasks.exe") + L"\" /Create /TN \"" + std::wstring(t.name) + L"\" /TR \"\\\"" +
           HelperExe() + L"\\\" " + t.arg + L"\" /SC ONCE /ST 00:00 /SD 01/01/2030 /RL HIGHEST /F >nul";
}

// Начало команды системного cmd с правами: папка помощника, копия exe и сверка её SHA-256 с хэшем, посчитанным при
// старте программы. Между стартом и копированием файл рядом с программой могли подменить: тогда копия удаляется, и
// cmd выходит, ничего не создав и не запустив.
std::wstring InstallCmd() {
    std::wstring dir = HelperDir(), data = DataDir(), helper = HelperExe();
    return L"(if not exist \"" + dir + L"\" mkdir \"" + dir + L"\") & (if not exist \"" + data + L"\" mkdir \"" + data +
           L"\") & (if not exist \"" + helper + L"\" copy /y \"" + ThisExe() + L"\" \"" + helper + L"\" >nul) & (\"" +
           System32(L"certutil.exe") + L"\" -hashfile \"" + helper + L"\" SHA256 | \"" + System32(L"findstr.exe") +
           L"\" /i /c:\"" + SelfHash() + L"\" >nul || (del /f /q \"" + helper + L"\" >nul 2>&1 & exit 1))";
}

}  // namespace

std::wstring HelperDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p)) && p) dir = std::wstring(p);
    CoTaskMemFree(p);
    return dir.empty() ? L"" : dir + L"\\MoonRadial";
}

// У каждой сборки своё имя помощника - по содержимому exe. Заменить файл на месте удаётся не всегда (помощник
// выполняет задание, или его держит антивирус), а новое имя ставится всегда; старые копии убираются, когда можно.
std::wstring HelperExe() { return HelperDir() + L"\\" + HelperName(SelfHash()); }

std::wstring SelfHash() {
    static const std::wstring hash = FileHash(ThisExe());
    return hash;
}

bool HelperIntact() {
    std::wstring self = SelfHash();
    return !HelperDir().empty() && self.size() == 64 && FileHash(HelperExe()) == self;
}

bool SelfTrusted() { return TrustedHelper(ThisExe(), HelperDir(), SelfHash()); }

void RemoveOldHelpers() {
    std::wstring dir = HelperDir(), self = ThisExe();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\RadialMenu-*.exe").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            _wcsicmp(p.c_str(), self.c_str()) != 0 && InsideDir(p, dir))
            DeleteFileW(p.c_str());  // занятую (ещё работает) - в другой раз
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void SystemEnvironment() {
    // свои переменные - прочь, все: и PATH, и COR_PROFILER*, и APPDATA
    if (wchar_t* env = GetEnvironmentStringsW()) {
        std::vector<std::wstring> names;
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const wchar_t* eq = wcschr(p + 1, L'=');  // «=C:=C:» - служебные, имя с «=» в начале
            if (eq) names.emplace_back(p, eq - p);
        }
        FreeEnvironmentStringsW(env);
        for (const std::wstring& n : names)
            if (!n.empty() && n[0] != L'=') SetEnvironmentVariableW(n.c_str(), nullptr);
    }
    void* block = nullptr;
    if (CreateEnvironmentBlock(&block, nullptr, FALSE) && block) {  // без токена - только системные переменные
        for (const wchar_t* p = (const wchar_t*)block; *p; p += wcslen(p) + 1) {
            const wchar_t* eq = wcschr(p + 1, L'=');
            if (eq) SetEnvironmentVariableW(std::wstring(p, eq - p).c_str(), eq + 1);
        }
        DestroyEnvironmentBlock(block);
    }
}

std::wstring DataDir() { return HelperDir() + L"\\data"; }

// Спрашиваем сам планировщик: ветку TaskCache в HKLM обычный пользователь читать не может, и прежняя проверка
// по реестру всегда отвечала «задачи нет» - каждое действие заново спрашивало UAC и пересоздавало задачу.
bool TaskExists(Task t) {
    return Sys(L"schtasks.exe", L"/Query /TN \"" + std::wstring(kTasks[t].name) + L"\"", nullptr, 15000) == 0;
}

// Проверка дорогая (два прохода по exe и запуск schtasks), а студия и кольцо утилит спрашивают её при каждой
// отрисовке: ответ помним 30 секунд, Setup сбрасывает.
std::atomic<ULONGLONG> g_readyAt[2];
std::atomic<bool> g_ready[2];

bool TaskReady(Task t) {
    ULONGLONG now = GetTickCount64(), at = g_readyAt[t];
    if (at && now - at < 30000) return g_ready[t];
    bool ok = HelperIntact() && TaskRunsHelper(kTasks[t]);
    g_ready[t] = ok;
    g_readyAt[t] = GetTickCount64();
    return ok;
}

std::atomic<bool> g_readyBusy[2];

bool TaskReadyCached(Task t) {
    ULONGLONG at = g_readyAt[t];
    if ((!at || GetTickCount64() - at >= 30000) && !g_readyBusy[t].exchange(true) &&
        !bg::Run(
            [t] {
                TaskReady(t);
                g_readyBusy[t] = false;
            },
            THREAD_PRIORITY_BELOW_NORMAL))
        g_readyBusy[t] = false;
    return g_ready[t];
}

// Ставит системный cmd с правами, а не сам RadialMenu.exe: Касперский останавливает программу, которая кладёт
// свой exe в Program Files и тут же заводит на него задачу с наивысшими правами (так выглядит закрепление
// вредоноса). Нужная задача - всегда, остальные - только если уже были; потом убрать прежние копии помощника.
bool Setup(Task t) {
    std::wstring dir = HelperDir();
    if (dir.empty() || SelfHash().size() != 64) return false;
    std::wstring cmd = L"/s /c \"" + InstallCmd();
    for (int i = 0; i < (int)(sizeof(kTasks) / sizeof(kTasks[0])); ++i)
        if (i == t)
            cmd += L" & " + CreateCmd(kTasks[i]);
        else
            cmd += L" & (\"" + System32(L"schtasks.exe") + L"\" /Query /TN \"" + std::wstring(kTasks[i].name) +
                   L"\" >nul 2>&1 && " + CreateCmd(kTasks[i]) + L")";
    // Задачу, созданную администратором, пользователь без прав может только читать, а запуск отвечает «параметр
    // задан неверно». Право «читать и запускать» (не «менять»: иначе любая программа пользователя переписала бы
    // задачу с правами) - явно, через планировщик.
    std::wstring sid = UserSid(), names;
    for (const TaskInfo& ti : kTasks) {
        std::wstring n = ti.name;
        names += (names.empty() ? L"'" : L",'") + n.substr(n.find(L'\\') + 1) + L"'";
    }
    if (!sid.empty())
        cmd += L" & \"" + System32(L"WindowsPowerShell\\v1.0\\powershell.exe") +
               L"\" -NoProfile -NonInteractive -Command \"$env:PSModulePath=$PSHOME+'\\Modules';"
               L"$s=New-Object -ComObject Schedule.Service;$s.Connect();"
               L"$f=$s.GetFolder('\\MoonRadial');foreach($n in @(" +
               names + L")){try{$f.GetTask($n).SetSecurityDescriptor('D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;FRFX;;;" + sid +
               L")',0)}catch{}}\" >nul 2>&1";
    // Старые копии помощника удаляет сам помощник при запуске (RemoveOldHelpers): в строке для cmd с правами нет
    // «%» - cmd раскрыл бы его переменными окружения пользователя
    cmd += L"\"";
    std::wstring shell = System32(L"cmd.exe");
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = shell.c_str();
    sei.lpParameters = cmd.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        CloseHandle(sei.hProcess);
    }
    for (auto& a : g_readyAt) a = 0;  // задачи пересозданы - старый ответ недействителен
    bool ok = TaskReady(t);
    if (ok) SetBlocked(t, false);  // задачу поставили заново - пусть попробует снова (антивирус мог передумать)
    return ok;
}

// отметка «задачу не пускает антивирус» - в HKCU: подделать её можно только в сторону лишних запросов UAC
std::wstring BlockedName(Task t) {
    std::wstring n = kTasks[t].name;
    return L"TaskBlocked-" + n.substr(n.find(L'\\') + 1);
}

bool Blocked(Task t) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kKey, BlockedName(t).c_str(), RRF_RT_REG_DWORD, nullptr, &v, &size) ==
               ERROR_SUCCESS &&
           v;
}

void SetBlocked(Task t, bool on) {
    DWORD v = on ? 1 : 0;
    if (on)
        RegSetKeyValueW(HKEY_CURRENT_USER, kKey, BlockedName(t).c_str(), REG_DWORD, &v, sizeof(v));
    else
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, BlockedName(t).c_str());
}

// Только помощник из Program Files: устарел - тот же cmd с правами сначала ставит и сверяет его, потом запускает.
// Сам RadialMenu.exe из папки пользователя с правами не запускается никогда.
bool RunOnce(Task t, bool consent) {
    std::wstring helper = HelperExe(), file, args;
    if (HelperDir().empty() || SelfHash().size() != 64) return false;
    // --consented: задание запущено через окно UAC, то есть с согласия человека, а не задачей, которую может дёрнуть
    // любая программа (ConsentJob)
    std::wstring arg = std::wstring(kTasks[t].arg) + (consent ? L" --consented" : L"");
    if (HelperIntact()) {
        file = helper;
        args = arg;
    } else {
        file = System32(L"cmd.exe");
        args = L"/s /c \"" + InstallCmd() + L" & \"" + helper + L"\" " + arg + L"\"";
    }
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    return ShellExecuteExW(&sei) != FALSE;
}

// Перед запуском - сверка помощника: задачу создал администратор, но файл проверяем каждый раз.
bool Run(Task t) {
    return HelperIntact() &&
           Sys(L"schtasks.exe", L"/Run /TN \"" + std::wstring(kTasks[t].name) + L"\"", nullptr, 15000) == 0;
}

void Put(const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_SZ, value.c_str(),
                    (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring Take(const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    std::wstring v;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) v = buf;
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, name);
    return v;
}

}  // namespace elevate
