// Режим фокуса (Л8) и шахматы без отвлечений (Л34): тишина, усыплённые отвлекающие программы, ожидание тяжёлого.
#include <windows.h>

#include <algorithm>

#include "app/autos.h"
#include "app/focus.h"
#include "app/toolsui.h"
#include "core/config.h"
#include "core/fx.h"
#include "core/procsnap.h"
#include "core/themes.h"
#include "desk/boostcard.h"
#include "desk/gameboost.h"
#include "tools/guard.h"

namespace focus {

namespace {

const Config* g_cfg = nullptr;
BoostCard g_card;
bool g_on = false, g_auto = false, g_softened = false;
ULONGLONG g_until = 0, g_startedAt = 0, g_checkAt = 0, g_autoSeenAt = 0;
std::wstring g_autoWhat;

std::vector<std::wstring> Titles() {
    std::vector<std::wstring> out;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            wchar_t t[512];
            int n = GetWindowTextW(w, t, 512);
            if (n > 0) ((std::vector<std::wstring>*)lp)->push_back(std::wstring(t, n));
            return TRUE;
        },
        (LPARAM)&out);
    return out;
}

// отвлекающие программы: окна свернуть, процессы усыпить (EcoQoS, память в подкачку) - вернёт Restore('s')
void Soften() {
    if (!g_cfg || g_cfg->focusApps.empty()) return;
    std::vector<DWORD> pids;
    for (const procsnap::Info& p : procsnap::Session())
        if (guard::Match(p.name, g_cfg->focusApps)) pids.push_back(p.pid);
    if (pids.empty()) return;
    struct Box {
        std::vector<DWORD>* pids;
    } box{&pids};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            auto* b = (Box*)lp;
            if (IsWindowVisible(w) && !GetWindow(w, GW_OWNER) &&
                std::find(b->pids->begin(), b->pids->end(), pid) != b->pids->end())
                ShowWindowAsync(w, SW_MINIMIZE);
            return TRUE;
        },
        (LPARAM)&box);
    gboost::Soften(pids);
    g_softened = true;
}

void Begin(bool automatic, int minutes) {
    g_on = true;
    g_auto = automatic;
    g_startedAt = GetTickCount64();
    g_until = automatic ? 0 : g_startedAt + (ULONGLONG)minutes * 60000;
    gboost::Quiet(true);
    toolsui::SetFocusHold(true);
    if (!automatic) Soften();
    autos::Note(automatic ? L"Фокус сам: открыт " + g_autoWhat + L" - тишина, тяжёлое ждёт"
                          : L"Фокус на " + std::to_wstring(minutes) + L" мин: тишина, отвлекающее усыплено");
}

void End(bool timeUp) {
    if (!g_on) return;
    ULONGLONG mins = (GetTickCount64() - g_startedAt) / 60000;
    g_on = false;
    if (!fx::Game()) gboost::Quiet(false);  // в игре тишину держит игровой режим
    toolsui::SetFocusHold(false);
    if (g_softened && !fx::Game()) gboost::Restore(L's');
    g_softened = false;
    autos::Note((g_auto ? L"Фокус сам закончился (" + g_autoWhat + L" закрыт)" : std::wstring(L"Фокус закончился")) +
                L", " + std::to_wstring(mins) + L" мин");
    if (!timeUp || !g_cfg) return;
    g_card.kind = L"-";
    g_card.yesLabel = L"Ещё 25 минут";
    g_card.noLabel = L"Перерыв";
    g_card.onYes = [] {
        g_card.yesLabel.clear(), g_card.noLabel.clear();
        Start(25);
    };
    g_card.onNo = [] { g_card.yesLabel.clear(), g_card.noLabel.clear(); };
    g_card.Show(BoostCard::OFFER, L"Перерыв",
                std::to_wstring(mins) +
                    L" минут фокуса позади. Встаньте, посмотрите вдаль, попейте воды - уведомления "
                    L"снова приходят, программы проснулись.",
                CurrentTheme(*g_cfg), 60000, Toast::G_OK);
}

}  // namespace

void Init(HINSTANCE inst, const Config* cfg) {
    g_cfg = cfg;
    g_card.Create(inst);
}

void Start(int minutes) {
    if (!g_cfg) return;
    if (minutes <= 0) minutes = g_cfg->focusMinutes;
    if (g_on) End(false);
    Begin(false, minutes);
}

void Stop() {
    g_autoWhat.clear();
    End(false);
}

bool On() { return g_on; }
bool Auto() { return g_on && g_auto; }

std::wstring Status() {
    if (!g_on) return L"";
    if (g_auto) return L"Тишина: открыт " + g_autoWhat;
    ULONGLONG now = GetTickCount64();
    return LeftText(g_until > now ? g_until - now : 0);
}

void Tick() {
    if (!g_cfg) return;
    g_card.Tick();
    ULONGLONG now = GetTickCount64();
    if (g_on && !g_auto && now >= g_until) {
        End(true);
        return;
    }
    if (now < g_checkAt) return;
    g_checkAt = now + 3000;
    if (g_on) gboost::Quiet(true);  // игра кончилась и вернула уведомления - а фокус ещё идёт
    if (g_cfg->focusAuto.empty() || (g_on && !g_auto)) return;
    std::wstring hit = AutoMatch(Titles(), g_cfg->focusAuto);
    if (!hit.empty()) {
        g_autoSeenAt = now;
        if (!g_on) {
            g_autoWhat = hit;
            Begin(true, 0);
        }
    } else if (g_on && g_auto && now - g_autoSeenAt > 30000) {
        End(false);
    }
}

void Shutdown() {
    End(false);
    g_card.Destroy();
}

}  // namespace focus
