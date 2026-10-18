// Системные гиганты (И11): файл гибернации, подкачка, Windows.old и папки обновления до новой версии, точки
// восстановления, кэш загрузок Центра обновления, дампы памяти. Сколько занимают и честное действие для каждого:
// выключить гибернацию, ограничить место под точки, убрать прошлую Windows встроенной очисткой - через задачу с
// правами, с вопросом. Подкачку и дампы не трогаем сами - только объясняем и открываем, где это настраивается.
#include <windows.h>

#include <algorithm>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

std::wstring WinDir() {
    wchar_t w[MAX_PATH] = {};
    GetWindowsDirectoryW(w, MAX_PATH);
    return w;
}

std::wstring SysExe(const wchar_t* exe) {
    wchar_t s[MAX_PATH] = {};
    GetSystemDirectoryW(s, MAX_PATH);
    return L"\"" + std::wstring(s) + L"\\" + exe + L"\"";
}

ULONGLONG FileBytes(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) {
        // pagefile.sys и hiberfil.sys заняты системой: атрибуты - через список папки
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(p.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        FindClose(h);
        return ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    }
    return ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

bool Has(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

std::vector<Giant> SystemGiants(const std::map<std::wstring, ULONGLONG>& exact) {
    std::wstring win = WinDir(), root = win.substr(0, 3);
    std::vector<Giant> out;
    auto add = [&](const wchar_t* key, const wchar_t* name, const std::wstring& path, ULONGLONG bytes,
                   const wchar_t* why, const wchar_t* action, const wchar_t* undo = L"") {
        Giant g;
        g.key = key, g.name = name, g.path = path, g.bytes = bytes, g.why = why, g.action = action, g.undo = undo;
        auto e = exact.find(key);
        if (e != exact.end()) g.bytes = e->second, g.exact = true;
        if (g.bytes || g.exact || g.key == L"shadow") out.push_back(g);  // точки: размер виден только с правами
    };
    add(L"hiber", L"Файл гибернации", root + L"hiberfil.sys", FileBytes(root + L"hiberfil.sys"),
        L"Копия памяти для гибернации и быстрого запуска. Если гибернацией не пользуетесь - его можно убрать: "
        L"компьютер будет включаться на пару секунд дольше.",
        L"hiber-off", L"hiber-on");
    add(L"page", L"Файл подкачки", root + L"pagefile.sys", FileBytes(root + L"pagefile.sys"),
        L"Нужен Windows и играм, когда памяти не хватает. Удалять не стоит; уменьшить можно в «Быстродействии» - "
        L"открою окно, решать вам.",
        L"page-open");
    add(L"swap", L"Файл подкачки приложений", root + L"swapfile.sys", FileBytes(root + L"swapfile.sys"),
        L"Приложения из Магазина. Небольшой, управляется вместе с подкачкой.", L"page-open");
    for (const wchar_t* dir : {L"Windows.old", L"$WINDOWS.~BT", L"$WINDOWS.~WS"}) {
        std::wstring p = root + dir;
        if (!Has(p)) continue;
        std::wstring key = std::wstring(L"old:") + dir;
        Giant g;
        g.key = key;
        g.name = dir == std::wstring(L"Windows.old") ? L"Прошлая Windows (Windows.old)" : L"Файлы обновления Windows";
        g.path = p;
        g.bytes = DirBytes(p);  // без прав часть папок закрыта - примерно, точно - кнопкой с правами
        auto e = exact.find(key);
        if (e != exact.end()) g.bytes = e->second, g.exact = true;
        g.why =
            L"Остатки перехода на новую версию: нужны, только чтобы откатиться назад (первые 10 дней). Убирает "
            L"встроенная очистка Windows (cleanmgr /autoclean); после этого откат станет невозможен.";
        g.action = L"old-windows";
        out.push_back(g);
    }
    add(L"shadow", L"Точки восстановления", L"", 0,
        L"Теневые копии «Защиты системы». Место под них ограничивается: 5% диска хватает на несколько точек, самые "
        L"старые при этом удалятся.",
        L"shadow-shrink");
    add(L"wu", L"Загрузки Центра обновления", win + L"\\SoftwareDistribution\\Download",
        DirBytes(win + L"\\SoftwareDistribution\\Download"),
        L"Скачанные обновления. Уже установленные убирает «Остатки обновлений» (кэш доставки и старые компоненты).",
        L"sys-clean");
    ULONGLONG dumps = FileBytes(win + L"\\MEMORY.DMP") + DirBytes(win + L"\\LiveKernelReports");
    add(L"dumps", L"Дампы памяти", win + L"\\MEMORY.DMP", dumps,
        L"Снимки памяти после синих экранов и сбоев драйверов. Если сбои не разбираете - их убирает «Очистка диска» "
        L"(пункт «Файлы дампа памяти»), открою её.",
        L"cleanmgr");
    std::sort(out.begin(), out.end(), [](const Giant& a, const Giant& b) { return a.bytes > b.bytes; });
    return out;
}

std::wstring GiantsAdminMain(const std::wstring& job, const Log& log) {
    std::wstring win = WinDir(), root = win.substr(0, 3), out;
    if (job == L"read") {  // только чтение: точные размеры, что видно лишь с правами
        RunCapture(SysExe(L"vssadmin.exe") + L" list shadowstorage /for=" + root.substr(0, 2), out, 60000);
        log(L"@giant\tshadow\t" + std::to_wstring(ParseShadowUsed(out)));
        for (const wchar_t* dir : {L"Windows.old", L"$WINDOWS.~BT", L"$WINDOWS.~WS"})
            if (Has(root + dir))
                log(L"@giant\told:" + std::wstring(dir) + L"\t" + std::to_wstring(DirBytes(root + dir)));
        return L"ok";
    }
    if (job == L"hiber-off" || job == L"hiber-on") {
        int code = RunCapture(SysExe(L"powercfg.exe") + (job == L"hiber-off" ? L" /hibernate off" : L" /hibernate on"),
                              out, 60000);
        log(code == 0 ? (job == L"hiber-off" ? L"Гибернация выключена, hiberfil.sys удалён."
                                             : L"Гибернация и быстрый запуск снова включены.")
                      : L"powercfg не смог (код " + std::to_wstring(code) + L"): " + Trim(out));
        return code == 0 ? L"ok" : L"";
    }
    if (job == L"shadow-shrink") {
        int code = RunCapture(SysExe(L"vssadmin.exe") + L" resize shadowstorage /for=" + root.substr(0, 2) + L" /on=" +
                                  root.substr(0, 2) + L" /maxsize=5%",
                              out, 120000);
        log(code == 0 ? L"Под точки восстановления - не больше 5% диска; лишние старые удалены."
                      : L"vssadmin не смог (код " + std::to_wstring(code) + L"): " + Trim(out));
        return code == 0 ? L"ok" : L"";
    }
    if (job == L"old-windows") {
        log(L"Встроенная очистка убирает прошлую Windows и файлы обновления (несколько минут)...");
        ULARGE_INTEGER before{}, after{};
        GetDiskFreeSpaceExW(root.c_str(), &before, nullptr, nullptr);
        int code = RunCapture(SysExe(L"cleanmgr.exe") + L" /autoclean /d " + root.substr(0, 2), out, 45 * 60 * 1000);
        GetDiskFreeSpaceExW(root.c_str(), &after, nullptr, nullptr);
        bool gone = !Has(root + L"Windows.old") && !Has(root + L"$WINDOWS.~BT");
        log(gone ? L"Прошлая Windows убрана, освободилось около " +
                       Bytes(after.QuadPart > before.QuadPart ? after.QuadPart - before.QuadPart : 0) + L"."
                 : L"Очистка закончилась (код " + std::to_wstring(code) +
                       L"), но папки остались: Windows держит их, пока не прошли 10 дней или идёт обновление.");
        return gone ? L"ok" : L"";
    }
    return L"";
}

}  // namespace tools
