// «Что изменилось за неделю»: снимок того, что стоит и запускается, раз в сутки и сравнение с неделей назад.
#include "tools/weekdiff.h"

#include <shlobj.h>

#include <algorithm>
#include <map>

#include "tools/tools.h"

namespace tools {

namespace {

constexpr int kKeepDays = 15;

std::wstring WeekDir() {
    std::wstring d = DataDir() + L"\\week";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring Clean(std::wstring s) {
    std::replace(s.begin(), s.end(), L'\t', L' ');
    std::replace(s.begin(), s.end(), L'\n', L' ');
    std::replace(s.begin(), s.end(), L'\r', L' ');
    return s;
}

// Программы из «Программ и компонентов»: имя, версия; обновления Windows и системные компоненты - не в счёт.
void ReadApps(std::vector<WeekEntry>& out) {
    struct Root {
        HKEY root;
        REGSAM view;
    } roots[] = {{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}, {HKEY_CURRENT_USER, 0}};
    std::map<std::wstring, std::wstring> seen;
    for (const Root& r : roots) {
        HKEY k;
        if (RegOpenKeyExW(r.root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | r.view,
                          &k) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; ++i) {
            wchar_t sub[256];
            DWORD n = 256;
            if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t name[512] = {}, ver[128] = {};
            DWORD sn = sizeof(name), sv = sizeof(ver), comp = 0, sc = sizeof(comp);
            if (RegGetValueW(k, sub, L"DisplayName", RRF_RT_REG_SZ, nullptr, name, &sn) != ERROR_SUCCESS || !name[0])
                continue;
            if (RegGetValueW(k, sub, L"SystemComponent", RRF_RT_REG_DWORD, nullptr, &comp, &sc) == ERROR_SUCCESS &&
                comp)
                continue;
            RegGetValueW(k, sub, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, ver, &sv);
            seen[Clean(name)] = Clean(ver);
        }
        RegCloseKey(k);
    }
    for (const auto& [name, ver] : seen) out.push_back({L"app", name, ver.empty() ? name : name + L" " + ver});
}

ULONGLONG Today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return t.wYear * 10000ull + t.wMonth * 100 + t.wDay;
}

// дней между датами ГГГГММДД
int DaysBetween(ULONGLONG a, ULONGLONG b) {
    auto days = [](ULONGLONG d) {
        SYSTEMTIME t{};
        t.wYear = (WORD)(d / 10000), t.wMonth = (WORD)(d / 100 % 100), t.wDay = (WORD)(d % 100);
        FILETIME ft;
        SystemTimeToFileTime(&t, &ft);
        return (LONGLONG)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / (864000000000ull));
    };
    return (int)(days(b) - days(a));
}

std::wstring ReadText(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string raw((size_t)std::min<LONGLONG>(size.QuadPart, 8 << 20), '\0');
    DWORD got = 0;
    ReadFile(f, raw.data(), (DWORD)raw.size(), &got, nullptr);
    CloseHandle(f);
    raw.resize(got);
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), w.data(), n);
    return w;
}

void WriteText(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string raw(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), raw.data(), n, nullptr, nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    bool ok = WriteFile(f, raw.data(), (DWORD)raw.size(), &put, nullptr) && put == raw.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

std::vector<WeekEntry> WeekSnapshot() {
    std::vector<WeekEntry> out;
    ReadApps(out);
    for (const StartItem& s : StartupItems())
        if (s.enabled) out.push_back({L"start", Clean(s.where + L"|" + s.name), Clean(s.name)});
    for (SysItem& s : SysItems()) {
        // часть Windows (подпись Microsoft) не в счёт; подписи - из общего кэша, первый раз дольше
        std::wstring signer;
        if (CheckSign(s.path, &signer) == SIGN_OK && signer.find(L"Microsoft") != std::wstring::npos) continue;
        out.push_back({s.kind, Clean(SysItemKey(s)), Clean(s.display.empty() ? s.name : s.display)});
    }
    return out;
}

WeekReport WeekRecordAndCompare() {
    WeekReport r;
    std::wstring dir = WeekDir();
    ULONGLONG today = Today();
    std::vector<ULONGLONG> dates;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.tsv").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            ULONGLONG d = _wcstoui64(fd.cFileName, nullptr, 10);
            if (d > 20000000 && d < 30000000) dates.push_back(d);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(dates.begin(), dates.end());
    auto file = [&](ULONGLONG d) { return dir + L"\\" + std::to_wstring(d) + L".tsv"; };
    std::vector<WeekEntry> now;
    if (!dates.empty() && dates.back() == today) {
        now = ParseWeekRows(ReadText(file(today)));
    } else {
        now = WeekSnapshot();
        WriteText(file(today), WeekRows(now));
        dates.push_back(today);
    }
    for (ULONGLONG d : dates)  // старше двух недель не нужны
        if (DaysBetween(d, today) > kKeepDays) DeleteFileW(file(d).c_str());
    ULONGLONG base = 0;
    for (ULONGLONG d : dates)
        if (d != today && DaysBetween(d, today) >= 7) base = d;  // самый свежий из недельной давности
    if (!base)
        for (ULONGLONG d : dates)
            if (d != today) {
                base = d;  // недели ещё нет - самый старый
                break;
            }
    if (!base) return r;
    r.days = DaysBetween(base, today);
    r.changes = WeekDiff(ParseWeekRows(ReadText(file(base))), now);
    return r;
}

}  // namespace tools
