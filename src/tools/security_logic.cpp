// «Защита», чистое: по имени файла и строке запуска - похоже ли на маскировку (под тестами).
#include <windows.h>

#include <cwchar>

#include "core/text.h"
#include "tools/tools.h"

namespace tools {

// Расстояние правки не больше одного (замена, вставка, удаление, перестановка соседних): «scvhost», «svch0st».
bool OneEdit(const std::wstring& a, const std::wstring& b) {
    if (a == b) return false;
    size_t la = a.size(), lb = b.size();
    if (la == lb) {
        std::vector<size_t> diff;
        for (size_t i = 0; i < la && diff.size() < 3; ++i)
            if (a[i] != b[i]) diff.push_back(i);
        if (diff.size() == 1) return true;
        return diff.size() == 2 && diff[1] == diff[0] + 1 && a[diff[0]] == b[diff[1]] && a[diff[1]] == b[diff[0]];
    }
    if (la + 1 != lb && lb + 1 != la) return false;
    const std::wstring& s = la < lb ? a : b;
    const std::wstring& l = la < lb ? b : a;
    size_t i = 0;
    while (i < s.size() && s[i] == l[i]) ++i;
    return s.compare(i, std::wstring::npos, l, i + 1, std::wstring::npos) == 0;
}

std::wstring LookAlikeIn(const std::wstring& lowerLeaf, const std::vector<std::wstring>& systemNames) {
    if (!EndsWithI(lowerLeaf, L".exe")) return L"";
    std::wstring base = lowerLeaf.substr(0, lowerLeaf.size() - 4);
    if (base.size() < 5) return L"";
    for (const std::wstring& n : systemNames) {
        if (n.size() < 4) continue;
        std::wstring sys = n.substr(0, n.size() - 4);
        if (sys.size() >= 5 && OneEdit(base, sys)) return n;
    }
    return L"";
}

// «отчёт.pdf.exe», «отчёт.pdf      .exe»: документ, который на деле программа или скрипт
bool DoubleExt(const std::wstring& lowerLeaf) {
    static const wchar_t* kDoc[] = {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".jpg",  L".jpeg", L".png", L".txt",
                                    L".mp4", L".mp3", L".zip",  L".rar", L".avi",  L".mkv",  L".pptx", L".gif", L".rtf",
                                    L".7z",  L".csv", L".odt",  L".wav", L".mov",  L".webp", L".html", L".htm"};
    static const wchar_t* kExe[] = {L".exe", L".scr", L".com", L".bat", L".cmd", L".pif", L".js",
                                    L".jse", L".vbs", L".vbe", L".wsf", L".hta", L".ps1", L".msi"};
    for (const wchar_t* e : kExe) {
        if (!EndsWithI(lowerLeaf, e)) continue;
        std::wstring rest = lowerLeaf.substr(0, lowerLeaf.size() - wcslen(e));
        // пробелы перед настоящим расширением прячут его за краем столбца Проводника
        while (!rest.empty() && (rest.back() == L' ' || rest.back() == L'.' || rest.back() == 0x3000)) rest.pop_back();
        for (const wchar_t* d : kDoc)
            if (EndsWithI(rest, d)) return true;
    }
    return false;
}

// Невидимые знаки направления текста: «отчёт<RLO>fdp.exe» (U+202E перед «fdp») Проводник показывает как
// «отчётexe.pdf». В именах настоящих программ их не бывает.
bool BidiSpoof(const std::wstring& leaf) {
    for (wchar_t c : leaf)
        if ((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200E || c == 0x200F || c == 0x061C)
            return true;
    return false;
}

std::wstring ScriptLaunch(const std::wstring& cmd) {
    std::wstring l = Lower(cmd);
    auto has = [&](const wchar_t* s) { return l.find(s) != std::wstring::npos; };
    // ключ PowerShell с «-» или «/» и любым сокращением: -e, -ec, -enc, -encodedcommand
    auto psFlag = [&](const wchar_t* full, size_t minLen) {
        for (size_t at = 0; (at = l.find_first_of(L"-/", at)) != std::wstring::npos; ++at) {
            if (at && l[at - 1] != L' ' && l[at - 1] != L'"') continue;
            size_t e = at + 1;
            while (e < l.size() && iswalpha(l[e])) ++e;
            std::wstring w = l.substr(at + 1, e - at - 1);
            if (w.size() >= minLen && std::wstring(full).compare(0, w.size(), w) == 0) return true;
        }
        return false;
    };
    if (has(L"mshta")) return L"запускает mshta (HTML-приложение)";
    if (has(L"wscript") || has(L"cscript")) return L"запускает скрипт Windows Script Host";
    if (has(L"powershell") || has(L"pwsh")) {
        if (psFlag(L"encodedcommand", 1) || psFlag(L"ec", 2) || has(L"frombase64")) return L"закодированный PowerShell";
        if (has(L"downloadstring") || has(L"downloadfile") || has(L"net.webclient") || has(L"invoke-webrequest") ||
            has(L"iwr ") || has(L"invoke-restmethod") || has(L"irm ") || has(L"start-bitstransfer"))
            return L"PowerShell качает из интернета";
        if (has(L"iex ") || has(L"iex(") || has(L"invoke-expression") || has(L"hidden"))
            return L"скрытый PowerShell или выполнение строки";
    }
    if (has(L"regsvr32") && (has(L"/i:http") || has(L"scrobj"))) return L"regsvr32 грузит скрипт из сети";
    if (has(L"rundll32") && (has(L"\\appdata\\") || has(L"\\temp\\") || has(L"javascript:")))
        return L"rundll32 с библиотекой из AppData или TEMP";
    if (has(L"certutil") && (has(L"-urlcache") || has(L"/urlcache") || has(L"-decode") || has(L"/decode")))
        return L"certutil качает или раскодирует файл";
    if (has(L"bitsadmin") && has(L"/transfer")) return L"bitsadmin качает файл";
    if ((has(L"cmd.exe") || has(L"cmd ")) && (has(L"/c ") || has(L"/k ")) && (has(L"\\appdata\\") || has(L"\\temp\\")))
        return L"командная строка из AppData или TEMP";
    return L"";
}

}  // namespace tools
