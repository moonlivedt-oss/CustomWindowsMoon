#include "desk/whreg.h"

#include <shellapi.h>
#include <shlobj.h>

#include <memory>
#include <vector>

namespace whreg {

namespace {

constexpr wchar_t kMods[] = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Windhawk\\Engine\\Mods\\";

std::wstring Dir() {
    wchar_t ad[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", ad, MAX_PATH);
    SHCreateDirectoryExW(nullptr, ad, nullptr);
    return ad;
}

// В .reg строки в кавычках: \ и " экранируются.
std::wstring Q(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'"') o += L'\\';
        o += c;
    }
    return o + L"\"";
}

std::wstring Hex8(COLORREF c, BYTE a) {
    wchar_t b[16];
    swprintf(b, 16, L"#%02X%02X%02X%02X", a, GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

// Новая отметка времени настроек: Windhawk замечает её и мод перечитывает настройки без выключения.
std::wstring Touch(const wchar_t* mod) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ULL - 11644473600ULL;
    wchar_t b[32];
    swprintf(b, 32, L"%08x", (unsigned)t);
    return L"\r\n[" + std::wstring(kMods) + mod + L"]\r\n\"SettingsChangeTime\"=dword:" + b + L"\r\n";
}

std::wstring Write(const std::wstring& name, const std::wstring& body) {
    std::wstring path = Dir() + L"\\" + name;
    std::wstring text = L"Windows Registry Editor Version 5.00\r\n\r\n" + body;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    DWORD put = 0;
    WORD bom = 0xFEFF;  // regedit ждёт UTF-16
    WriteFile(f, &bom, 2, &put, nullptr);
    WriteFile(f, text.data(), (DWORD)(text.size() * 2), &put, nullptr);
    CloseHandle(f);
    return path;
}

}  // namespace

namespace {

COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

struct Style {
    const wchar_t* target;
    std::vector<std::wstring> styles;
};

// Настройки мода-стилизатора (Notification Center, Explorer, Settings, Start
// Menu Styler устроены одинаково): head - строки вроде "theme"="...", consts -
// styleConstants, list - controlStyles. Пустые хвосты прежних настроек
// затираются, чтобы старые стили не мешали.
std::wstring StylerBody(const wchar_t* mod, const std::wstring& head, const std::vector<std::wstring>& consts,
                        const std::vector<Style>& list) {
    std::wstring body = L"[" + std::wstring(kMods) + mod + L"\\Settings]\r\n" + head;
    for (size_t i = 0; i < consts.size(); ++i)
        body += L"\"styleConstants[" + std::to_wstring(i) + L"]\"=" + Q(consts[i]) + L"\r\n";
    for (size_t i = 0; i < list.size(); ++i) {
        std::wstring p = L"\"controlStyles[" + std::to_wstring(i) + L"].";
        body += p + L"target\"=" + Q(list[i].target) + L"\r\n";
        for (size_t j = 0; j < list[i].styles.size(); ++j)
            body += p + L"styles[" + std::to_wstring(j) + L"]\"=" + Q(list[i].styles[j]) + L"\r\n";
    }
    if (list.empty())
        body +=
            L"\"controlStyles[0].target\"=\"\"\r\n\"controlStyles[0].styles[0]"
            L"\"=\"\"\r\n";
    return body + Touch(mod) + L"\r\n";
}

constexpr wchar_t kNotify[] = L"windows-11-notification-center-styler";
constexpr wchar_t kExplorer[] = L"windows-11-file-explorer-styler";
constexpr wchar_t kSettings[] = L"windows-11-settings-styler";
constexpr wchar_t kStartMenu[] = L"windows-11-start-menu-styler";

std::wstring NotificationBody(COLORREF bg, COLORREF accent) {
    return StylerBody(kNotify, L"\"theme\"=\"TintedGlass\"\r\n",
                      {
                          L"Base=<WindhawkBlur BlurAmount=\"22\" TintColor=\"" + Hex8(bg, 0xC8) + L"\"/>",
                          L"Overlay=<WindhawkBlur BlurAmount=\"22\" TintColor=\"" + Hex8(accent, 0x2A) + L"\"/>",
                          L"Radius=18",
                      },
                      {});
}

// своя тема вместо встроенной: стекло вкладок и панели с каплей акцента,
// графитовое содержимое, тонкая акцентная кромка у адресной строки и
// всплывающих меню
std::wstring ExplorerBody(COLORREF bg, COLORREF accent) {
    std::vector<Style> list = {
        {L"FileExplorerExtensions.CommandBarControl_Wave1 > Grid, "
         L"Grid#CommandBarControlRootGrid",
         {L"Background:=$Glass", L"BorderThickness=0,0,0,0"}},
        {L"CommandBar#FileExplorerCommandBar", {L"Background=Transparent"}},
        {L"Grid#NavigationBarControlGrid", {L"Background:=$Glass"}},
        {L"TabViewItem > Grid#LayoutRoot > Canvas > "
         L"Microsoft.UI.Xaml.Shapes.Path#SelectedBackgroundPath",
         {L"Fill:=$Glass"}},
        {L"Grid#HomeViewRootGrid", {L"Background:=$Content"}},
        {L"FileExplorerExtensions.GalleryViewControl#GalleryViewControl > Grid", {L"Background:=$Content"}},
        {L"Microsoft.UI.Xaml.Controls.Grid#GalleryRootGrid", {L"Background:=$Content"}},
        {L"Grid#DetailsViewControlRootGrid", {L"Background:=$Content"}},
        {L"StackPanel#DetailsViewThumbnail > Grid", {L"Background:=$Content"}},
        {L"ToolTip", {L"Background:=$Glass", L"BorderBrush:=$Edge", L"BorderThickness=1", L"CornerRadius=6"}},
        {L"Microsoft.UI.Xaml.Controls.Grid#FileExplorerAddressBarGrid",
         {L"CornerRadius=8", L"BorderBrush:=$Edge", L"BorderThickness=1"}},
        {L"Microsoft.UI.Xaml.Controls.AutoSuggestBox#FileExplorerSearchBox > "
         L"Microsoft.UI.Xaml.Controls.Grid#LayoutRoot "
         L"> Microsoft.UI.Xaml.Controls.TextBox#TextBox",
         {L"CornerRadius=8"}},
        {L"CommandBarOverflowPresenter#SecondaryItemsControl > Grid#LayoutRoot",
         {L"Background:=$Glass", L"BorderBrush:=$Edge", L"BorderThickness=1", L"CornerRadius=8"}},
    };
    // эффект только на рамке: на всё окно акрил просвечивает сквозь список файлов
    // серой пеленой
    return StylerBody(
        kExplorer,
        L"\"theme\"=\"\"\r\n\"backgroundTranslucentEffect\"=\"mica\"\r\n"
        L"\"backgroundTranslucentEffectRegion\"=\"explorerFrame\"\r\n",
        {
            L"Glass=<WindhawkBlur BlurAmount=\"18\" TintColor=\"" + Hex8(Mix(bg, accent, 0.12), 0xB8) + L"\"/>",
            L"Content=<WindhawkBlur BlurAmount=\"18\" TintColor=\"" + Hex8(bg, 0xC8) + L"\"/>",
            L"Edge=<SolidColorBrush Color=\"" + Hex8(accent, 0x90) + L"\"/>",
        },
        list);
}

// «Параметры»: графитовое стекло окна и меню навигации, карточки чуть светлее с
// каплей акцента, всплывающие меню и подсказки - стекло с акцентной кромкой.
// Переключатели и так берут цвет акцента Windows.
std::wstring SettingsBody(COLORREF bg, COLORREF panel, COLORREF accent) {
    const wchar_t* flyout =
        L"Windows.UI.Xaml.Controls.MenuFlyoutPresenter > "
        L"Windows.UI.Xaml.Controls.Border";
    std::vector<Style> list = {
        {L"Frame#PermanentNavRootFrame", {L"Background:=$Base"}},
        {L"Microsoft.UI.Xaml.Controls.NavigationView#PermanentNavigationView", {L"Background:=$Base"}},
        {L"SystemSettings.View.EntityItem", {L"Background:=$Card"}},
        {flyout, {L"Background:=$Glass", L"BorderBrush:=$Edge", L"BorderThickness=1", L"CornerRadius=8"}},
        {L"Windows.UI.Xaml.Controls.ToolTip > "
         L"Windows.UI.Xaml.Controls.ContentPresenter#LayoutRoot",
         {L"Background:=$Glass", L"BorderBrush:=$Edge", L"BorderThickness=1", L"CornerRadius=6"}},
    };
    return StylerBody(
        kSettings, L"\"theme\"=\"\"\r\n",
        {
            L"Base=<WindhawkBlur BlurAmount=\"25\" TintColor=\"" + Hex8(bg, 0xE0) + L"\"/>",
            L"Card=<SolidColorBrush Color=\"" + Hex8(Mix(panel, accent, 0.07), 0xFF) + L"\"/>",
            L"Glass=<WindhawkBlur BlurAmount=\"20\" TintColor=\"" + Hex8(Mix(bg, accent, 0.12), 0xD0) + L"\"/>",
            L"Edge=<SolidColorBrush Color=\"" + Hex8(accent, 0x90) + L"\"/>",
        },
        list);
}

// Поиск Windows (мод Start Menu Styler, процесс SearchHost): тема TintedGlass,
// её стекло - графит с каплей акцента. Пуск у пользователя от Start11, поэтому
// то, что тема делает с обычным Пуском, не видно.
std::wstring SearchBody(COLORREF bg, COLORREF accent) {
    return StylerBody(
        kStartMenu, L"\"theme\"=\"TintedGlass\"\r\n",
        {L"CommonBgBrush=<WindhawkBlur BlurAmount=\"22\" TintColor=\"" + Hex8(Mix(bg, accent, 0.10), 0xC8) + L"\"/>"},
        {});
}

}  // namespace

bool ModInstalled(const wchar_t* mod) {
    HKEY k;
    std::wstring path = L"SOFTWARE\\Windhawk\\Engine\\Mods\\" + std::wstring(mod);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

std::wstring IconThemeReg(const std::wstring& folder) {
    std::wstring body = L"[" + std::wstring(kMods) + L"icon-resource-redirect\\Settings]\r\n";
    body += L"\"themeFolder\"=" + Q(folder) + L"\r\n";
    body += L"\"iconTheme\"=\"\"\r\n";  // иначе мод возьмёт тему из своего списка,
                                        // а не папку
    return Write(L"icons-theme.reg", body);
}

std::wstring IconThemeRestoreReg(const std::wstring& oldFolder) {
    std::wstring body = L"[" + std::wstring(kMods) + L"icon-resource-redirect\\Settings]\r\n";
    body += L"\"themeFolder\"=" + Q(oldFolder) + L"\r\n";
    return Write(L"icons-theme-restore.reg", body);
}

std::wstring NotificationReg(COLORREF bg, COLORREF accent) {
    return Write(L"notification-theme.reg", NotificationBody(bg, accent));
}

std::wstring ExplorerReg(COLORREF bg, COLORREF accent) {
    return Write(L"explorer-theme.reg", ExplorerBody(bg, accent));
}

std::wstring SettingsReg(COLORREF bg, COLORREF panel, COLORREF accent) {
    return Write(L"settings-theme.reg", SettingsBody(bg, panel, accent));
}

std::wstring SearchReg(COLORREF bg, COLORREF accent) { return Write(L"search-theme.reg", SearchBody(bg, accent)); }

std::wstring AllReg(COLORREF bg, COLORREF panel, COLORREF accent, std::wstring* mods) {
    std::wstring body;
    auto add = [&](const wchar_t* mod, const wchar_t* name, std::wstring part) {
        if (!ModInstalled(mod)) return;
        body += part;
        if (mods) *mods += (mods->empty() ? L"" : L", ") + std::wstring(name);
    };
    add(kExplorer, L"Проводник", ExplorerBody(bg, accent));
    add(kNotify, L"центр уведомлений", NotificationBody(bg, accent));
    add(kSettings, L"Параметры", SettingsBody(bg, panel, accent));
    add(kStartMenu, L"поиск", SearchBody(bg, accent));
    return body.empty() ? L"" : Write(L"theme-everywhere.reg", body);
}

std::wstring LogonReg(bool crisp) {
    return Write(L"logon-background.reg",
                 L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\System]\r\n"
                 L"\"DisableAcrylicBackgroundOnLogon\"=" +
                     std::wstring(crisp ? L"dword:00000001" : L"-") + L"\r\n");
}

bool LogonCrisp() {
    DWORD v = 0, sz = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System",
                        L"DisableAcrylicBackgroundOnLogon", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS &&
           v != 0;
}

// .reg лежит в %APPDATA%: пока на экране вопрос UAC, другая программа могла бы подменить его своим содержимым -
// и regedit с правами внёс бы чужое. Поэтому файл держим открытым без права записи до конца импорта, а перед
// запуском сверяем: только разделы, которые пишет сама программа (настройки модов Windhawk и фон экрана входа).
void Run(const std::wstring& regFile) {
    if (regFile.empty()) return;
    HANDLE lock = CreateFileW(regFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return;
    std::wstring text;
    {
        std::string raw(1 << 20, '\0');
        DWORD got = 0;
        ReadFile(lock, raw.data(), (DWORD)raw.size(), &got, nullptr);
        raw.resize(got);
        if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE)
            text.assign((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
    }
    bool ok = text.rfind(L"Windows Registry Editor Version 5.00", 0) == 0;
    for (size_t at = text.find(L"\n["); ok && at != std::wstring::npos; at = text.find(L"\n[", at + 1)) {
        std::wstring sec = text.substr(at + 2, text.find(L']', at) - at - 2);
        ok = sec.rfind(std::wstring(kMods), 0) == 0 ||
             sec == L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\System";
    }
    if (!ok) {
        CloseHandle(lock);
        return;  // файл не наш - не импортируем
    }
    // regedit сам попросит права администратора и подтверждение; путь - из папки Windows, не по поиску
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring exe = std::wstring(win) + L"\\regedit.exe", args = L"\"" + regFile + L"\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        CloseHandle(lock);
        return;
    }
    // замок снимаем, когда regedit закончит (или через 5 минут, если его окно так и висит)
    struct Box {
        HANDLE proc, lock;
    };
    auto* box = new Box{sei.hProcess, lock};
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<Box> b((Box*)p);
            WaitForSingleObject(b->proc, 5 * 60 * 1000);
            CloseHandle(b->proc);
            CloseHandle(b->lock);
            return 0;
        },
        box, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        CloseHandle(sei.hProcess);
        CloseHandle(lock);
        delete box;
    }
}

}  // namespace whreg
