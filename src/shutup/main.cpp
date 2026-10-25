// RadialShutUp.exe: приватность Windows одним окном, в духе O&O ShutUp10 - переключатели по группам с уровнями
// «рекомендуется / с оговорками / не рекомендуется», откат каждого из бэкапа, точка восстановления, профили.
// Рисуется тем же набором элементов, что и студия RadialMenu (ui/ui), в цветах её темы из radial.cfg рядом.
//
// Без окна (для RadialMenu и планировщика; итог строками «@ключ=значение» в stdout, код 0 - всё получилось):
//   --status           сколько включено, изменено нами и сброшено (и id сброшенных)
//   --reapply          снова включить то, что включали здесь, а обновление Windows или другая программа вернули
//   --profile файл     включить настройки из профиля
//   --revert           вернуть как было всё, что меняли здесь
//   --snap файл.png [ширина высота [фильтр]]  кадр окна в PNG (проверка вёрстки без мыши)
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/text.h"
#include "core/themes.h"
#include "shutup/catalog.h"
#include "ui/ui.h"

using namespace Gdiplus;
using ui::Kind;

namespace {

constexpr wchar_t kClass[] = L"MoonRadialShutUp";
constexpr UINT_PTR kTickTimer = 1, kMsgTimer = 2;
constexpr UINT WM_RUN = WM_APP + 1;  // отложенные действия: после кадра, не внутри WM_PAINT

// id элементов: у каждого свой диапазон
enum {
    ID_ELEVATE = 1,
    ID_SEARCH,
    ID_FILTER,
    ID_RECOMMENDED,
    ID_LIMITED,
    ID_REVERT,
    ID_RESTORE,
    ID_EXPORT,
    ID_IMPORT,
    ID_LIST,
    ID_CHROME,  // шапка и строка состояния: щелчок по ним не проходит к строкам списка под ними
    ID_EMPTY,
    ID_DRIFT,
    ID_UNDO,
    ID_EXPLORER,
    ID_GROUP = 100,   // + номер группы: свернуть
    ID_TOGGLE = 1000  // + номер настройки
};

std::wstring ExeFolder() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L'\\'));
}

Color LevelColor(shutup::Level l) {
    switch (l) {
        case shutup::Level::Recommended:
            return Color(255, 92, 196, 120);
        case shutup::Level::Limited:
            return Color(255, 232, 178, 72);
        default:
            return Color(255, 226, 84, 84);
    }
}

const wchar_t* LevelName(shutup::Level l) {
    switch (l) {
        case shutup::Level::Recommended:
            return L"рекомендуется";
        case shutup::Level::Limited:
            return L"с оговорками";
        default:
            return L"не рекомендуется";
    }
}

// SID владельца процесса; пусто - не узнать
std::vector<BYTE> ProcessUser(HANDLE proc) {
    std::vector<BYTE> out;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &tok)) return out;
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> buf(n);
    if (n && GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
        PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
        out.assign((BYTE*)sid, (BYTE*)sid + GetLengthSid(sid));
    }
    CloseHandle(tok);
    return out;
}

// Права дала другая учётная запись (обычный пользователь ввёл пароль администратора): HKCU тогда - профиль
// администратора, и «свои» настройки ушли бы не тому человеку.
bool ForeignAdmin() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetShellWindow(), &pid);
    if (!pid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    std::vector<BYTE> shell = ProcessUser(p);
    CloseHandle(p);
    std::vector<BYTE> self = ProcessUser(GetCurrentProcess());
    return !shell.empty() && !self.empty() && shell != self;
}

// почему настройку сейчас не тронуть; пусто - можно
std::wstring BlockReason(const shutup::Setting& s, bool admin, bool foreign) {
    if (s.Machine() && !admin)
        return L"«" + std::wstring(s.title) +
               L"» - для всего компьютера: нужны права. Нажмите «Перезапустить с правами».";
    if (s.User() && foreign)
        return L"«" + std::wstring(s.title) +
               L"» - ваша личная настройка, а права получены от другой учётной записи: запустите без прав.";
    return L"";
}

// System Restore отвечает процессу обратным вызовом COM от имени служб: по примеру Microsoft к SRSetRestorePoint
// им нужно разрешить такие вызовы. Только в процессе с правами - точку восстановления создаёт лишь он.
void InitComSecurity() {
    PSECURITY_DESCRIPTOR rel = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:(A;;0x7;;;SY)(A;;0x7;;;BA)(A;;0x7;;;NS)(A;;0x7;;;LS)", SDDL_REVISION_1, &rel, nullptr))
        return;
    // CoInitializeSecurity берёт описатель только в абсолютном виде, и он должен жить до конца процесса
    static std::vector<BYTE> sd, dacl, sacl, owner, group;
    DWORD sdN = 0, daclN = 0, saclN = 0, ownerN = 0, groupN = 0;
    MakeAbsoluteSD(rel, nullptr, &sdN, nullptr, &daclN, nullptr, &saclN, nullptr, &ownerN, nullptr, &groupN);
    sd.resize(std::max<DWORD>(sdN, 1));
    dacl.resize(std::max<DWORD>(daclN, 1));
    sacl.resize(std::max<DWORD>(saclN, 1));
    owner.resize(std::max<DWORD>(ownerN, 1));
    group.resize(std::max<DWORD>(groupN, 1));
    if (MakeAbsoluteSD(rel, sd.data(), &sdN, daclN ? (PACL)dacl.data() : nullptr, &daclN,
                       saclN ? (PACL)sacl.data() : nullptr, &saclN, ownerN ? owner.data() : nullptr, &ownerN,
                       groupN ? group.data() : nullptr, &groupN))
        CoInitializeSecurity(sd.data(), -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IDENTIFY,
                             nullptr, EOAC_NONE, nullptr);
    LocalFree(rel);
}

// Перезапустить Проводник: значения Explorer\Advanced (кнопки панели задач, Пуск) он читает при старте. Windows
// поднимает оболочку сама; из процесса с правами запускать её нельзя - она тоже стала бы с правами.
bool RestartExplorer(bool admin) {
    DWORD pid = 0;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &pid);
    if (!pid) return false;
    HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!p) return false;
    bool killed = TerminateProcess(p, 1);
    WaitForSingleObject(p, 3000);
    CloseHandle(p);
    if (!killed) return false;
    for (int i = 0; i < 25; ++i) {
        Sleep(200);
        if (FindWindowW(L"Shell_TrayWnd", nullptr)) return true;
    }
    if (admin) return false;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring exe = std::wstring(win) + L"\\explorer.exe";
    return (INT_PTR)ShellExecuteW(nullptr, nullptr, exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

std::vector<shutup::Status> AllStatus() {
    shutup::Backup user = shutup::LoadBackup(HKEY_CURRENT_USER), machine = shutup::LoadBackup(HKEY_LOCAL_MACHINE);
    std::vector<shutup::Status> st;
    for (const shutup::Setting& s : shutup::Catalog()) st.push_back(shutup::StatusOf(s, user, machine));
    return st;
}

std::string ReadSmall(const std::wstring& file, bool& ok) {
    std::string bytes;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    ok = f != INVALID_HANDLE_VALUE;
    if (!ok) return bytes;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got && bytes.size() < (1 << 20)) bytes.append(buf, got);
    CloseHandle(f);
    return bytes;
}

class App {
public:
    bool Create(HINSTANCE inst, int show);
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);
    void Bind(HWND h) {
        if (!wnd_) wnd_ = h;
    }
    bool Snap(const std::wstring& file, int w, int h, int filter);

private:
    struct Result {
        int done = 0, skipped = 0, failed = 0;
        std::wstring line, why;  // последняя строка Apply и последняя причина пропуска
    };
    void Reload();
    void Paint();
    void Frame(float w, float h);
    void Say(const std::wstring& s);
    std::wstring Blocked(size_t i) const { return BlockReason(shutup::Catalog()[i], admin_, foreign_); }
    // общая часть всех действий: правка, запись для «Отменить», что нужно перезапустить, рассылка, перечитать
    Result Run(const std::vector<size_t>& items, bool priv, const std::wstring& what);
    std::wstring Summary(const Result& r, const wchar_t* verb) const;
    void Toggle(size_t i, bool on);
    void Bulk(shutup::Level upTo);
    void Reapply();
    void RevertAll();
    void Undo();
    void Restore();
    void Export();
    void Import();
    void Elevate();
    void Explorer();
    bool AskRestorePoint();
    bool Visible(size_t i) const;
    // действие с окнами (диалоги, MessageBox) - после кадра: внутри WM_PAINT модальный цикл рисовал бы поверх
    void Defer(std::function<void()> f) {
        deferred_.push_back(std::move(f));
        PostMessageW(wnd_, WM_RUN, 0, 0);
    }

    HWND wnd_ = nullptr;
    ui::Ui ui_;
    Config cfg_;
    double k_ = 1;
    bool admin_ = false, foreign_ = false, askedRestore_ = false;
    bool needExplorer_ = false, needReboot_ = false;
    std::vector<shutup::Status> st_;
    std::vector<std::wstring> groups_;
    std::vector<bool> folded_;
    std::wstring query_, msg_;
    int filter_ = 0;                             // все, рекомендуемые, с оговорками, не рекомендуемые, изменённые здесь
    std::vector<std::pair<size_t, bool>> last_;  // последнее действие: что и в какую сторону правили
    std::wstring lastWhat_;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int bw_ = 0, bh_ = 0;
    std::vector<std::function<void()>> deferred_;
};

App* g_app = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) {
        g_app->Bind(h);
        return g_app->Proc(msg, wp, lp);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool App::Create(HINSTANCE inst, int show) {
    LoadConfig(ExeFolder() + L"\\radial.cfg", cfg_);  // нет файла - тема по умолчанию
    admin_ = shutup::IsAdmin();
    foreign_ = admin_ && ForeignAdmin();
    groups_ = shutup::Groups();
    folded_.assign(groups_.size(), false);
    Reload();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kClass;
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    k_ = dpi / 96.0;
    int w = (int)(1100 * k_), h = (int)(780 * k_);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    w = std::min(w, (int)(wa.right - wa.left));
    h = std::min(h, (int)(wa.bottom - wa.top));
    std::wstring title = std::wstring(L"RadialShutUp") + (admin_ ? L" (администратор)" : L"");
    wnd_ = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr,
                           nullptr, inst, nullptr);
    if (!wnd_) return false;
    k_ = GetDpiForWindow(wnd_) / 96.0;
    ui_.Attach(wnd_);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(wnd_, 20, &dark, sizeof(dark));  // тёмный заголовок
    UiColors c = UiPalette(cfg_);
    COLORREF cap = RGB(c.rail.GetR(), c.rail.GetG(), c.rail.GetB());
    DwmSetWindowAttribute(wnd_, 35, &cap, sizeof(cap));
    ShowWindow(wnd_, show);
    return true;
}

void App::Reload() { st_ = AllStatus(); }

void App::Say(const std::wstring& s) {
    msg_ = s;
    SetTimer(wnd_, kMsgTimer, 9000, nullptr);
    InvalidateRect(wnd_, nullptr, FALSE);
}

bool App::Visible(size_t i) const {
    const shutup::Setting& s = shutup::Catalog()[i];
    if (filter_ == 1 && s.level != shutup::Level::Recommended) return false;
    if (filter_ == 2 && s.level != shutup::Level::Limited) return false;
    if (filter_ == 3 && s.level != shutup::Level::NotRecommended) return false;
    if (filter_ == 4 && !st_[i].ours) return false;
    if (query_.empty()) return true;
    std::wstring q = Lower(query_);
    return Lower(s.title).find(q) != std::wstring::npos || Lower(s.why).find(q) != std::wstring::npos ||
           Lower(s.group).find(q) != std::wstring::npos || Lower(s.id).find(q) != std::wstring::npos;
}

// ShutUp10 перед первой правкой предлагает точку восстановления - и мы так же, раз за запуск
bool App::AskRestorePoint() {
    if (askedRestore_ || !admin_) return true;
    askedRestore_ = true;
    int r = MessageBoxW(wnd_,
                        L"Создать точку восстановления системы перед изменениями?\n\n"
                        L"Каждую настройку и так можно вернуть кнопкой, но точка восстановления спасёт, даже если "
                        L"Windows перестанет загружаться.",
                        L"RadialShutUp", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) {
        askedRestore_ = false;  // отменили само действие - спросить снова в следующий раз
        return false;
    }
    if (r == IDYES) Restore();
    return true;
}

App::Result App::Run(const std::vector<size_t>& items, bool priv, const std::wstring& what) {
    Result r;
    std::vector<std::pair<size_t, bool>> did;
    const auto& all = shutup::Catalog();
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (size_t i : items) {
        std::wstring why = Blocked(i);
        if (!why.empty()) {
            ++r.skipped;
            r.why = why;
            continue;
        }
        if (!shutup::Apply(all[i], priv, [&](const std::wstring& m) { r.line = m; })) {
            ++r.failed;
            continue;
        }
        ++r.done;
        did.push_back({i, priv});
        shutup::Restart need = shutup::RestartOf(all[i]);
        needExplorer_ = needExplorer_ || need == shutup::Restart::Explorer;
        needReboot_ = needReboot_ || need == shutup::Restart::Reboot;
    }
    SetCursor(old);
    if (!did.empty()) {
        last_ = std::move(did);
        lastWhat_ = what;
        shutup::NotifyChanged();
    }
    Reload();
    return r;
}

std::wstring App::Summary(const Result& r, const wchar_t* verb) const {
    std::wstring s = std::wstring(verb) + L": " + std::to_wstring(r.done);
    if (r.skipped)
        s += foreign_ ? L". Пропущено: " + std::to_wstring(r.skipped) + L" (права от другой учётной записи)"
                      : L". Без прав пропущено: " + std::to_wstring(r.skipped) + L" (для всего компьютера)";
    if (r.failed) s += L". Не получилось: " + std::to_wstring(r.failed);
    return s + L".";
}

void App::Toggle(size_t i, bool on) {
    const shutup::Setting& s = shutup::Catalog()[i];
    std::wstring why = Blocked(i);
    if (!why.empty()) {
        Say(why);
        return;
    }
    if (!on && !st_[i].ours) {
        Say(L"Это включено не RadialShutUp - возвращать нечего: так настроена сама Windows или другая программа.");
        return;
    }
    if (on && !AskRestorePoint()) return;
    Result r = Run({i}, on, s.title);
    std::wstring tail;
    if (r.done && on) {
        shutup::Restart need = shutup::RestartOf(s);
        if (need == shutup::Restart::Explorer) tail = L" Заработает после перезапуска Проводника.";
        if (need == shutup::Restart::Reboot) tail = L" Заработает после перезагрузки.";
    }
    Say(r.line + tail);
}

void App::Bulk(shutup::Level upTo) {
    std::vector<size_t> items;
    const auto& all = shutup::Catalog();
    for (size_t i = 0; i < all.size(); ++i)
        if ((int)all[i].level <= (int)upTo && st_[i].state != shutup::State::On) items.push_back(i);
    if (items.empty()) {
        Say(L"Всё это уже включено.");
        return;
    }
    if (!AskRestorePoint()) return;
    Say(Summary(Run(items, true, upTo == shutup::Level::Recommended ? L"рекомендуемые" : L"с оговорками"),
                L"Включено"));
}

// включали здесь, а потом выключило обновление Windows, Параметры или чистильщик - включить снова
void App::Reapply() {
    std::vector<size_t> items;
    for (size_t i = 0; i < st_.size(); ++i)
        if (st_[i].Drifted()) items.push_back(i);
    if (items.empty()) return;
    Say(Summary(Run(items, true, L"сброшенное"), L"Снова включено"));
}

void App::RevertAll() {
    std::vector<size_t> items;
    for (size_t i = 0; i < st_.size(); ++i)
        if (st_[i].ours) items.push_back(i);
    if (items.empty()) {
        Say(L"Возвращать нечего: RadialShutUp ничего не менял.");
        return;
    }
    Say(Summary(Run(items, false, L"«вернуть всё»"), L"Как было"));
}

// отменить последнее действие: включённое - вернуть как было, возвращённое - включить снова
void App::Undo() {
    if (last_.empty()) return;
    std::vector<size_t> on, off;
    for (const auto& [i, priv] : last_) (priv ? off : on).push_back(i);
    std::wstring what = lastWhat_;
    Result a = Run(off, false, L""), b = Run(on, true, L"");
    // повторная «Отменить» вернёт отменённое: в last_ - то, что сделала сама отмена
    last_.clear();
    for (size_t i : off) last_.push_back({i, false});
    for (size_t i : on) last_.push_back({i, true});
    lastWhat_ = L"отмену";
    int done = a.done + b.done, failed = a.failed + a.skipped + b.failed + b.skipped;
    Say(L"Отменено (" + what + L"): " + std::to_wstring(done) +
        (failed ? L", не получилось: " + std::to_wstring(failed) : L"") + L".");
}

void App::Restore() {
    if (!admin_) {
        Say(L"Точку восстановления создаёт только администратор - перезапустите с правами.");
        return;
    }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring why;
    bool ok = shutup::CreateRestorePoint(L"RadialShutUp: до изменения приватности", why);
    SetCursor(old);
    Say(ok ? L"Точка восстановления создана (Windows делает не больше одной в сутки - свежая могла не добавиться)."
           : L"Точку восстановления создать не удалось: " + why + L".");
}

void App::Export() {
    wchar_t file[MAX_PATH] = L"RadialShutUp.txt";
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = wnd_;
    of.lpstrFilter = L"Профиль RadialShutUp (*.txt)\0*.txt\0Все файлы\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"txt";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&of)) return;
    std::vector<std::wstring> ids;
    const auto& all = shutup::Catalog();
    for (size_t i = 0; i < all.size(); ++i)
        if (st_[i].state == shutup::State::On) ids.push_back(all[i].id);
    std::string bytes = Narrow(shutup::EncodeProfile(ids));
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD put = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    Say(ok ? L"Профиль сохранён: " + std::to_wstring(ids.size()) + L" включённых." : L"Не удалось записать файл.");
}

void App::Import() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = wnd_;
    of.lpstrFilter = L"Профиль RadialShutUp (*.txt)\0*.txt\0Все файлы\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    bool read = false;
    std::string bytes = ReadSmall(file, read);
    if (!read) {
        Say(L"Не удалось открыть файл.");
        return;
    }
    std::vector<size_t> items;
    const auto& all = shutup::Catalog();
    for (const std::wstring& id : shutup::DecodeProfile(Widen(bytes)))
        for (size_t i = 0; i < all.size(); ++i)
            if (id == all[i].id && st_[i].state != shutup::State::On) items.push_back(i);
    if (items.empty()) {
        Say(L"Из профиля включать нечего: в файле нет настроек RadialShutUp или всё уже включено.");
        return;
    }
    if (!AskRestorePoint()) return;
    Say(Summary(Run(items, true, L"профиль"), L"Из профиля включено"));
}

void App::Elevate() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW si{sizeof(si)};
    si.lpVerb = L"runas";
    si.lpFile = exe;
    si.nShow = SW_SHOWNORMAL;
    si.hwnd = wnd_;
    if (ShellExecuteExW(&si))
        PostMessageW(wnd_, WM_CLOSE, 0, 0);
    else
        Say(L"Права не получены.");
}

void App::Explorer() {
    if (MessageBoxW(wnd_,
                    L"Перезапустить Проводник?\n\nПанель задач на пару секунд пропадёт, открытые папки закроются. "
                    L"Программы и документы не трогаются.",
                    L"RadialShutUp", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = RestartExplorer(admin_);
    SetCursor(old);
    if (ok) needExplorer_ = false;
    Say(ok ? L"Проводник перезапущен - настройки панели задач и Пуска применены."
           : L"Проводник не перезапустился сам. Запустите его: Ctrl+Shift+Esc - «Запустить задачу» - explorer.");
}

void App::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd_, &ps);
    RECT rc;
    GetClientRect(wnd_, &rc);
    int w = std::max(1L, rc.right), h = std::max(1L, rc.bottom);
    if (!memDC_ || w != bw_ || h != bh_) {
        if (memDC_) {
            SelectObject(memDC_, oldBmp_);
            DeleteObject(dib_);
            DeleteDC(memDC_);
        }
        memDC_ = CreateCompatibleDC(dc);
        dib_ = CreateCompatibleBitmap(dc, w, h);
        oldBmp_ = SelectObject(memDC_, dib_);
        bw_ = w;
        bh_ = h;
    }
    {
        Graphics g(memDC_);
        ui_.Begin(g, UiPalette(cfg_), k_);
        Frame((float)w, (float)h);
        ui_.End();
    }
    BitBlt(dc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    EndPaint(wnd_, &ps);
    if (ui_.TakeRepaint())
        InvalidateRect(wnd_, nullptr, FALSE);
    else if (ui_.TakeTick())
        SetTimer(wnd_, kTickTimer, 33, nullptr);
}

// кадр в PNG без показа окна: анимации успевают дойти до конца за несколько кадров
bool App::Snap(const std::wstring& file, int w, int h, int filter) {
    filter_ = filter;
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    for (int n = 0; n < 30; ++n) {
        Graphics g(&bmp);
        ui_.Begin(g, UiPalette(cfg_), k_);
        Frame((float)w, (float)h);
        ui_.End();
        ui_.TakeRepaint();
        ui_.TakeTick();
        Sleep(20);
    }
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0)
            return bmp.Save(file.c_str(), &codecs[i].Clsid, nullptr) == Ok;
    return false;
}

void App::Frame(float w, float h) {
    ui::Ui& u = ui_;
    const UiColors& c = u.C();
    u.Fill(RectF(0, 0, w, h), c.win);
    const auto& all = shutup::Catalog();
    const float pad = u.S(24), top = u.S(178), bottom = u.S(44);
    int drift = 0;
    for (const shutup::Status& s : st_) drift += s.Drifted();

    // ---------------- список по группам ----------------
    RectF view(0, top, w, std::max(0.0f, h - top - bottom));
    const float rowH = u.S(66), headH = u.S(48), gap = u.S(14), cardW = std::min(w - 2 * pad, u.S(1100));
    const float cardX = (w - cardW) / 2, bannerH = drift ? u.S(64) + gap : 0;
    float content = u.S(6) + bannerH;
    std::vector<std::vector<size_t>> rows(groups_.size());
    for (size_t i = 0; i < all.size(); ++i)
        if (Visible(i)) {
            size_t gi = std::find(groups_.begin(), groups_.end(), all[i].group) - groups_.begin();
            rows[gi].push_back(i);
        }
    for (size_t gi = 0; gi < groups_.size(); ++gi)
        if (!rows[gi].empty()) content += headH + (folded_[gi] ? 0 : rows[gi].size() * rowH) + gap;
    float off = u.Scroll(ID_LIST, view, content + u.S(10));
    GraphicsState clip = u.G().Save();
    u.G().SetClip(view);
    float y = top + u.S(6) - off;
    // ---------------- сброшенное: включали здесь, а Windows вернула ----------------
    if (drift) {
        RectF ban(cardX, y, cardW, bannerH - gap);
        u.Fill(ban, c.warnBg, u.S(14));
        u.Stroke(ban, WithAlpha(c.warn, 90), u.S(14));
        u.Text(L"Сбросилось " + Plural(drift, L"настройка", L"настройки", L"настроек"),
               RectF(cardX + u.S(20), y + u.S(11), cardW - u.S(240), u.S(22)), c.warn, 13.0f, true);
        u.Text(L"Включали здесь, а потом вернуло обновление Windows или другая программа.",
               RectF(cardX + u.S(20), y + u.S(33), cardW - u.S(240), u.S(22)), c.muted, 11.0f);
        RectF b(cardX + cardW - u.S(200), y + u.S(15), u.S(180), u.S(34));
        if (u.Button(ID_DRIFT, b, L"Включить снова", Kind::Primary)) Defer([this] { Reapply(); });
        y += bannerH;
    }
    bool any = false;
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
        if (rows[gi].empty()) continue;
        any = true;
        float folded = u.Anim(ID_GROUP + (int)gi, folded_[gi], 18);
        float bodyH = rows[gi].size() * rowH * (1 - folded);
        RectF card(cardX, y, cardW, headH + bodyH);
        if (card.Y + card.Height >= view.Y && card.Y <= view.Y + view.Height) {
            u.Fill(card, c.card, u.S(14));
            u.Stroke(card, c.border, u.S(14));
            // заголовок группы: имя, сколько включено и полоска доли
            int on = 0;
            for (size_t i : rows[gi]) on += st_[i].state == shutup::State::On;
            RectF head(cardX, y, cardW, headH);
            if (u.Click(ID_GROUP + (int)gi, head)) folded_[gi] = !folded_[gi];
            float hot = u.Anim(ID_GROUP + 50 + (int)gi, u.Hot(ID_GROUP + (int)gi), 16);
            if (hot > 0.01f) u.Fill(head, WithAlpha(c.text, (BYTE)(10 * hot)), u.S(14));
            {  // галочка свёртки: вниз - открыта, поворачивается вправо, когда группа свёрнута
                GraphicsState st = u.G().Save();
                u.G().TranslateTransform(cardX + u.S(24), y + headH / 2);
                u.G().RotateTransform(-90.0f * folded);
                Pen pen(MixColor(c.muted, c.text, hot), u.S(1.8f));
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapRound);
                pen.SetLineJoin(LineJoinRound);
                PointF pts[3] = {{-u.S(4.5f), -u.S(2.2f)}, {0, u.S(2.6f)}, {u.S(4.5f), -u.S(2.2f)}};
                u.G().DrawLines(&pen, pts, 3);
                u.G().Restore(st);
            }
            u.Text(groups_[gi], RectF(cardX + u.S(36), y + u.S(13), cardW * 0.5f, u.S(24)), c.text, 14.5f, true);
            std::wstring count = L"включено " + std::to_wstring(on) + L" из " + std::to_wstring(rows[gi].size());
            float bw = u.S(120), bx = cardX + cardW - u.S(20) - bw;
            u.Text(count, RectF(bx - u.S(170), y + u.S(15), u.S(160), u.S(20)), c.muted, 11.0f, false, ui::Right);
            RectF track(bx, y + u.S(22), bw, u.S(5));
            u.Fill(track, c.field, u.S(3));
            float share = (float)on / rows[gi].size();
            if (share > 0) u.Fill(RectF(bx, track.Y, bw * share, track.Height), c.accent, u.S(3));
        }
        y += headH;
        if (bodyH > 0.5f) {
            GraphicsState inner = u.G().Save();
            u.G().SetClip(RectF(cardX, y, cardW, bodyH), CombineModeIntersect);
            for (size_t n = 0; n < rows[gi].size(); ++n) {
                size_t i = rows[gi][n];
                const shutup::Setting& s = all[i];
                RectF row(cardX, y + n * rowH, cardW, rowH);
                if (row.Y + row.Height < view.Y || row.Y > view.Y + view.Height || row.Y > y + bodyH) continue;
                u.Fill(RectF(cardX + u.S(16), row.Y, cardW - u.S(32), 1), c.border);  // разделитель
                bool on = st_[i].state == shutup::State::On;
                RectF tg(cardX + u.S(20), row.Y + u.S(14), u.S(44), u.S(26));
                bool v = on;
                if (u.Toggle(ID_TOGGLE + (int)i, tg, v) && folded < 0.01f) Defer([this, i, v] { Toggle(i, v); });
                // уровень - цветная точка у заголовка, как светофор ShutUp10
                Color lc = LevelColor(s.level);
                float tx = cardX + u.S(80);
                u.Fill(RectF(tx, row.Y + u.S(19), u.S(8), u.S(8)), lc, u.S(4));
                u.Tip(ID_TOGGLE + (int)i, LevelName(s.level));
                float titleW = u.Measure(s.title, 13.0f, true);
                u.Text(s.title, RectF(tx + u.S(16), row.Y + u.S(11), cardW * 0.62f, u.S(22)), on ? c.text : c.muted,
                       13.0f, true);
                // метки справа от заголовка: сбросилось/частично, для компьютера, изменено здесь, нужен перезапуск
                float cx = tx + u.S(16) + std::min(titleW, cardW * 0.62f) + u.S(10);
                auto chip = [&](const std::wstring& t, Color bg, Color fg) {
                    float cw = u.Measure(t, 10.0f) + u.S(14);
                    u.Chip(RectF(cx, row.Y + u.S(12), cw, u.S(19)), t, bg, fg);
                    cx += cw + u.S(6);
                };
                if (st_[i].Drifted())
                    chip(L"сбросилось", c.warnBg, c.warn);
                else if (st_[i].state == shutup::State::Partial)
                    chip(L"частично", c.warnBg, c.warn);
                if (s.Machine()) chip(L"для всех", c.field, c.muted);
                if (st_[i].ours && !st_[i].Drifted()) chip(L"изменено здесь", WithAlpha(c.accent, 40), c.accent);
                shutup::Restart need = shutup::RestartOf(s);
                if (need == shutup::Restart::Reboot) chip(L"после перезагрузки", c.field, c.muted);
                u.Text(s.why, RectF(tx + u.S(16), row.Y + u.S(34), cardW - u.S(140), u.S(30)), c.muted, 11.0f, false,
                       ui::Left, true);
            }
            u.G().Restore(inner);
        }
        y += bodyH + gap;
    }
    if (!any)
        u.EmptyState(ID_EMPTY, RectF(cardX, top + bannerH + u.S(40), cardW, u.S(200)),
                     L"Ничего не найдено - смените фильтр.");
    u.G().Restore(clip);

    // ---------------- шапка поверх списка ----------------
    u.Fill(RectF(0, 0, w, top), c.win);
    u.Hover(ID_CHROME, RectF(0, 0, w, top));
    u.Text(L"RadialShutUp", RectF(pad, u.S(18), u.S(400), u.S(34)), c.text, 22.0f, true);
    u.Text(L"Приватность Windows: понятные переключатели, каждый можно вернуть как было.",
           RectF(pad, u.S(52), w * 0.6f, u.S(22)), c.muted, 12.0f);
    if (admin_) {
        std::wstring t = foreign_ ? L"Права другой учётной записи" : L"С правами администратора";
        float cw = u.Measure(t, 10.5f) + u.S(20);
        u.Chip(RectF(w - pad - cw, u.S(26), cw, u.S(24)), t, foreign_ ? c.warnBg : WithAlpha(c.accent, 46),
               foreign_ ? c.warn : c.accent);
    } else if (u.Button(ID_ELEVATE, RectF(w - pad - u.S(230), u.S(20), u.S(230), u.S(36)), L"Перезапустить с правами",
                        Kind::Primary)) {
        Defer([this] { Elevate(); });
    }
    // поиск и фильтр
    u.Field(
        ID_SEARCH, RectF(pad, u.S(84), u.S(260), u.S(34)), query_, [this](const std::wstring& v) { query_ = v; },
        L"Поиск настройки  (Ctrl+F)");
    u.Segmented(ID_FILTER, RectF(pad + u.S(274), u.S(84), std::min(u.S(620), w - pad * 2 - u.S(274)), u.S(34)),
                {L"Все", L"Рекомендуемые", L"С оговорками", L"Не рекомендуемые", L"Изменённые"}, filter_);
    // действия: слева правка, справа служебное; не влезают - короткие подписи
    struct Act {
        int id;
        const wchar_t *full, *brief;
        Kind kind;
        std::function<void()> run;
    };
    std::vector<Act> left = {
        {ID_RECOMMENDED, L"Включить рекомендуемые", L"Рекомендуемые", Kind::Primary,
         [this] { Bulk(shutup::Level::Recommended); }},
        {ID_LIMITED, L"+ с оговорками", L"+ с оговорками", Kind::Secondary, [this] { Bulk(shutup::Level::Limited); }},
        {ID_REVERT, L"Вернуть всё как было", L"Вернуть всё", Kind::Secondary, [this] { RevertAll(); }}};
    std::vector<Act> right = {
        {ID_RESTORE, L"Точка восстановления", L"Точка восст.", Kind::Ghost, [this] { Restore(); }},
        {ID_EXPORT, L"Сохранить профиль", L"Сохранить", Kind::Ghost, [this] { Export(); }},
        {ID_IMPORT, L"Загрузить профиль", L"Загрузить", Kind::Ghost, [this] { Import(); }}};
    const float by = u.S(130), bh = u.S(34), bgap = u.S(8);
    auto widthOf = [&](const Act& a, bool brief) { return u.Measure(brief ? a.brief : a.full, 12.5f, true) + u.S(34); };
    auto rowW = [&](const std::vector<Act>& v, bool brief) {
        float s = 0;
        for (const Act& a : v) s += widthOf(a, brief) + bgap;
        return s;
    };
    const float room = w - 2 * pad - u.S(16);  // сначала короче служебное справа, потом и правка слева
    bool briefR = rowW(left, false) + rowW(right, false) > room, briefL = rowW(left, false) + rowW(right, true) > room;
    float bx = pad;
    for (const Act& a : left) {
        float bw = widthOf(a, briefL);
        if (u.Button(a.id, RectF(bx, by, bw, bh), briefL ? a.brief : a.full, a.kind)) Defer(a.run);
        if (briefL) u.Tip(a.id, a.full);
        bx += bw + bgap;
    }
    bx = w - pad - rowW(right, briefR) + bgap;
    for (const Act& a : right) {
        float bw = widthOf(a, briefR);
        if (u.Button(a.id, RectF(bx, by, bw, bh), briefR ? a.brief : a.full, a.kind)) Defer(a.run);
        if (briefR) u.Tip(a.id, a.full);
        bx += bw + bgap;
    }
    u.Fill(RectF(0, top - 1, w, 1), c.border);

    // ---------------- строка состояния ----------------
    RectF bar(0, h - bottom, w, bottom);
    u.Fill(bar, c.rail);
    u.Hover(ID_CHROME, bar);
    u.Fill(RectF(0, bar.Y, w, 1), c.border);
    // справа: отменить последнее и перезапуск Проводника, когда они есть; иначе легенда уровней
    float rx = w - pad;
    const float sbh = u.S(30), sby = bar.Y + (bottom - sbh) / 2;
    if (needExplorer_) {
        std::wstring t = L"Перезапустить Проводник";
        float bw = u.Measure(t, 12.5f, true) + u.S(34);
        rx -= bw;
        if (u.Button(ID_EXPLORER, RectF(rx, sby, bw, sbh), t, Kind::Primary)) Defer([this] { Explorer(); });
        u.Tip(ID_EXPLORER, L"Кнопки панели задач и Пуск Проводник перечитывает только при запуске");
        rx -= bgap;
    }
    if (!last_.empty()) {
        std::wstring t = L"Отменить";
        float bw = u.Measure(t, 12.5f, true) + u.S(34);
        rx -= bw;
        if (u.Button(ID_UNDO, RectF(rx, sby, bw, sbh), t, Kind::Secondary)) Defer([this] { Undo(); });
        u.Tip(ID_UNDO, L"Отменить: " + lastWhat_ + L"  (Ctrl+Z)");
        rx -= bgap;
    }
    if (rx > w - pad - 1) {
        for (int l = 2; l >= 0; --l) {  // легенда уровней справа
            auto lev = (shutup::Level)l;
            float tw = u.Measure(LevelName(lev), 10.5f);
            rx -= tw;
            u.Text(LevelName(lev), RectF(rx, bar.Y + u.S(13), tw + u.S(4), u.S(18)), c.muted, 10.5f);
            rx -= u.S(14);
            u.Fill(RectF(rx, bar.Y + u.S(18), u.S(8), u.S(8)), LevelColor(lev), u.S(4));
            rx -= u.S(18);
        }
    }
    int on = 0, ours = 0;
    for (const shutup::Status& s : st_) {
        on += s.state == shutup::State::On;
        ours += s.ours;
    }
    std::wstring status = L"Включено " + std::to_wstring(on) + L" из " + std::to_wstring(all.size()) +
                          L", изменено RadialShutUp: " + std::to_wstring(ours);
    if (needReboot_) status += L". Часть заработает после перезагрузки";
    if (!msg_.empty()) status = msg_;
    u.Text(status, RectF(pad, bar.Y + u.S(12), std::max(0.0f, rx - pad - u.S(12)), u.S(20)),
           msg_.empty() ? c.muted : c.text, 11.0f);
    u.DrawTip(w, h);
}

LRESULT App::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    auto after = [&] {
        if (ui_.TakeRepaint()) InvalidateRect(wnd_, nullptr, FALSE);
    };
    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd_, 0};
            TrackMouseEvent(&tme);
            ui_.OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp));
            if (ui_.HotChanged()) SetTimer(wnd_, kTickTimer, 650, nullptr);  // подсказка, если задержаться
            after();
            return 0;
        }
        case WM_MOUSELEAVE:
            ui_.OnLeave();
            after();
            return 0;
        case WM_LBUTTONDOWN:
            ui_.OnDown((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_LBUTTONUP:
            ui_.OnUp((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_LBUTTONDBLCLK:
            ui_.OnDown((short)LOWORD(lp), (short)HIWORD(lp));
            ui_.OnDouble((short)LOWORD(lp), (short)HIWORD(lp));
            after();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(wnd_, &p);
            ui_.OnWheel(p.x, p.y, (short)HIWORD(wp));
            after();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (HWND)wp == wnd_ && ui_.OnSetCursor()) return TRUE;
            break;
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE && ui_.OnEditChange((HWND)lp)) InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, ui_.FieldText());
            SetBkColor(dc, ui_.FieldBack());
            return (LRESULT)ui_.FieldBrush();
        }
        case WM_RUN: {
            auto todo = std::move(deferred_);
            deferred_.clear();
            for (auto& f : todo) f();
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            KillTimer(wnd_, wp);
            if (wp == kMsgTimer) msg_.clear();
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) {  // вернулись в окно - вдруг что-то поменяли в Параметрах
                Reload();
                InvalidateRect(wnd_, nullptr, FALSE);
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {(LONG)(900 * k_), (LONG)(560 * k_)};
            return 0;
        }
        case WM_DPICHANGED: {
            k_ = HIWORD(wp) / 96.0;
            RECT* r = (RECT*)lp;
            SetWindowPos(wnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                ui_.FocusField(ID_SEARCH);
                return 0;
            }
            if (wp == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) && !last_.empty()) {
                Defer([this] { Undo(); });
                return 0;
            }
            break;
        case WM_DESTROY:
            if (memDC_) {
                SelectObject(memDC_, oldBmp_);
                DeleteObject(dib_);
                DeleteDC(memDC_);
                memDC_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}

// ---------------- без окна ----------------

void Out(const std::wstring& line) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    std::string s = Narrow(line + L"\n");
    DWORD put = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &put, nullptr);
}

// включить список; итог строками. 0 - всё включилось, 1 - что-то пропущено или не вышло
int CliApply(const std::vector<size_t>& items) {
    bool admin = shutup::IsAdmin(), foreign = admin && ForeignAdmin();
    int done = 0, skipped = 0, failed = 0;
    const auto& all = shutup::Catalog();
    for (size_t i : items) {
        std::wstring why = BlockReason(all[i], admin, foreign);
        if (!why.empty()) {
            ++skipped;
            Out(L"@skip=" + std::wstring(all[i].id));
            continue;
        }
        bool ok = shutup::Apply(all[i], true, nullptr);
        (ok ? done : failed)++;
        Out(std::wstring(ok ? L"@on=" : L"@fail=") + all[i].id);
    }
    if (done) shutup::NotifyChanged(true);
    Out(L"@done=" + std::to_wstring(done));
    Out(L"@skipped=" + std::to_wstring(skipped));
    Out(L"@failed=" + std::to_wstring(failed));
    return skipped || failed ? 1 : 0;
}

// -1 - не служебный запуск, открыть окно
int Cli(const std::vector<std::wstring>& a) {
    if (a.empty()) return -1;
    const auto& all = shutup::Catalog();
    if (a[0] == L"--status") {
        std::vector<shutup::Status> st = AllStatus();
        int on = 0, ours = 0, drift = 0;
        for (size_t i = 0; i < st.size(); ++i) {
            on += st[i].state == shutup::State::On;
            ours += st[i].ours;
            if (st[i].Drifted()) ++drift, Out(L"@drift_id=" + std::wstring(all[i].id));
        }
        Out(L"@total=" + std::to_wstring(all.size()));
        Out(L"@on=" + std::to_wstring(on));
        Out(L"@ours=" + std::to_wstring(ours));
        Out(L"@drift=" + std::to_wstring(drift));
        return 0;
    }
    if (a[0] == L"--reapply") {
        std::vector<shutup::Status> st = AllStatus();
        std::vector<size_t> items;
        for (size_t i = 0; i < st.size(); ++i)
            if (st[i].Drifted()) items.push_back(i);
        return CliApply(items);
    }
    if (a[0] == L"--profile" && a.size() >= 2) {
        bool read = false;
        std::string bytes = ReadSmall(a[1], read);
        if (!read) {
            Out(L"@error=файл не открылся");
            return 2;
        }
        std::vector<shutup::Status> st = AllStatus();
        std::vector<size_t> items;
        for (const std::wstring& id : shutup::DecodeProfile(Widen(bytes)))
            for (size_t i = 0; i < all.size(); ++i)
                if (id == all[i].id && st[i].state != shutup::State::On) items.push_back(i);
        return CliApply(items);
    }
    if (a[0] == L"--revert") {
        bool admin = shutup::IsAdmin(), foreign = admin && ForeignAdmin();
        std::vector<shutup::Status> st = AllStatus();
        int done = 0, skipped = 0, failed = 0;
        for (size_t i = 0; i < st.size(); ++i) {
            if (!st[i].ours) continue;
            if (!BlockReason(all[i], admin, foreign).empty()) {
                ++skipped;
                Out(L"@skip=" + std::wstring(all[i].id));
                continue;
            }
            bool ok = shutup::Apply(all[i], false, nullptr);
            (ok ? done : failed)++;
            Out(std::wstring(ok ? L"@off=" : L"@fail=") + all[i].id);
        }
        if (done) shutup::NotifyChanged(true);
        Out(L"@done=" + std::to_wstring(done));
        Out(L"@skipped=" + std::to_wstring(skipped));
        Out(L"@failed=" + std::to_wstring(failed));
        return skipped || failed ? 1 : 0;
    }
    if (a[0] == L"--snap") return -1;  // окно нужно - снимок делает wWinMain
    Out(L"RadialShutUp: --status | --reapply | --profile файл | --revert | --snap файл.png [ширина высота [фильтр]]");
    return 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    std::vector<std::wstring> args;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
        LocalFree(argv);
    }
    int cli = Cli(args);
    if (cli >= 0) return cli;

    OleInitialize(nullptr);
    if (shutup::IsAdmin()) InitComSecurity();
    GdiplusStartupInput gin;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gin, nullptr);
    int code = 0;
    {
        // не static: окно и шрифты GDI+ в ui_ должны умереть до GdiplusShutdown, иначе выход падал (0xc0000005)
        App app;
        g_app = &app;
        bool snap = !args.empty() && args[0] == L"--snap" && args.size() >= 2;
        if (app.Create(inst, snap ? SW_HIDE : show)) {
            if (snap) {
                int w = args.size() >= 4 ? _wtoi(args[2].c_str()) : 1100,
                    h = args.size() >= 4 ? _wtoi(args[3].c_str()) : 780;
                int filter = args.size() >= 5 ? _wtoi(args[4].c_str()) : 0;
                code = app.Snap(args[1], std::max(w, 200), std::max(h, 200), filter) ? 0 : 1;
                DestroyWindow(FindWindowW(kClass, nullptr));
            } else {
                MSG m;
                while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                code = (int)m.wParam;
            }
        }
        g_app = nullptr;
    }
    GdiplusShutdown(token);
    OleUninitialize();
    return code;
}
