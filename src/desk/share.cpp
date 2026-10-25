// Режим показа экрана: свои окна - мимо захвата экрана, уведомления молчат.
#include "desk/share.h"

#include "core/config.h"
#include "core/procsnap.h"
#include "desk/gameboost.h"
#include "desk/gamemode.h"
#include "desk/modeswitch.h"

namespace share {

std::function<void(bool on)> onChange;

namespace {

const Config* g_cfg = nullptr;
ModeSwitch g_sw;
bool g_on = false, g_detected = false;
std::wstring g_reason;
ULONGLONG g_checkAt = 0;

// Запущена ли программа показа (маски share_apps по имени exe): какая; пусто - нет
std::wstring Detect() {
    if (!g_cfg || !g_cfg->shareAuto || g_cfg->shareApps.empty()) return L"";
    auto procs = procsnap::Get();
    for (const PROCESSENTRY32W& pe : *procs)
        if (gm::MatchAny(pe.szExeFile, g_cfg->shareApps)) return pe.szExeFile;
    return L"";
}

// все видимые окна верхнего уровня своего процесса - по режиму
void CoverAll(DWORD affinity) {
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && !GetParent(w)) {
                DWORD cur = 0;
                if (!GetWindowDisplayAffinity(w, &cur) || cur != (DWORD)lp) SetWindowDisplayAffinity(w, (DWORD)lp);
            }
            return TRUE;
        },
        (LPARAM)affinity);
}

void Set(bool on) {
    if (on == g_on) return;
    g_on = g_sw.on = on;
    gboost::QuietFor(L'p', on);
    CoverAll(Affinity());
    if (onChange) onChange(on);
}

}  // namespace

void Init(const Config* cfg) { g_cfg = cfg; }

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
    if (g_on && w) SetWindowDisplayAffinity(w, Affinity());
}

void Shutdown() {
    if (!g_on) return;
    g_on = false;
    gboost::QuietFor(L'p', false);
    CoverAll(WDA_NONE);
}

}  // namespace share
