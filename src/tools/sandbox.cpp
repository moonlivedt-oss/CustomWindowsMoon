// Песочница для незнакомых программ: Windows Sandbox (Windows 10/11 Pro) - одноразовая чистая Windows внутри
// окна. Файл попадает туда только для чтения, сеть по выбору; закрыли окно - всё, что программа сделала, исчезло.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "core/files.h"
#include "core/text.h"
#include "tools/tools.h"

namespace tools {

namespace {

std::wstring System32(const wchar_t* name) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys) + L"\\" + name;
}

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return Lower(out);
}

std::wstring Xml(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'&')
            o += L"&amp;";
        else if (c == L'<')
            o += L"&lt;";
        else if (c == L'>')
            o += L"&gt;";
        else if (c == L'"')
            o += L"&quot;";
        else
            o += c;
    }
    return o;
}

bool HasDll(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

void ClearDir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Инструмент в папке защиты и её подпапках (Sysinternals лежат каждый в своей).
std::wstring FindTool(const std::wstring& dir, const wchar_t* name, int depth) {
    std::wstring direct = dir + L"\\" + name;
    if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES) return direct;
    if (depth >= 2) return L"";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
            found = FindTool(dir + L"\\" + fd.cFileName, name, depth + 1);
    while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

}  // namespace

bool SandboxAvailable() {
    return GetFileAttributesW(System32(L"WindowsSandbox.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
}

void OpenSandboxFeature() {
    std::wstring exe = System32(L"OptionalFeatures.exe");
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring SandboxConfig(const std::wstring& file, bool network, std::wstring& err, const std::wstring& checkTools) {
    DWORD a = GetFileAttributesW(file.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
        err = L"Нет такого файла: " + file;
        return L"";
    }
    wchar_t la[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\MoonDesk\\sandbox", la, MAX_PATH);
    std::wstring base = la, in = base + L"\\in";
    SHCreateDirectoryExW(nullptr, in.c_str(), nullptr);

    // Что подключить: папку программы, если ей нужны DLL рядом (переносная программа), иначе - копию одного файла.
    // Загрузки, Рабочий стол, Документы и корень диска целиком не отдаём: там всё остальное.
    size_t cut = file.find_last_of(L'\\');
    std::wstring dir = file.substr(0, cut), name = file.substr(cut + 1), ldir = Lower(dir);
    bool common = dir.size() <= 3 || ldir == Known(FOLDERID_Downloads) || ldir == Known(FOLDERID_Desktop) ||
                  ldir == Known(FOLDERID_Documents) || ldir == Known(FOLDERID_Profile);
    std::wstring host;
    if (!common && HasDll(dir)) {
        host = dir;
    } else {
        ClearDir(in);
        if (!CopyFileW(file.c_str(), (in + L"\\" + name).c_str(), FALSE)) {
            err = L"Не скопировался файл для песочницы";
            return L"";
        }
        host = in;
    }
    // Режим проверки: рядом вторая папка (только чтение) с инструментами Sysinternals и скриптом check.ps1 - он
    // снимает систему до и после запуска и пишет отчёт на рабочий стол песочницы.
    std::wstring kit;
    if (!checkTools.empty()) {
        kit = base + L"\\kit";
        SHCreateDirectoryExW(nullptr, kit.c_str(), nullptr);
        ClearDir(kit);
        bool ok = CopyFileW((ExeDir() + L"\\sandbox\\check.ps1").c_str(), (kit + L"\\check.ps1").c_str(), FALSE) != FALSE;
        for (const wchar_t* tool : {L"Procmon64.exe", L"procexp64.exe", L"tcpview64.exe", L"Autoruns64.exe"}) {
            std::wstring found = FindTool(checkTools, tool, 0);
            bool need = !_wcsicmp(tool, L"Procmon64.exe") || !_wcsicmp(tool, L"procexp64.exe");
            if (found.empty()) {
                if (need) ok = false;
                continue;
            }
            CopyFileW(found.c_str(), (kit + L"\\" + tool).c_str(), FALSE);
        }
        if (!ok) {
            err = L"Для проверки нужны check.ps1 рядом с RadialMenu и Process Monitor с Process Explorer в папке защиты";
            return L"";
        }
    }
    const std::wstring inside = L"C:\\Users\\WDAGUtilityAccount\\Desktop\\Check";  // латиницей: cmd в песочнице
    // запуск сам: exe и msi стартуют, остальное открывается программой по умолчанию; рядом - окно папки
    const std::wstring tools = L"C:\\Users\\WDAGUtilityAccount\\Desktop\\Tools";
    std::wstring cmd = !kit.empty()
                           ? L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File &quot;" + tools +
                                 L"\\check.ps1&quot; -Target &quot;" + inside + L"\\" + Xml(name) + L"&quot; -Tools &quot;" +
                                 tools + L"&quot;" + (network ? L" -Network" : L"")
                           : L"cmd.exe /c start \"\" explorer.exe \"" + inside + L"\" &amp; start \"\" \"" + inside + L"\\" +
                       Xml(name) + L"\"";
    std::wstring wsb =
        L"<Configuration>\r\n"
        L"  <Networking>" +
        std::wstring(network ? L"Default" : L"Disable") +
        L"</Networking>\r\n"
        // без видеоускорения: на ноутбуках с двумя видеокартами (NVIDIA + встроенная) с ним окно песочницы теряет
        // связь, а незнакомой программе доступ к драйверу GPU и не нужен
        L"  <vGPU>Disable</vGPU>\r\n"
        L"  <ClipboardRedirection>Disable</ClipboardRedirection>\r\n"
        L"  <PrinterRedirection>Disable</PrinterRedirection>\r\n"
        L"  <MappedFolders>\r\n"
        L"    <MappedFolder>\r\n"
        L"      <HostFolder>" +
        Xml(host) +
        L"</HostFolder>\r\n"
        L"      <SandboxFolder>" +
        Xml(inside) +
        L"</SandboxFolder>\r\n"
        L"      <ReadOnly>true</ReadOnly>\r\n"
        L"    </MappedFolder>\r\n" +
        (kit.empty() ? std::wstring()
                     : L"    <MappedFolder>\r\n      <HostFolder>" + Xml(kit) + L"</HostFolder>\r\n      <SandboxFolder>" +
                           tools + L"</SandboxFolder>\r\n      <ReadOnly>true</ReadOnly>\r\n    </MappedFolder>\r\n") +
        L"  </MappedFolders>\r\n"
        L"  <LogonCommand>\r\n"
        L"    <Command>" +
        cmd +
        L"</Command>\r\n"
        L"  </LogonCommand>\r\n"
        L"</Configuration>\r\n";
    std::wstring path = base + L"\\run.wsb";
    std::string bytes = "\xEF\xBB\xBF" + Narrow(wsb);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = L"Не записался файл настроек песочницы";
        return L"";
    }
    DWORD put = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    CloseHandle(f);
    return path;
}

bool RunInSandbox(const std::wstring& file, bool network, std::wstring& err, const std::wstring& checkTools) {
    if (!SandboxAvailable()) {
        err = L"Песочница Windows не включена";
        return false;
    }
    std::wstring path = SandboxConfig(file, network, err, checkTools);
    if (path.empty()) return false;
    // запускаем сам WindowsSandbox.exe из System32, а не по ассоциации .wsb: её может переписать любая программа
    std::wstring exe = System32(L"WindowsSandbox.exe"), args = L"\"" + path + L"\"";
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL) <= 32) {
        err = L"Песочница не запустилась";
        return false;
    }
    return true;
}

}  // namespace tools
