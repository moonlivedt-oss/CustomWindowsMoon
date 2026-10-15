#include "app/autostart.h"

#include <string>

#include <windows.h>
#include <taskschd.h>

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunName[] = L"MoonRadialMenu";
constexpr wchar_t kTaskFolder[] = L"\\MoonDesk";
constexpr wchar_t kTaskName[] = L"RadialMenuStart";

std::wstring ExePath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return exe;
}

std::wstring ExeCommand() { return L"\"" + ExePath() + L"\""; }

// ---- запасной путь: HKCU\...\Run (Проводник запускает такие программы с задержкой) ----

std::wstring RunValue() {
    wchar_t buf[MAX_PATH + 16];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// Диспетчер задач выключает автозапуск не удаляя Run, а флагом в StartupApproved:
// первый байт нечётный (03) - выключено, чётный (02) - включено.
bool DisabledByTaskManager() {
    BYTE v[12];
    DWORD size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunName, RRF_RT_REG_BINARY, nullptr, v, &size) != ERROR_SUCCESS)
        return false;
    return size > 0 && (v[0] & 1);
}

void ClearApproved() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, kRunName);
    RegCloseKey(k);
}

void WriteRun(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = ExeCommand();
        RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunName);
    }
    RegCloseKey(k);
}

// ---- основной путь: задача планировщика «при входе» ----
// Стартует сразу после входа, раньше программ из Run, - портал успевает показаться.
// Для своей учётной записи прав администратора не нужно.

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'&')
            out += L"&amp;";
        else if (c == L'<')
            out += L"&lt;";
        else if (c == L'>')
            out += L"&gt;";
        else
            out += c;
    }
    return out;
}

std::wstring UserId() {
    wchar_t dom[256] = L"", user[256] = L"";
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    return std::wstring(dom) + L"\\" + user;
}

std::wstring TaskXml() {
    std::wstring exe = ExePath(), dir = exe.substr(0, exe.find_last_of(L'\\'));
    // Priority 4 - обычный приоритет: по умолчанию задачи идут с 7 (ниже обычного, и диск тоже)
    return L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
           L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
           L"<RegistrationInfo><Description>Radial Menu: запуск при входе в Windows</Description></RegistrationInfo>"
           L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" +
           XmlEscape(UserId()) +
           L"</UserId></LogonTrigger></Triggers>"
           L"<Principals><Principal id=\"Author\"><UserId>" +
           XmlEscape(UserId()) +
           L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></"
           L"Principals>"
           L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
           L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
           L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
           L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
           L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
           L"<Enabled>true</Enabled><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>"
           L"<Actions Context=\"Author\"><Exec><Command>" +
           XmlEscape(exe) + L"</Command><WorkingDirectory>" + XmlEscape(dir) +
           L"</WorkingDirectory></Exec></Actions></Task>";
}

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
};

bool OpenFolder(Com<ITaskFolder>& folder, bool create) {
    Com<ITaskService> svc;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc.p)))
        return false;
    VARIANT empty{};
    if (FAILED(svc->Connect(empty, empty, empty, empty))) return false;
    BSTR path = SysAllocString(kTaskFolder);
    HRESULT hr = svc->GetFolder(path, &folder.p);
    SysFreeString(path);
    if (SUCCEEDED(hr)) return true;
    if (!create) return false;
    Com<ITaskFolder> root;
    BSTR rootPath = SysAllocString(L"\\");
    hr = svc->GetFolder(rootPath, &root.p);
    SysFreeString(rootPath);
    if (FAILED(hr)) return false;
    BSTR name = SysAllocString(kTaskFolder + 1);
    hr = root->CreateFolder(name, empty, &folder.p);
    SysFreeString(name);
    return SUCCEEDED(hr);
}

// 0 - задачи нет, 1 - есть и включена, -1 - есть, но выключена.
// xml - текст задачи, чтобы сверить путь к exe.
int TaskState(std::wstring* xml = nullptr) {
    Com<ITaskFolder> folder;
    if (!OpenFolder(folder, false)) return 0;
    Com<IRegisteredTask> task;
    BSTR name = SysAllocString(kTaskName);
    HRESULT hr = folder->GetTask(name, &task.p);
    SysFreeString(name);
    if (FAILED(hr)) return 0;
    VARIANT_BOOL en = VARIANT_FALSE;
    task->get_Enabled(&en);
    if (xml) {
        BSTR x = nullptr;
        if (SUCCEEDED(task->get_Xml(&x)) && x) {
            *xml = x;
            SysFreeString(x);
        }
    }
    return en ? 1 : -1;
}

bool RegisterTask() {
    Com<ITaskFolder> folder;
    if (!OpenFolder(folder, true)) return false;
    Com<IRegisteredTask> task;
    BSTR name = SysAllocString(kTaskName);
    BSTR xml = SysAllocString(TaskXml().c_str());
    VARIANT empty{};
    HRESULT hr = folder->RegisterTask(name, xml, TASK_CREATE_OR_UPDATE, empty, empty, TASK_LOGON_INTERACTIVE_TOKEN,
                                      empty, &task.p);
    SysFreeString(xml);
    SysFreeString(name);
    return SUCCEEDED(hr);
}

void DeleteTask() {
    Com<ITaskFolder> folder;
    if (!OpenFolder(folder, false)) return;
    BSTR name = SysAllocString(kTaskName);
    folder->DeleteTask(name, 0);
    SysFreeString(name);
}

// Студия спрашивает состояние каждый кадр: планировщик опрашиваем не чаще раза в 2 с.
bool g_cached = false;
ULONGLONG g_cachedAt = 0;

bool QueryOn() {
    int t = TaskState();
    if (t != 0) return t > 0;
    return !RunValue().empty() && !DisabledByTaskManager();
}
}  // namespace

bool AutostartOn() {
    ULONGLONG now = GetTickCount64();
    if (!g_cachedAt || now - g_cachedAt > 2000) {
        g_cached = QueryOn();
        g_cachedAt = now;
    }
    return g_cached;
}

bool AutostartEarly() { return TaskState() != 0; }

void SetAutostart(bool on) {
    if (on) {
        if (RegisterTask()) {
            WriteRun(false);  // две записи не нужны: вторая копия всё равно вышла бы по мьютексу
        } else {
            WriteRun(true);  // планировщик недоступен - обычный автозапуск
        }
    } else {
        DeleteTask();
        WriteRun(false);
    }
    ClearApproved();  // включение из меню снимает и запрет Диспетчера задач
    g_cachedAt = 0;
}

void AutostartRefresh() {
    std::wstring xml;
    int t = TaskState(&xml);
    if (t != 0) {
        // exe переехал - путь за ним
        if (xml.find(L"<Command>" + XmlEscape(ExePath()) + L"</Command>") == std::wstring::npos) RegisterTask();
        return;
    }
    std::wstring cur = RunValue();
    if (cur.empty() || DisabledByTaskManager()) return;
    if (RegisterTask())
        WriteRun(false);  // старая запись в Run переезжает в планировщик
    else if (cur != ExeCommand())
        WriteRun(true);
    g_cachedAt = 0;
}
