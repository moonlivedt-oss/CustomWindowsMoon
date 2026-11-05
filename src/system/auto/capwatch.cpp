// Кто слушает микрофон или смотрит в камеру: поток ждёт перемен ConsentStore, основной поток сверяет и показывает.
#include <shlobj.h>

#include <atomic>

#include "system/auto/autos.h"
#include "core/config.h"
#include "core/text.h"
#include "core/themes.h"
#include "cards/boostcard.h"
#include "system/auto/capwatch.h"
#include "system/tools.h"

namespace capwatch {

namespace {

constexpr wchar_t kStore[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore";

const Config* g_cfg = nullptr;
BoostCard g_card;
HANDLE g_thread = nullptr, g_stop = nullptr;
std::atomic<bool> g_changed{true};  // первый раз - сверить сразу
std::set<std::wstring> g_seen;
std::vector<Use> g_queue;  // впервые включившие, ждут своей карточки
bool g_loaded = false;
ULONGLONG g_at = 0;

std::wstring File() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir.empty() ? L"" : dir + L"\\MoonDesk\\capseen.tsv";
}

bool LoadSeen() {
    HANDLE f = CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string u(std::min<DWORD>(GetFileSize(f, nullptr), 1 << 20), '\0');
    DWORD got = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &got, nullptr);
    CloseHandle(f);
    u.resize(got);
    for (const std::wstring& line : Split(Widen(u), L'\n'))
        if (!line.empty()) g_seen.insert(line);
    return true;
}

void SaveSeen() {
    std::wstring t;
    for (const std::wstring& s : g_seen) t += s + L"\n";
    std::string u = Narrow(t);
    HANDLE f = CreateFileW(File().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &put, nullptr);
    CloseHandle(f);
}

std::vector<Use> Now() {
    std::vector<Use> out;
    for (const tools::PrivacyUse& p : tools::PrivacyUses(false)) {
        if (p.capability == L"геопозиция") continue;
        out.push_back({p.capability, p.app, p.key, p.path, p.inUse, p.packaged});
    }
    return out;
}

DWORD WINAPI Watch(void*) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStore, 0, KEY_NOTIFY, &k) != ERROR_SUCCESS) return 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (;;) {
        if (RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, ev, TRUE) !=
            ERROR_SUCCESS)
            break;
        HANDLE h[2] = {g_stop, ev};
        if (WaitForMultipleObjects(2, h, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        g_changed = true;
    }
    CloseHandle(ev);
    RegCloseKey(k);
    return 0;
}

void Deny(const Use& u) {
    std::wstring path = std::wstring(kStore) + L"\\" + (u.cap == L"камера" ? L"webcam" : L"microphone") + L"\\" + u.key;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    const wchar_t v[] = L"Deny";
    RegSetValueExW(k, L"Value", 0, REG_SZ, (const BYTE*)v, sizeof(v));
    RegCloseKey(k);
    autos::Note(L"Запрещено: " + u.app + L" - " + u.cap + L" (вернуть - Параметры, Конфиденциальность)");
}

void Show(const Use& u) {
    std::wstring what = u.cap == L"камера" ? L"Камеру включил" : L"Микрофон включил";
    autos::Note(what + L": " + u.app + (u.path.empty() ? L"" : L" (" + u.path + L")"));
    g_card.kind = L"Микрофон и камера";
    g_card.yesLabel = u.packaged ? L"Запретить" : L"Параметры";
    g_card.noLabel = L"Это я";
    Use x = u;
    g_card.onYes = [x] {
        if (x.packaged) {
            Deny(x);
        } else {
            const wchar_t* page =
                x.cap == L"камера" ? L"ms-settings:privacy-webcam" : L"ms-settings:privacy-microphone";
            ShellExecuteW(nullptr, L"open", page, nullptr, nullptr, SW_SHOWNORMAL);
        }
    };
    g_card.onNo = nullptr;
    g_card.more = u.packaged
                      ? L"Приложение Магазина: «Запретить» выключит ему " + u.cap +
                            L" (как переключатель в Параметрах - Конфиденциальность; вернуть - там же)."
                      : L"Обычной программе Windows не даёт запретить " + u.cap +
                            L" отдельно - только всем обычным программам сразу (Параметры - Конфиденциальность). "
                            L"Если программа незнакома - закройте её и проверьте в «Защите».";
    g_card.Show(BoostCard::OFFER, what + L": " + u.app, u.path.empty() ? L"приложение Магазина" : u.path,
                CurrentTheme(*g_cfg), 30000, Toast::G_WARN);
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg) {
    g_cfg = cfg;
    g_card.Create(inst);
    g_loaded = LoadSeen();
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
}

void Tick() {
    g_card.Tick();
    if (!g_queue.empty() && !g_card.Visible()) {  // карточки по одной
        Use u = g_queue.front();
        g_queue.erase(g_queue.begin());
        Show(u);
    }
    ULONGLONG t = GetTickCount64();
    if (!g_changed || t - g_at < 500) return;  // запись идёт пачкой - собрать
    g_at = t;
    g_changed = false;
    std::vector<Use> now = Now();
    if (!g_loaded) {  // первый запуск: всё, что уже было, - знакомое
        for (const Use& u : now) g_seen.insert(SeenKey(u));
        g_loaded = true;
        SaveSeen();
        return;
    }
    std::vector<Use> fresh = Fresh(now, g_seen);
    if (fresh.empty()) return;
    for (const Use& u : fresh) g_seen.insert(SeenKey(u));
    SaveSeen();
    g_queue.insert(g_queue.end(), fresh.begin(), fresh.end());
}

void Shutdown() {
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
    }
    g_thread = nullptr;
    g_card.Destroy();
}

}  // namespace capwatch
