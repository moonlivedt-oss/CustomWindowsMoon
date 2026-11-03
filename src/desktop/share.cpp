#include "core/usage.h"
#include <atomic>
// Режим показа экрана: свои окна - мимо захвата экрана, уведомления молчат.
#include "desktop/share.h"

#include <algorithm>

#include "core/config.h"
#include "core/procsnap.h"
#include "core/text.h"
#include "modes/gameboost.h"
#include "modes/gamemode.h"
#include "modes/modeswitch.h"

namespace share {

std::function<void(bool on)> onChange;

namespace {
std::atomic<bool> g_maskUsed{false};

const Config* g_cfg = nullptr;
ModeSwitch g_sw;
bool g_on = false, g_detected = false;
std::wstring g_reason;
ULONGLONG g_checkAt = 0;
std::vector<std::wstring> g_words;  // имена сетей и прочее, что прятать (кроме имени и компьютера)
SRWLOCK g_wordsLock = SRWLOCK_INIT;

// Окно-полоска показа экрана из браузера или звонка: видимое, низкое (полоска, а не страница), чужое, в заголовке
// одно из слов share_titles. Имя программы; пусто - такого окна нет.
std::wstring SharingBar() {
    struct Box {
        const std::wstring* masks;
        DWORD pid = 0;
    } box{&g_cfg->shareTitles};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            auto* b = (Box*)lp;
            if (!IsWindowVisible(w) || IsIconic(w)) return TRUE;
            RECT r;
            if (!GetWindowRect(w, &r) || r.bottom - r.top > 240 * (LONG)GetDpiForWindow(w) / 96) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            wchar_t t[256];
            int n = GetWindowTextW(w, t, 256);
            if (n <= 0 || !TitleHit(std::wstring(t, n), *b->masks)) return TRUE;
            b->pid = pid;
            return FALSE;
        },
        (LPARAM)&box);
    if (!box.pid) return L"";
    std::wstring exe = gm::FileName(gm::ProcessPath(box.pid));
    return exe.empty() ? L"показ из браузера" : exe;
}

// Запущена ли программа показа (маски share_apps по имени exe) или видна полоска показа: что; пусто - нет
std::wstring Detect() {
    if (!g_cfg || !g_cfg->shareAuto) return L"";
    if (!g_cfg->shareApps.empty()) {
        auto procs = procsnap::Get();
        for (const PROCESSENTRY32W& pe : *procs)
            if (gm::MatchAny(pe.szExeFile, g_cfg->shareApps)) return pe.szExeFile;
    }
    return g_cfg->shareTitles.empty() ? L"" : SharingBar();
}

// папки пользователя (длинная и короткая запись) и имена, которые прятать
void Personal(std::vector<std::wstring>& profiles, std::vector<std::wstring>& words) {
    static std::vector<std::wstring> p, w;
    if (p.empty()) {
        wchar_t b[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", b, MAX_PATH)) {
            p.push_back(b);
            wchar_t s[MAX_PATH];
            if (GetShortPathNameW(b, s, MAX_PATH) && _wcsicmp(s, b)) p.push_back(s);
        }
        DWORD n = MAX_PATH;
        if (GetUserNameW(b, &n)) w.push_back(b);
        n = MAX_PATH;
        if (GetComputerNameW(b, &n)) w.push_back(b);
    }
    profiles = p;
    words = w;
}

// все видимые окна верхнего уровня своего процесса - по режиму
void CoverAll(DWORD affinity) {
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && !GetParent(w)) {
                DWORD cur = 0;
                if (!GetWindowDisplayAffinity(w, &cur) || cur != (DWORD)lp) {
                    if (SetWindowDisplayAffinity(w, (DWORD)lp) && lp) usage::Hit(L"share_hide");
                }
            }
            return TRUE;
        },
        (LPARAM)affinity);
}

void Set(bool on) {
    if (on == g_on) return;
    g_maskUsed = false;
    g_on = g_sw.on = on;
    if (on && !g_sw.manual && g_cfg && g_cfg->shareAuto) usage::Hit(L"share_auto");
    gboost::QuietFor(L'p', on);
    CoverAll(Affinity());
    if (onChange) onChange(on);
}

}  // namespace

void Init(const Config* cfg) { g_cfg = cfg; }

bool Masking() { return g_on && g_cfg && g_cfg->shareMask; }

std::wstring Mask(const std::wstring& s) {
    if (!Masking() || s.empty()) return s;
    std::vector<std::wstring> profiles, words;
    Personal(profiles, words);
    AcquireSRWLockShared(&g_wordsLock);
    words.insert(words.end(), g_words.begin(), g_words.end());
    ReleaseSRWLockShared(&g_wordsLock);
    auto masked = MaskText(s, profiles, words);
    if (masked != s && !g_maskUsed.exchange(true)) usage::Hit(L"share_mask");
    return masked;
}

void HideWords(const std::vector<std::wstring>& words) {
    AcquireSRWLockExclusive(&g_wordsLock);
    for (const std::wstring& w : words)
        if (w.size() >= 3 && std::find(g_words.begin(), g_words.end(), w) == g_words.end() && g_words.size() < 64)
            g_words.push_back(w);
    ReleaseSRWLockExclusive(&g_wordsLock);
}

void Tick() {
    ULONGLONG now = GetTickCount64();
    if (now < g_checkAt) return;
    g_checkAt = now + 1000;
    std::wstring app = Detect();
    g_detected = !app.empty();
    if (g_detected && !g_sw.manual) g_reason = app;
    Set(g_sw.Want(now, g_detected));
    if (g_on) CoverAll(Affinity());  // окна, открытые с прошлой проверки
}

void Toggle() {
    bool want = !g_on;
    g_sw.SetManual(want, GetTickCount64(), g_detected);
    if (want) g_reason = L"вручную";
    Set(want);
}

bool On() { return g_on; }
bool Manual() { return g_sw.manual; }
const std::wstring& Reason() { return g_reason; }

DWORD Affinity() { return g_on && (!g_cfg || g_cfg->shareHide) ? 0x11 /* WDA_EXCLUDEFROMCAPTURE */ : WDA_NONE; }

void Cover(HWND w) {
    if (g_on && w && SetWindowDisplayAffinity(w, Affinity()) && Affinity()) usage::Hit(L"share_hide");
}

void Shutdown() {
    if (!g_on) return;
    g_on = false;
    gboost::QuietFor(L'p', false);
    CoverAll(WDA_NONE);
}

}  // namespace share
