// Сторож служб (И17), чистое: кто из служб - защита компьютера, и список выключенного задачей с правами (под тестами).
#include <windows.h>

#include <cwchar>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

bool SecurityVendor(const std::wstring& lower) {
    for (const wchar_t* v : {L"windows defender",
                             L"kaspersky",
                             L"\\eset\\",
                             L"eset,",
                             L"eset spol",
                             L"avast",
                             L"avg technologies",
                             L"bitdefender",
                             L"malwarebytes",
                             L"drweb",
                             L"doctor web",
                             L"norton",
                             L"symantec",
                             L"gen digital",
                             L"mcafee",
                             L"trellix",
                             L"crowdstrike",
                             L"sentinelone",
                             L"sophos",
                             L"trend micro",
                             L"f-secure",
                             L"withsecure",
                             L"panda security",
                             L"watchguard",
                             L"emsisoft",
                             L"cylance",
                             L"carbon black",
                             L"vmware carbon",
                             L"cisco secure",
                             L"palo alto",
                             L"cortex xdr",
                             L"elastic endpoint",
                             L"huorong",
                             L"360 total",
                             L"qihoo",
                             L"comodo",
                             L"xcitium",
                             L"zonealarm",
                             L"check point",
                             L"webroot",
                             L"opentext",
                             L"g data",
                             L"gdata",
                             L"k7 computing",
                             L"quick heal",
                             L"avira"})
        if (lower.find(v) != std::wstring::npos) return true;
    return false;
}

std::wstring SysOffAdd(const std::wstring& table, const std::wstring& kind, const std::wstring& name, DWORD start) {
    DWORD old = 0;
    std::wstring t = table;
    SysOffTake(t, kind, name, old);
    return t + kind + L"\t" + name + L"\t" + std::to_wstring(start) + L"\n";
}

bool SysOffTake(std::wstring& table, const std::wstring& kind, const std::wstring& name, DWORD& start) {
    std::wstring rest;
    bool found = false;
    for (const std::wstring& line : Split(table, L'\n')) {
        std::vector<std::wstring> p = Split(line, L'\t');
        if (p.size() != 3) continue;
        if (!found && p[0] == kind && _wcsicmp(p[1].c_str(), name.c_str()) == 0) {
            DWORD s = (DWORD)_wtoi(p[2].c_str());
            // только обычные типы запуска: «при загрузке ядра» и «при старте системы» - у драйверов, и те - как были
            found = s <= SERVICE_DEMAND_START && (kind == L"drv" || s >= SERVICE_AUTO_START);
            if (found) start = s;
            continue;
        }
        rest += line + L"\n";
    }
    table = rest;
    return found;
}

}  // namespace tools
