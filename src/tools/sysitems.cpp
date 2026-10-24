// Сторож новых служб, драйверов и задач планировщика (И17): как с автозапуском - снимок в sysitems.tsv, новое против
// прошлого снимка - карточка «Оставить / Выключить». Службы и драйверы выключаются задачей с правами
// (JOB_SYS_ITEM: только не Microsoft и не своё), задачи - сами, если позволяют права.
#include <windows.h>
#include <taskschd.h>

#include <algorithm>
#include <set>

#include "core/elevate.h"
#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::wstring StartName(DWORD t) {
    switch (t) {
        case SERVICE_BOOT_START:
            return L"при загрузке ядра";
        case SERVICE_SYSTEM_START:
            return L"при старте системы";
        case SERVICE_AUTO_START:
            return L"автоматически";
        case SERVICE_DEMAND_START:
            return L"вручную";
        default:
            return L"отключена";
    }
}

void ReadServices(std::vector<SysItem>& out) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return;
    DWORD need = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL, nullptr, 0,
                          &need, &count, &resume, nullptr);
    std::vector<BYTE> buf(need + 4096);
    resume = 0;
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL, buf.data(),
                              (DWORD)buf.size(), &need, &count, &resume, nullptr)) {
        auto* s = (ENUM_SERVICE_STATUS_PROCESSW*)buf.data();
        for (DWORD i = 0; i < count; ++i) {
            // службы пользователя (CDPUserSvc_4a5b6) получают новый хвост при каждом входе - не новость
            if (s[i].ServiceStatusProcess.dwServiceType & 0x80 /* SERVICE_USERSERVICE_INSTANCE */) continue;
            SysItem it;
            it.kind = (s[i].ServiceStatusProcess.dwServiceType & SERVICE_DRIVER) ? L"drv" : L"svc";
            it.name = s[i].lpServiceName;
            it.display = s[i].lpDisplayName ? s[i].lpDisplayName : it.name;
            if (SC_HANDLE h = OpenServiceW(scm, it.name.c_str(), SERVICE_QUERY_CONFIG)) {
                DWORD n = 0;
                QueryServiceConfigW(h, nullptr, 0, &n);
                std::vector<BYTE> cb(n + 16);
                auto* qc = (QUERY_SERVICE_CONFIGW*)cb.data();
                if (n && QueryServiceConfigW(h, qc, (DWORD)cb.size(), &n)) {
                    it.command = qc->lpBinaryPathName ? qc->lpBinaryPathName : L"";
                    it.start = (int)qc->dwStartType;
                    it.how = StartName(qc->dwStartType);
                }
                CloseServiceHandle(h);
            }
            it.path = ImageOf(it.command);
            out.push_back(it);
        }
    }
    CloseServiceHandle(scm);
}

void ReadTasks(ITaskFolder* folder, std::vector<SysItem>& out, int depth) {
    if (!folder || depth > 6) return;
    BSTR path = nullptr;
    folder->get_Path(&path);
    std::wstring fp = path ? path : L"";
    SysFreeString(path);
    // задачи самой Windows приходят и уходят с обновлениями - шум; свои - не новость
    if (Lower(fp).rfind(L"\\microsoft", 0) == 0 || Lower(fp).rfind(L"\\moonradial", 0) == 0 ||
        Lower(fp).rfind(L"\\moondesk", 0) == 0)
        return;
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
            SysItem it;
            it.kind = L"task";
            bool logon = false;
            ITaskDefinition* def = nullptr;
            if (SUCCEEDED(t->get_Definition(&def))) {
                ITriggerCollection* trig = nullptr;
                if (SUCCEEDED(def->get_Triggers(&trig))) {
                    LONG tn = 0;
                    trig->get_Count(&tn);
                    for (LONG j = 1; j <= tn; ++j) {
                        ITrigger* tr = nullptr;
                        if (SUCCEEDED(trig->get_Item(j, &tr))) {
                            TASK_TRIGGER_TYPE2 type;
                            if (SUCCEEDED(tr->get_Type(&type))) {
                                if (type == TASK_TRIGGER_LOGON || type == TASK_TRIGGER_BOOT) logon = true;
                                const wchar_t* w = type == TASK_TRIGGER_TIME || type == TASK_TRIGGER_DAILY ||
                                                           type == TASK_TRIGGER_WEEKLY || type == TASK_TRIGGER_MONTHLY
                                                       ? L"по расписанию"
                                                   : type == TASK_TRIGGER_IDLE  ? L"при простое"
                                                   : type == TASK_TRIGGER_EVENT ? L"по событию"
                                                                                : L"";
                                if (*w && it.how.find(w) == std::wstring::npos)
                                    it.how += (it.how.empty() ? L"" : L", ") + std::wstring(w);
                            }
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
                            BSTR p = nullptr, args = nullptr;
                            ex->get_Path(&p);
                            ex->get_Arguments(&args);
                            it.command = std::wstring(p ? p : L"") + (args && *args ? L" " + std::wstring(args) : L"");
                            SysFreeString(p);
                            SysFreeString(args);
                            ex->Release();
                        }
                        a->Release();
                    }
                    acts->Release();
                }
                def->Release();
            }
            BSTR name = nullptr, tpath = nullptr;
            t->get_Name(&name);
            t->get_Path(&tpath);
            it.display = name ? name : L"";
            it.name = tpath ? tpath : L"";
            SysFreeString(name);
            SysFreeString(tpath);
            it.path = ImageOf(it.command);
            if (it.how.empty()) it.how = L"по запросу";
            if (!logon) out.push_back(it);  // при входе и загрузке - это автозапуск, его ловит NewStartupItems
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

ITaskService* Connect() {
    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc)))
        return nullptr;
    VARIANT empty{};
    if (FAILED(svc->Connect(empty, empty, empty, empty))) {
        svc->Release();
        return nullptr;
    }
    return svc;
}

}  // namespace

std::wstring ImageOf(const std::wstring& command) {
    std::wstring c = Trim(command);
    std::wstring exe;
    if (!c.empty() && c[0] == L'"') {
        size_t e = c.find(L'"', 1);
        exe = c.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
    } else {
        std::wstring low = Lower(c);
        size_t cut = std::wstring::npos;
        for (const wchar_t* ext : {L".exe", L".sys", L".dll"}) {
            size_t at = low.find(ext);
            if (at != std::wstring::npos && (cut == std::wstring::npos || at < cut)) cut = at + 4;
        }
        exe = cut == std::wstring::npos ? c.substr(0, c.find(L' ')) : c.substr(0, cut);
    }
    // пути драйверов: \SystemRoot\..., \??\C:\..., System32\drivers\x.sys без диска
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring low = Lower(exe);
    if (low.rfind(L"\\??\\", 0) == 0)
        exe = exe.substr(4);
    else if (low.rfind(L"\\systemroot\\", 0) == 0)
        exe = std::wstring(win) + exe.substr(11);
    else if (low.rfind(L"system32\\", 0) == 0)
        exe = std::wstring(win) + L"\\" + exe;
    wchar_t full[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(exe.c_str(), full, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? std::wstring(full) : exe;
}

std::vector<SysItem> SysItems() {
    ComScope com;
    std::vector<SysItem> out;
    ReadServices(out);
    if (ITaskService* svc = Connect()) {
        ITaskFolder* root = nullptr;
        BSTR r = SysAllocString(L"\\");
        if (SUCCEEDED(svc->GetFolder(r, &root))) {
            ReadTasks(root, out, 0);
            root->Release();
        }
        SysFreeString(r);
        svc->Release();
    }
    return out;
}

std::wstring SysItemKey(const SysItem& it) {
    // версия и GUID в хвосте имени (GoogleUpdaterService157.0.8089.0, ...TaskMachineCore{GUID}) меняются с каждым
    // обновлением программы - это та же служба
    std::wstring n = Lower(it.name);
    if (!n.empty() && n.back() == L'}') {
        size_t open = n.rfind(L'{');
        if (open != std::wstring::npos) n.erase(open);
    }
    while (!n.empty() && (iswdigit(n.back()) || n.back() == L'.' || n.back() == L'_' || n.back() == L' ')) n.pop_back();
    return it.kind + L"|" + n;
}

std::vector<SysItem> NewSysItems() {
    std::vector<SysItem> items = SysItems(), fresh;
    if (items.empty()) return fresh;  // не прочиталось - снимок не трогаем, иначе всё станет «новым»
    std::wstring file = DataDir() + L"\\sysitems.tsv";
    std::set<std::wstring> known;
    bool first = GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (!first) {
        std::string bytes;
        HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char chunk[8192];
            DWORD got = 0;
            while (ReadFile(f, chunk, sizeof(chunk), &got, nullptr) && got) bytes.append(chunk, got);
            CloseHandle(f);
        }
        for (const std::wstring& line : Split(Widen(bytes), L'\n')) {
            std::wstring key = line.substr(0, line.find(L'\t'));
            if (!key.empty()) known.insert(key);
        }
    }
    std::wstring all;
    for (SysItem& it : items) {
        std::wstring key = SysItemKey(it);
        all += key + L"\t" + it.command + L"\n";
        if (first || known.count(key)) continue;
        // обновления Windows ставят свои службы и драйверы - подписанное Microsoft не новость
        std::wstring signer;
        it.sign = CheckSign(it.path, &signer);
        it.signer = signer;
        if (it.sign == SIGN_OK && Lower(signer).find(L"microsoft") != std::wstring::npos) continue;
        if (Lower(it.path).find(L"radialmenu") != std::wstring::npos) continue;
        fresh.push_back(it);
    }
    std::string utf = Narrow(all);
    std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        BOOL ok = WriteFile(h, utf.data(), (DWORD)utf.size(), &put, nullptr);
        CloseHandle(h);
        if (ok) MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return fresh;
}

std::wstring SysItemRefuse(const SysItem& it) {
    if (it.kind != L"svc" && it.kind != L"drv") return L"это не служба и не драйвер";
    if (it.name.empty() || it.name.find_first_of(L"\\/\"|") != std::wstring::npos) return L"странное имя";
    std::wstring low = Lower(it.path);
    if (low.find(L"radialmenu") != std::wstring::npos || low.find(L"\\moonradial\\") != std::wstring::npos)
        return L"это сама RadialMenu";
    if (SecurityVendor(low)) return L"это защита компьютера";
    std::wstring signer;
    if (CheckSign(it.path, &signer) == SIGN_OK) {
        std::wstring ls = Lower(signer);
        if (ls.find(L"microsoft") != std::wstring::npos) return L"служба Windows (подпись Microsoft)";
        if (SecurityVendor(ls)) return L"это защита компьютера (по подписи)";
    }
    return L"";
}

bool SetTaskEnabled(const std::wstring& path, bool on) {
    ComScope com;
    bool ok = false;
    if (ITaskService* svc = Connect()) {
        ITaskFolder* root = nullptr;
        BSTR r = SysAllocString(L"\\");
        if (SUCCEEDED(svc->GetFolder(r, &root))) {
            IRegisteredTask* t = nullptr;
            BSTR p = SysAllocString(path.c_str());
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
    return ok;
}

namespace {

std::wstring OffFile() { return elevate::DataDir() + L"\\sysitems-off.tsv"; }

std::wstring ReadOff() {
    HANDLE f = CreateFileW(OffFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
    CloseHandle(f);
    return Widen(bytes);
}

bool WriteOff(const std::wstring& table) {
    std::string bytes = Narrow(table);
    std::wstring file = OffFile(), tmp = file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Защищённая служба (PPL антивируса или EDR): Windows сама не даёт её трогать даже администратору - и мы не пробуем
bool LaunchProtected(SC_HANDLE h) {
    struct Info {
        DWORD dwLaunchProtected;
    } info{};
    DWORD need = 0;
    return QueryServiceConfig2W(h, 12 /* SERVICE_CONFIG_LAUNCH_PROTECTED */, (BYTE*)&info, sizeof(info), &need) &&
           info.dwLaunchProtected != 0;
}

}  // namespace

// Задача с правами: запрос «svc|имя» или «drv|имя» (выключить), «on|svc|имя» (вернуть как было). Запрос пишет
// обычный процесс - вернуть можно только выключенное этой задачей и только с записанным ею типом запуска.
bool SysItemAdminMain(const std::wstring& req, const Log& log) {
    std::vector<std::wstring> p = Split(req, L'|');
    bool back = !p.empty() && p[0] == L"on";
    if (back) p.erase(p.begin());
    if (p.size() < 2) {
        log(L"Не понял, что выключить.");
        return false;
    }
    SysItem it;
    it.kind = p[0];
    it.name = p[1];
    std::wstring table = back ? ReadOff() : L"";
    DWORD recorded = 0;
    if (back && !SysOffTake(table, it.kind, it.name, recorded)) {
        log(L"Не трогаю: " + it.name + L" выключали не мы - возвращать нечего.");
        return false;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE h =
        scm ? OpenServiceW(scm, it.name.c_str(), SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG | SERVICE_STOP) : nullptr;
    if (!h) {
        log(L"Службы " + it.name + L" нет или к ней нет доступа.");
        if (scm) CloseServiceHandle(scm);
        return false;
    }
    DWORD n = 0;
    QueryServiceConfigW(h, nullptr, 0, &n);
    std::vector<BYTE> cb(n + 16);
    auto* qc = (QUERY_SERVICE_CONFIGW*)cb.data();
    DWORD type = 0;
    if (n && QueryServiceConfigW(h, qc, (DWORD)cb.size(), &n)) {
        it.command = qc->lpBinaryPathName ? qc->lpBinaryPathName : L"";
        it.path = ImageOf(it.command);
        it.start = (int)qc->dwStartType;
        type = qc->dwServiceType;
    }
    // проверка заново, здесь: запрос пришёл от обычного процесса
    std::wstring why = it.command.empty() ? L"не прочитать, что это за служба" : SysItemRefuse(it);
    bool driver = (type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER)) != 0;
    if (why.empty() && driver != (it.kind == L"drv")) why = L"вид не совпадает: служба выдана за драйвер или наоборот";
    if (why.empty() && LaunchProtected(h)) why = L"защищённая служба (антивирус или EDR)";
    bool ok = false;
    if (!why.empty()) {
        log(L"Не трогаю: " + why + L".");
    } else if (back) {
        ok = ChangeServiceConfigW(h, SERVICE_NO_CHANGE, recorded, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr);
        if (ok && !WriteOff(table)) log(L"Список выключенного не обновился.");
        log(ok ? it.name + L": запуск снова «" + StartName(recorded) + L"»." : it.name + L": вернуть не получилось.");
    } else {
        ok = ChangeServiceConfigW(h, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr);
        if (ok && it.kind == L"svc") {
            SERVICE_STATUS st{};
            ControlService(h, SERVICE_CONTROL_STOP, &st);
        }
        log(ok ? it.name + L": отключена (было «" + StartName((DWORD)it.start) + L"»)." +
                     (it.kind == L"drv" ? L" Драйвер перестанет грузиться после перезагрузки." : L"")
               : it.name + L": отключить не получилось.");
        if (ok && (DWORD)it.start != SERVICE_DISABLED) {
            CreateDirectoryW(elevate::DataDir().c_str(), nullptr);
            if (!WriteOff(SysOffAdd(ReadOff(), it.kind, it.name, (DWORD)it.start)))
                log(L"Не записалось, как было: вернуть - вручную (sc config).");
        }
        if (ok) log(L"@start=" + std::to_wstring(it.start));
    }
    CloseServiceHandle(h);
    CloseServiceHandle(scm);
    return ok;
}

}  // namespace tools
