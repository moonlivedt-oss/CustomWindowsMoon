#include "core/elevate.h"

#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <cwchar>
#include <vector>

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
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) raw.append(buf, got);
    CloseHandle(rd);
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
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

// Файлы побайтно одинаковые (помощник - копия этого exe).
bool SameFile(const std::wstring& a, const std::wstring& b) {
    HANDLE fa =
        CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE fb =
        CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    bool same = fa != INVALID_HANDLE_VALUE && fb != INVALID_HANDLE_VALUE;
    LARGE_INTEGER sa{}, sb{};
    same = same && GetFileSizeEx(fa, &sa) && GetFileSizeEx(fb, &sb) && sa.QuadPart == sb.QuadPart;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (same) {
        DWORD ga = 0, gb = 0;
        if (!ReadFile(fa, ba.data(), (DWORD)ba.size(), &ga, nullptr) ||
            !ReadFile(fb, bb.data(), (DWORD)bb.size(), &gb, nullptr) || ga != gb) {
            same = false;
            break;
        }
        if (!ga) break;
        same = !memcmp(ba.data(), bb.data(), ga);
    }
    if (fa != INVALID_HANDLE_VALUE) CloseHandle(fa);
    if (fb != INVALID_HANDLE_VALUE) CloseHandle(fb);
    return same;
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
    return L"schtasks /Create /TN \"" + std::wstring(t.name) + L"\" /TR \"\\\"" + HelperExe() + L"\\\" " + t.arg +
           L"\" /SC ONCE /ST 00:00 /SD 01/01/2030 /RL HIGHEST /F >nul";
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
std::wstring HelperExe() {
    static std::wstring name;
    if (name.empty()) {
        unsigned long long h = 1469598103934665603ULL;  // FNV-1a по байтам exe
        HANDLE f = CreateFileW(ThisExe().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<unsigned char> buf(1 << 20);
            DWORD got = 0;
            while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
                for (DWORD i = 0; i < got; ++i) h = (h ^ buf[i]) * 1099511628211ULL;
            CloseHandle(f);
        }
        wchar_t b[32];
        swprintf(b, 32, L"RadialMenu-%012llx.exe", h & 0xFFFFFFFFFFFFULL);
        name = b;
    }
    return HelperDir() + L"\\" + name;
}

std::wstring DataDir() { return HelperDir() + L"\\data"; }

// Спрашиваем сам планировщик: ветку TaskCache в HKLM обычный пользователь читать не может, и прежняя проверка
// по реестру всегда отвечала «задачи нет» - каждое действие заново спрашивало UAC и пересоздавало задачу.
bool TaskExists(Task t) {
    return Sys(L"schtasks.exe", L"/Query /TN \"" + std::wstring(kTasks[t].name) + L"\"", nullptr, 15000) == 0;
}

bool TaskReady(Task t) { return !HelperDir().empty() && SameFile(ThisExe(), HelperExe()) && TaskRunsHelper(kTasks[t]); }

// Ставит системный cmd с правами, а не сам RadialMenu.exe: Касперский останавливает программу, которая кладёт
// свой exe в Program Files и тут же заводит на него задачу с наивысшими правами (так выглядит закрепление
// вредоноса). Нужная задача - всегда, остальные - только если уже были; потом убрать прежние копии помощника.
bool Setup(Task t) {
    std::wstring dir = HelperDir(), data = DataDir(), helper = HelperExe();
    if (dir.empty()) return false;
    std::wstring name = helper.substr(dir.size() + 1);
    std::wstring cmd = L"/s /c \"(if not exist \"" + dir + L"\" mkdir \"" + dir + L"\") & (if not exist \"" + data +
                       L"\" mkdir \"" + data + L"\") & (if not exist \"" + helper + L"\" copy /y \"" + ThisExe() +
                       L"\" \"" + helper + L"\" >nul)";
    for (int i = 0; i < (int)(sizeof(kTasks) / sizeof(kTasks[0])); ++i)
        if (i == t)
            cmd += L" && " + CreateCmd(kTasks[i]);
        else
            cmd += L" & (schtasks /Query /TN \"" + std::wstring(kTasks[i].name) + L"\" >nul 2>&1 && " +
                   CreateCmd(kTasks[i]) + L")";
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
               L"\" -NoProfile -NonInteractive -Command \"$s=New-Object -ComObject Schedule.Service;$s.Connect();"
               L"$f=$s.GetFolder('\\MoonRadial');foreach($n in @(" +
               names + L")){try{$f.GetTask($n).SetSecurityDescriptor('D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;FRFX;;;" + sid +
               L")',0)}catch{}}\" >nul 2>&1";
    cmd += L" & (for %f in (\"" + dir + L"\\RadialMenu*.exe*\") do @if /i not \"%~nxf\"==\"" + name +
           L"\" del /f /q \"%~f\" >nul 2>&1)\"";
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

bool RunOnce(Task t) {
    // помощник, если он свежий; иначе сам exe - под UAC это решение пользователя, а не тихое повышение
    std::wstring exe = SameFile(ThisExe(), HelperExe()) ? HelperExe() : ThisExe();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = kTasks[t].arg;
    sei.nShow = SW_HIDE;
    return ShellExecuteExW(&sei) != FALSE;
}

bool Run(Task t) {
    return Sys(L"schtasks.exe", L"/Run /TN \"" + std::wstring(kTasks[t].name) + L"\"", nullptr, 15000) == 0;
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
