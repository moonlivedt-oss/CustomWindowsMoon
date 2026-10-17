#include "core/elevate.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <vector>

namespace elevate {

namespace {

constexpr wchar_t kKey[] = L"Software\\MoonRadial";

struct TaskInfo {
    const wchar_t* name;   // имя в планировщике
    const wchar_t* arg;    // что выполняет помощник
    const wchar_t* which;  // как её назвать в --install-helper
};
const TaskInfo kTasks[] = {
    {L"MoonRadial\\RadialMenuNet", L"--net-job", L"net"},
    {L"MoonRadial\\RadialMenuPurge", L"--purge", L"purge"},
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

bool Create(const TaskInfo& t) {
    // запускается только по требованию: дата старта - далёкое будущее
    std::wstring args = L"/Create /TN \"" + std::wstring(t.name) + L"\" /TR \"\\\"" + HelperExe() + L"\\\" " + t.arg +
                        L"\" /SC ONCE /ST 00:00 /SD 01/01/2030 /RL HIGHEST /F";
    return Sys(L"schtasks.exe", args, nullptr, 20000) == 0;
}

}  // namespace

std::wstring HelperDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p)) && p) dir = std::wstring(p);
    CoTaskMemFree(p);
    return dir.empty() ? L"" : dir + L"\\MoonRadial";
}

std::wstring HelperExe() { return HelperDir() + L"\\RadialMenu.exe"; }

std::wstring DataDir() { return HelperDir() + L"\\data"; }

// Спрашиваем сам планировщик: ветку TaskCache в HKLM обычный пользователь читать не может, и прежняя проверка
// по реестру всегда отвечала «задачи нет» - каждое действие заново спрашивало UAC и пересоздавало задачу.
bool TaskExists(Task t) {
    return Sys(L"schtasks.exe", L"/Query /TN \"" + std::wstring(kTasks[t].name) + L"\"", nullptr, 15000) == 0;
}

bool TaskReady(Task t) { return !HelperDir().empty() && SameFile(ThisExe(), HelperExe()) && TaskRunsHelper(kTasks[t]); }

bool Setup(Task t) {
    std::wstring exe = ThisExe(), args = std::wstring(L"--install-helper ") + kTasks[t].which;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        CloseHandle(sei.hProcess);
    }
    return TaskReady(t);
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

int InstallMain(const std::wstring& which) {
    int want = -1;
    for (int i = 0; i < (int)(sizeof(kTasks) / sizeof(kTasks[0])); ++i)
        if (which == kTasks[i].which) want = i;
    std::wstring dir = HelperDir();
    if (want < 0 || dir.empty()) return 2;
    CreateDirectoryW(dir.c_str(), nullptr);
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return 3;
    // копия рядом, потом подмена: помощник может как раз выполнять задание - тогда подождать
    std::wstring tmp = HelperExe() + L".new";
    bool copied = false;
    for (int i = 0; i < 40 && !copied; ++i) {
        copied = CopyFileW(ThisExe().c_str(), tmp.c_str(), FALSE) &&
                 MoveFileExW(tmp.c_str(), HelperExe().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!copied) Sleep(250);
    }
    if (!copied) {
        DeleteFileW(tmp.c_str());
        return 4;
    }
    CreateDirectoryW(DataDir().c_str(), nullptr);
    // нужная задача - всегда, остальные - только если уже были: старые смотрели на exe в папке пользователя
    bool ok = true;
    for (int i = 0; i < (int)(sizeof(kTasks) / sizeof(kTasks[0])); ++i)
        if (i == want || TaskExists((Task)i)) ok = Create(kTasks[i]) && ok;
    return ok ? 0 : 5;
}

}  // namespace elevate
