// «Восстановление» - свой Recovery Kit: починка Windows по шагам, сброс Центра обновления, возврат того, что
// ломают вирусы, ожидание перезагрузки, особая загрузка, офлайн-проверка. Всё, что меняет систему, выполняет
// процесс с правами (задача MoonRadial) и пересчитывает, что чинить, сам: из запроса берётся только факт задания.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>

#include "core/elevate.h"
#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

std::wstring RegStr(HKEY root, const std::wstring& key, const wchar_t* name, bool* found = nullptr) {
    DWORD size = 0;
    if (found) *found = false;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr,
                     &size) != ERROR_SUCCESS)
        return {};
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, s.data(),
                     &size) != ERROR_SUCCESS)
        return {};
    s.resize(wcslen(s.c_str()));
    if (found) *found = true;
    return s;
}

DWORD RegDw(HKEY root, const std::wstring& key, const wchar_t* name) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : 0;
}

bool KeyExists(HKEY root, const std::wstring& key) {
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

std::wstring WinDir() {
    wchar_t w[MAX_PATH] = {};
    GetWindowsDirectoryW(w, MAX_PATH);
    return w;
}

std::wstring Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf(b, 32, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

// последние непустые строки вывода - для журнала, без простыни
std::wstring Tail(const std::wstring& out, int lines) {
    std::vector<std::wstring> keep;
    for (const std::wstring& l : Split(out, L'\n')) {
        std::wstring t = Trim(l);
        if (!t.empty() && t.find(L'%') == std::wstring::npos) keep.push_back(t);  // строки прогресса «45.3%» не нужны
    }
    std::wstring s;
    for (size_t i = keep.size() > (size_t)lines ? keep.size() - lines : 0; i < keep.size(); ++i)
        s += (s.empty() ? L"" : L" ") + keep[i];
    return s;
}

constexpr wchar_t kPolSys[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
constexpr wchar_t kPolCmd[] = L"Software\\Policies\\Microsoft\\Windows\\System";
constexpr wchar_t kWinlogon[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
constexpr wchar_t kIfeo[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

// Программы, вместо которых вирусы подсовывают свои (IFEO Debugger): системные инструменты, антивирусы и
// «залипание клавиш» на экране входа (sethc, utilman - классический бэкдор: командная строка без пароля).
const wchar_t* const kIfeoGuard[] = {L"taskmgr.exe",
                                     L"regedit.exe",
                                     L"cmd.exe",
                                     L"powershell.exe",
                                     L"msconfig.exe",
                                     L"mmc.exe",
                                     L"rstrui.exe",
                                     L"avp.exe",
                                     L"avpui.exe",
                                     L"mrt.exe",
                                     L"msmpeng.exe",
                                     L"sethc.exe",
                                     L"utilman.exe",
                                     L"osk.exe",
                                     L"magnify.exe",
                                     L"narrator.exe",
                                     L"displayswitch.exe",
                                     L"atbroker.exe",
                                     L"securityhealthsystray.exe"};

// Свой отладчик для Диспетчера задач (System Informer, Process Explorer) - не поломка.
bool KnownTaskmgrSwap(const std::wstring& dbg) {
    std::wstring l = Lower(dbg);
    return l.find(L"systeminformer") != std::wstring::npos || l.find(L"procexp") != std::wstring::npos ||
           l.find(L"processhacker") != std::wstring::npos;
}

}  // namespace

// ---------------- что сломано ----------------

std::vector<Broken> FindBroken() {
    std::vector<Broken> out;
    auto add = [&](int id, const std::wstring& what, const std::wstring& where, const std::wstring& value, bool admin) {
        out.push_back({id, what, where, value, admin});
    };
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        bool user = root == HKEY_CURRENT_USER;
        std::wstring hive = user ? L"HKCU\\" : L"HKLM\\";
        if (RegDw(root, kPolSys, L"DisableTaskMgr"))
            add(BROKEN_TASKMGR, L"Диспетчер задач выключен политикой", hive + kPolSys, L"DisableTaskMgr = 1", !user);
        if (RegDw(root, kPolSys, L"DisableRegistryTools"))
            add(BROKEN_REGEDIT, L"Редактор реестра выключен политикой", hive + kPolSys, L"DisableRegistryTools", !user);
        if (RegDw(root, kPolCmd, L"DisableCMD"))
            add(BROKEN_CMD, L"Командная строка выключена политикой", hive + kPolCmd, L"DisableCMD", !user);
    }
    // запуск .exe, .bat, .reg перехвачен на уровне пользователя: Windows возьмёт команду отсюда, а не системную
    for (const wchar_t* type : {L"exefile", L"batfile", L"cmdfile", L"comfile", L"regfile", L"piffile"}) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + type + L"\\shell\\open\\command";
        bool found = false;
        std::wstring cmd = RegStr(HKEY_CURRENT_USER, key, L"", &found);
        if (found) add(BROKEN_ASSOC, std::wstring(L"Запуск ") + type + L" перехвачен", L"HKCU\\" + key, cmd, false);
    }
    for (const wchar_t* ext : {L".exe", L".bat", L".cmd", L".com", L".reg"}) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + ext;
        bool found = false;
        std::wstring v = RegStr(HKEY_CURRENT_USER, key, L"", &found);
        if (found && !v.empty())
            add(BROKEN_ASSOC, std::wstring(L"Тип ") + ext + L" переназначен", L"HKCU\\" + key, v, false);
    }
    // кто становится рабочим столом и что запускается при входе
    std::wstring shell = Trim(RegStr(HKEY_LOCAL_MACHINE, kWinlogon, L"Shell"));
    if (!shell.empty() && Lower(shell) != L"explorer.exe")
        add(BROKEN_SHELL, L"Вместо Проводника запускается другое", std::wstring(L"HKLM\\") + kWinlogon, shell, true);
    bool ushellFound = false;
    std::wstring ushell = RegStr(HKEY_CURRENT_USER, kWinlogon, L"Shell", &ushellFound);
    if (ushellFound)
        add(BROKEN_SHELL_USER, L"Для вас вместо Проводника запускается другое", std::wstring(L"HKCU\\") + kWinlogon,
            ushell, false);
    std::wstring ui = Lower(Trim(RegStr(HKEY_LOCAL_MACHINE, kWinlogon, L"Userinit")));
    std::wstring want = Lower(WinDir() + L"\\system32\\userinit.exe,");
    if (!ui.empty() && ui != want && ui != Lower(WinDir() + L"\\system32\\userinit.exe"))
        add(BROKEN_USERINIT, L"При входе вместе с userinit запускается другое", std::wstring(L"HKLM\\") + kWinlogon, ui,
            true);
    for (const wchar_t* exe : kIfeoGuard) {
        std::wstring key = std::wstring(kIfeo) + L"\\" + exe;
        std::wstring dbg = Trim(RegStr(HKEY_LOCAL_MACHINE, key, L"Debugger"));
        if (dbg.empty() || (!_wcsicmp(exe, L"taskmgr.exe") && KnownTaskmgrSwap(dbg))) continue;
        add(BROKEN_IFEO, std::wstring(L"Вместо ") + exe + L" запускается другое", L"HKLM\\" + key, dbg, true);
    }
    return out;
}

// HKCU - без прав: удалить подменённое значение или раздел, Windows вернётся к системному.
int FixBrokenUser(const std::vector<Broken>& list, const Log& log) {
    int fixed = 0;
    for (const Broken& b : list) {
        if (b.admin) continue;
        std::wstring key = b.where.substr(5);  // без «HKCU\»
        LSTATUS rc = ERROR_FILE_NOT_FOUND;
        if (b.id == BROKEN_TASKMGR)
            rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"DisableTaskMgr");
        else if (b.id == BROKEN_REGEDIT)
            rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"DisableRegistryTools");
        else if (b.id == BROKEN_CMD)
            rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"DisableCMD");
        else if (b.id == BROKEN_ASSOC)
            rc = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS
                     ? RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str())
                     : ERROR_ACCESS_DENIED;
        else if (b.id == BROKEN_SHELL_USER)
            rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"Shell");
        bool ok = rc == ERROR_SUCCESS;
        log((ok ? L"Вернул: " : L"Не вышло: ") + b.what);
        fixed += ok;
    }
    if (fixed)
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Policy", SMTO_ABORTIFHUNG, 3000, nullptr);
    return fixed;
}

// В процессе с правами: HKLM-часть, по свежей проверке. Копия значений - в журнале помощника.
int FixBrokenAdminMain(const Log& log) {
    int fixed = 0, seen = 0;
    for (const Broken& b : FindBroken()) {
        if (!b.admin) continue;
        ++seen;
        LSTATUS rc = ERROR_FILE_NOT_FOUND;
        std::wstring key = b.where.substr(5);  // без «HKLM\»
        log(L"Было: " + b.where + L" = " + b.value);
        if (b.id == BROKEN_TASKMGR)
            rc = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DisableTaskMgr");
        else if (b.id == BROKEN_REGEDIT)
            rc = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DisableRegistryTools");
        else if (b.id == BROKEN_CMD)
            rc = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DisableCMD");
        else if (b.id == BROKEN_SHELL) {
            const wchar_t v[] = L"explorer.exe";
            rc = RegSetKeyValueW(HKEY_LOCAL_MACHINE, kWinlogon, L"Shell", REG_SZ, v, sizeof(v));
        } else if (b.id == BROKEN_USERINIT) {
            std::wstring v = WinDir() + L"\\system32\\userinit.exe,";
            rc = RegSetKeyValueW(HKEY_LOCAL_MACHINE, kWinlogon, L"Userinit", REG_SZ, v.c_str(),
                                 (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        } else if (b.id == BROKEN_IFEO) {
            rc = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Debugger");
        }
        bool ok = rc == ERROR_SUCCESS;
        log((ok ? L"Вернул: " : L"Не вышло: ") + b.what);
        fixed += ok;
    }
    if (!seen) log(L"Системная часть цела - чинить нечего.");
    return fixed;
}

// ---------------- перезагрузка ----------------

std::vector<std::wstring> PendingReboot() {
    std::vector<std::wstring> why;
    if (KeyExists(HKEY_LOCAL_MACHINE,
                  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending"))
        why.push_back(L"компоненты Windows ставятся при перезагрузке");
    if (KeyExists(HKEY_LOCAL_MACHINE,
                  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired"))
        why.push_back(L"обновления Windows ждут перезагрузки");
    return why;
}

bool RestartToRecovery() {
    // меню особой загрузки: оттуда - безопасный режим, восстановление системы, командная строка
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = std::wstring(sys) + L"\\shutdown.exe";
    return (INT_PTR)ShellExecuteW(nullptr, L"runas", exe.c_str(), L"/r /o /t 15", sys, SW_HIDE) > 32;
}

// ---------------- в процессе с правами ----------------

bool RepairWindowsAdminMain(const Log& log) {
    std::wstring out;
    log(L"Шаг 1 из 2: DISM сверяет хранилище компонентов с Windows Update (до 20-30 минут, нужен интернет)...");
    int a = RunCapture(L"dism.exe /Online /Cleanup-Image /RestoreHealth", out, 60 * 60 * 1000);
    log(L"DISM: " + (a == 0 ? std::wstring(L"готово") : L"код " + std::to_wstring(a)) + L". " + Tail(out, 2));
    log(L"Шаг 2 из 2: SFC проверяет системные файлы (10-15 минут)...");
    int b = RunCapture(L"sfc.exe /scannow", out, 60 * 60 * 1000);
    log(L"SFC: " + Tail(out, 3));
    log(L"Подробный журнал SFC: " + WinDir() + L"\\Logs\\CBS\\CBS.log");
    return a == 0 && b == 0;
}

bool ResetWindowsUpdateAdminMain(const Log& log) {
    const wchar_t* services[] = {L"wuauserv", L"cryptSvc", L"bits", L"msiserver"};
    std::wstring out;
    log(L"Останавливаю службы обновления...");
    for (const wchar_t* s : services) RunCapture(std::wstring(L"net.exe stop ") + s + L" /y", out, 60000);
    std::wstring stamp = Stamp(), win = WinDir();
    // не удаляем, а переименовываем: если что-то пойдёт не так, старое можно вернуть руками
    struct Dir {
        std::wstring path;
        const wchar_t* name;
    } dirs[] = {{win + L"\\SoftwareDistribution", L"кэш загрузок обновлений"},
                {win + L"\\System32\\catroot2", L"каталог подписей обновлений"}};
    bool ok = true;
    for (const Dir& d : dirs) {
        std::wstring to = d.path + L".old-" + stamp;
        if (GetFileAttributesW(d.path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (MoveFileExW(d.path.c_str(), to.c_str(), 0)) {
            log(std::wstring(L"Убран ") + d.name + L" (копия: " + to + L").");
        } else {
            ok = false;
            log(std::wstring(L"Не удалось убрать ") + d.name + L" - занят. Попробуйте после перезагрузки.");
        }
    }
    log(L"Запускаю службы обратно...");
    for (const wchar_t* s : services) RunCapture(std::wstring(L"net.exe start ") + s, out, 60000);
    log(ok ? L"Центр обновления сброшен: Windows заново скачает список обновлений. Старые папки .old можно удалить "
             L"через неделю, если всё работает."
           : L"Сброс неполный.");
    return ok;
}

bool SystemCleanAdminMain(const Log& log) {
    std::wstring win = WinDir(), root = win.substr(0, 3), out;
    auto freeNow = [&] {
        ULARGE_INTEGER f{};
        GetDiskFreeSpaceExW(root.c_str(), &f, nullptr, nullptr);
        return f.QuadPart;
    };
    // Свободное место на диске за 5-20 минут меняют и другие программы (браузер, обновления, подкачка) - итог по
    // нему шумный. Поэтому замер до и после того, что чистим: размер хранилища компонентов (так его считает сам
    // DISM) и папки кэша Delivery Optimization. Не удалось замерить - запасной вариант: свободное место.
    auto storeSize = [&] {
        std::wstring o;
        RunCapture(L"dism.exe /Online /Cleanup-Image /AnalyzeComponentStore", o, 15 * 60 * 1000);
        return ParseStoreSize(o);
    };
    // кэш DO: папка службы (Windows 10 1709+), раньше - в SoftwareDistribution
    auto doSize = [&] {
        ULONGLONG s = 0;
        for (const wchar_t* sub : {L"\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\"
                                   L"DeliveryOptimization\\Cache",
                                   L"\\SoftwareDistribution\\DeliveryOptimization"})
            s += DirBytes(win + sub);
        return s;
    };
    ULONGLONG before = freeNow();
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    log(L"Шаг 1 из 4: замеряю хранилище компонентов (DISM, 1-3 минуты)...");
    ULONGLONG storeBefore = storeSize(), doBefore = doSize();
    log(storeBefore ? L"Хранилище компонентов: " + Bytes(storeBefore) + L", кэш Delivery Optimization: " +
                          Bytes(doBefore) + L"."
                    : L"DISM не сказал размер хранилища - итог посчитаю по свободному месту.");
    log(L"Шаг 2 из 4: кэш Delivery Optimization (копии обновлений для раздачи другим компьютерам)...");
    // модули - только системные: PSModulePath из переменных пользователя выполнил бы подложенный модуль с правами
    int a = RunCapture(L"\"" + std::wstring(sys) +
                           L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -Command "
                           L"\"$env:PSModulePath=$PSHOME+'\\Modules'; Delete-DeliveryOptimizationCache -Force\"",
                       out, 10 * 60 * 1000);
    log(a == 0 ? L"Кэш Delivery Optimization очищен." : L"Delivery Optimization: " + Tail(out, 2));
    log(L"Шаг 3 из 4: DISM убирает вытесненные версии компонентов Windows (5-20 минут)...");
    int b = RunCapture(L"dism.exe /Online /Cleanup-Image /StartComponentCleanup", out, 60 * 60 * 1000);
    log(L"DISM: " + (b == 0 ? std::wstring(L"готово") : L"код " + std::to_wstring(b)) + L". " + Tail(out, 2));
    ULONGLONG after = freeNow();
    ULONGLONG byDisk = after > before ? after - before : 0, freed = byDisk;
    if (storeBefore) {
        log(L"Шаг 4 из 4: замеряю хранилище после чистки...");
        ULONGLONG storeAfter = storeSize(), doAfter = doSize();
        if (storeAfter) {
            ULONGLONG store = storeBefore > storeAfter ? storeBefore - storeAfter : 0;
            ULONGLONG cache = doBefore > doAfter ? doBefore - doAfter : 0;
            freed = store + cache;
            log(L"Хранилище компонентов: " + Bytes(storeBefore) + L" -> " + Bytes(storeAfter) +
                L", кэш Delivery Optimization: " + Bytes(doBefore) + L" -> " + Bytes(doAfter) + L".");
            log(L"Освобождено: " + Bytes(freed) + L" (по свободному месту на " + root.substr(0, 2) + L" - " +
                Bytes(byDisk) + L", там видны и чужие записи на диск).");
        } else {
            log(L"Второй замер DISM не удался - итог по свободному месту на " + root.substr(0, 2) + L": " +
                Bytes(byDisk) + L".");
        }
    } else {
        log(L"Освобождено на " + root.substr(0, 2) + L": " + Bytes(byDisk) + L".");
    }
    log(L"@freed=" + std::to_wstring(freed));
    return a == 0 || b == 0;
}

bool DefenderOfflineAdminMain(const Log& log) {
    log(L"Запускаю автономную проверку Microsoft Defender: компьютер перезагрузится и проверится до загрузки "
        L"Windows (около 15 минут).");
    std::wstring out;
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    int code = RunCapture(L"\"" + std::wstring(sys) +
                              L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -Command "
                              L"\"$env:PSModulePath=$PSHOME+'\\Modules'; Start-MpWDOScan\"",
                          out, 120000);
    if (code != 0) log(L"Не запустилась: " + Tail(out, 2));
    return code == 0;
}

}  // namespace tools
