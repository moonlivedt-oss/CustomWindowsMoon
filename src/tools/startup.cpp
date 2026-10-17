// Автозагрузка: Run (пользователь и компьютер), папки «Автозагрузка», задачи при входе.
// Выключается так же, как в Диспетчере задач (StartupApproved), - ничего не удаляется, включить можно обратно.
#include <windows.h>
#include <shlobj.h>
#include <taskschd.h>

#include <algorithm>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

constexpr wchar_t kApproved[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
enum Kind { RUN_USER, RUN_MACHINE, RUN_MACHINE32, FOLDER_USER, FOLDER_COMMON, TASK, RUN_ONCE };

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

// Первый байт значения в StartupApproved: чётный - включено, нечётный (03) - выключено.
bool Approved(HKEY root, const std::wstring& sub, const std::wstring& name) {
    BYTE v[16];
    DWORD size = sizeof(v);
    std::wstring key = std::wstring(kApproved) + sub;
    if (RegGetValueW(root, key.c_str(), name.c_str(), RRF_RT_REG_BINARY, nullptr, v, &size) != ERROR_SUCCESS)
        return true;  // записи нет - включено
    return size == 0 || !(v[0] & 1);
}

bool SetApproved(HKEY root, const std::wstring& sub, const std::wstring& name, bool on) {
    std::wstring key = std::wstring(kApproved) + sub;
    HKEY k;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    BYTE v[12] = {};
    v[0] = on ? 0x02 : 0x03;
    if (!on) {  // когда выключили - как пишет Диспетчер задач
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        memcpy(v + 4, &ft, 8);
    }
    LONG rc = RegSetValueExW(k, name.c_str(), 0, REG_BINARY, v, sizeof(v));
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

void ReadRun(HKEY root, const wchar_t* key, REGSAM view, Kind kind, const wchar_t* where, const wchar_t* sub,
             std::vector<StartItem>& out) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD nlen = 512, type = 0, size = 0;
        if (RegEnumValueW(k, i, name, &nlen, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        std::wstring val(size / sizeof(wchar_t) + 1, L'\0');
        DWORD sz = (DWORD)(val.size() * sizeof(wchar_t)), t2 = 0;
        nlen = 512;
        RegEnumValueW(k, i, name, &nlen, nullptr, &t2, (BYTE*)val.data(), &sz);
        val.resize(wcslen(val.c_str()));
        StartItem it;
        it.name = name;
        it.command = val;
        it.where = where;
        it.kind = kind;
        it.key = name;
        it.enabled = Approved(root == HKEY_CURRENT_USER ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, sub, name);
        it.canToggle = kind == RUN_USER;
        out.push_back(it);
    }
    RegCloseKey(k);
}

void ReadFolder(REFKNOWNFOLDERID id, Kind kind, const wchar_t* where, std::vector<StartItem>& out) {
    PWSTR dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &dir))) return;
    std::wstring d = dir;
    CoTaskMemFree(dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;
        StartItem it;
        it.name = fd.cFileName;
        if (EndsWithI(it.name, L".lnk")) it.name.resize(it.name.size() - 4);
        it.command = d + L"\\" + fd.cFileName;
        it.where = where;
        it.kind = kind;
        it.key = fd.cFileName;
        it.enabled = Approved(kind == FOLDER_USER ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, L"StartupFolder", it.key);
        it.canToggle = kind == FOLDER_USER;
        out.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Задачи планировщика с запуском при входе; системные задачи Microsoft не показываем.
void ReadTasks(ITaskFolder* folder, std::vector<StartItem>& out, int depth) {
    if (!folder || depth > 6) return;
    BSTR path = nullptr;
    folder->get_Path(&path);
    std::wstring fp = path ? path : L"";
    SysFreeString(path);
    if (Lower(fp).rfind(L"\\microsoft", 0) == 0) return;
    IRegisteredTaskCollection* tasks = nullptr;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks))) {
        LONG n = 0;
        tasks->get_Count(&n);
        for (LONG i = 1; i <= n; ++i) {
            IRegisteredTask* t = nullptr;
            VARIANT idx;
            idx.vt = VT_I4;
            idx.lVal = i;
            if (FAILED(tasks->get_Item(idx, &t))) continue;
            ITaskDefinition* def = nullptr;
            bool logon = false;
            std::wstring cmd;
            if (SUCCEEDED(t->get_Definition(&def))) {
                ITriggerCollection* trig = nullptr;
                if (SUCCEEDED(def->get_Triggers(&trig))) {
                    LONG tn = 0;
                    trig->get_Count(&tn);
                    for (LONG j = 1; j <= tn; ++j) {
                        ITrigger* tr = nullptr;
                        if (SUCCEEDED(trig->get_Item(j, &tr))) {
                            TASK_TRIGGER_TYPE2 type;
                            if (SUCCEEDED(tr->get_Type(&type)) &&
                                (type == TASK_TRIGGER_LOGON || type == TASK_TRIGGER_BOOT))
                                logon = true;
                            tr->Release();
                        }
                    }
                    trig->Release();
                }
                IActionCollection* acts = nullptr;
                if (SUCCEEDED(def->get_Actions(&acts))) {
                    IAction* a = nullptr;
                    if (SUCCEEDED(acts->get_Item(1, &a))) {
                        IExecAction* ex = nullptr;
                        if (SUCCEEDED(a->QueryInterface(IID_IExecAction, (void**)&ex))) {
                            BSTR p = nullptr;
                            ex->get_Path(&p);
                            if (p) cmd = p;
                            // путь в задаче часто записан в кавычках (иногда двойных) - для показа они лишние
                            while (cmd.size() > 1 && cmd.front() == L'"' && cmd.back() == L'"')
                                cmd = cmd.substr(1, cmd.size() - 2);
                            SysFreeString(p);
                            ex->Release();
                        }
                        a->Release();
                    }
                    acts->Release();
                }
                def->Release();
            }
            if (logon) {
                BSTR name = nullptr, tpath = nullptr;
                t->get_Name(&name);
                t->get_Path(&tpath);
                VARIANT_BOOL en = VARIANT_FALSE;
                t->get_Enabled(&en);
                StartItem it;
                it.name = name ? name : L"";
                it.command = cmd;
                // одинаковые имена в разных папках планировщика (старая и новая) различаются папкой
                it.where = fp.size() > 1 ? L"Задача при входе (" + fp.substr(1) + L")" : L"Задача при входе";
                it.kind = TASK;
                it.key = tpath ? tpath : L"";
                it.enabled = en != VARIANT_FALSE;
                it.canToggle = true;  // свои задачи переключаются; чужие с правами - скажет ошибка
                out.push_back(it);
                SysFreeString(name);
                SysFreeString(tpath);
            }
            t->Release();
        }
        tasks->Release();
    }
    ITaskFolderCollection* subs = nullptr;
    if (SUCCEEDED(folder->GetFolders(0, &subs))) {
        LONG n = 0;
        subs->get_Count(&n);
        for (LONG i = 1; i <= n; ++i) {
            ITaskFolder* f = nullptr;
            VARIANT idx;
            idx.vt = VT_I4;
            idx.lVal = i;
            if (SUCCEEDED(subs->get_Item(idx, &f))) {
                ReadTasks(f, out, depth + 1);
                f->Release();
            }
        }
        subs->Release();
    }
}

}  // namespace

std::vector<StartItem> StartupItems() {
    ComScope com;
    std::vector<StartItem> out;
    constexpr wchar_t run[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    ReadRun(HKEY_CURRENT_USER, run, 0, RUN_USER, L"Run, пользователь", L"Run", out);
    ReadRun(HKEY_LOCAL_MACHINE, run, KEY_WOW64_64KEY, RUN_MACHINE, L"Run, компьютер", L"Run", out);
    ReadRun(HKEY_LOCAL_MACHINE, run, KEY_WOW64_32KEY, RUN_MACHINE32, L"Run, компьютер (32 бита)", L"Run32", out);
    // RunOnce: выполнится при следующем входе и сотрётся - любимое место закрепления вредоносов, «Защита» смотрит и
    // сюда; переключать нечего (Диспетчер задач их тоже не показывает)
    constexpr wchar_t once[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    ReadRun(HKEY_CURRENT_USER, once, 0, RUN_ONCE, L"RunOnce, пользователь", L"RunOnce", out);
    ReadRun(HKEY_LOCAL_MACHINE, once, KEY_WOW64_64KEY, RUN_ONCE, L"RunOnce, компьютер", L"RunOnce", out);
    ReadFolder(FOLDERID_Startup, FOLDER_USER, L"Папка «Автозагрузка»", out);
    ReadFolder(FOLDERID_CommonStartup, FOLDER_COMMON, L"Общая папка «Автозагрузка»", out);
    ITaskService* svc = nullptr;
    if (SUCCEEDED(
            CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc))) {
        VARIANT empty{};
        if (SUCCEEDED(svc->Connect(empty, empty, empty, empty))) {
            ITaskFolder* root = nullptr;
            BSTR r = SysAllocString(L"\\");
            if (SUCCEEDED(svc->GetFolder(r, &root))) {
                ReadTasks(root, out, 0);
                root->Release();
            }
            SysFreeString(r);
        }
        svc->Release();
    }
    return out;
}

bool SetStartup(const StartItem& it, bool on, const Log& log) {
    bool ok = false;
    switch (it.kind) {
        case RUN_USER:
            ok = SetApproved(HKEY_CURRENT_USER, L"Run", it.key, on);
            break;
        case FOLDER_USER:
            ok = SetApproved(HKEY_CURRENT_USER, L"StartupFolder", it.key, on);
            break;
        case TASK: {
            ComScope com;
            ITaskService* svc = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                           (void**)&svc))) {
                VARIANT empty{};
                ITaskFolder* root = nullptr;
                BSTR r = SysAllocString(L"\\");
                if (SUCCEEDED(svc->Connect(empty, empty, empty, empty)) && SUCCEEDED(svc->GetFolder(r, &root))) {
                    IRegisteredTask* t = nullptr;
                    BSTR p = SysAllocString(it.key.c_str());
                    if (SUCCEEDED(root->GetTask(p, &t))) {
                        ok = SUCCEEDED(t->put_Enabled(on ? VARIANT_TRUE : VARIANT_FALSE));
                        t->Release();
                    }
                    SysFreeString(p);
                    root->Release();
                }
                SysFreeString(r);
                svc->Release();
            }
            break;
        }
        default:
            log(it.name + L": это включается для всего компьютера - переключи в Диспетчере задач (нужны права).");
            return false;
    }
    log(ok ? it.name + (on ? L": снова запускается с Windows." : L": больше не запускается с Windows.")
           : it.name + L": переключить не получилось (возможно, нужны права).");
    return ok;
}

}  // namespace tools
