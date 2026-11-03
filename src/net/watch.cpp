// Сторож связи и проверка сети для игры.
#include "net/net_internal.h"

#include "net/watch.h"
#include "net/recovery.h"
#include "net/security.h"
#include "net/background.h"
#include "life/vpnexit.h"

#include <algorithm>
#include <map>
#include <memory>

#include "core/bg.h"
#include "core/text.h"

namespace net {

void Watch::Start(HWND notify, UINT msg) {
    stop_ = false;
    notify_ = notify;
    msg_ = msg;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    graceUntil_ = GetTickCount64() + 30000;  // при входе в Windows сеть поднимается не сразу
    recoveryUntil_ = graceUntil_.load();
    NotifyUnicastIpAddressChange(
        AF_INET,
        [](void* ctx, MIB_UNICASTIPADDRESS_ROW*, MIB_NOTIFICATION_TYPE) {
            auto* watch = static_cast<Watch*>(ctx);
            AcquireSRWLockExclusive(&watch->lock_);
            bool accepted = watch->addressBurst_.Observe(GetTickCount64());
            if (accepted) {
                watch->graceUntil_ = watch->addressBurst_.Until();
                watch->recoveryUntil_ = watch->addressBurst_.Until();
            }
            ReleaseSRWLockExclusive(&watch->lock_);
            if (accepted) watch->Kick(0);
            // Always notify the phone server: the final address in a burst still matters.
            auto* e = new Event;
            e->kind = Event::NETWORK;
            watch->Post(e);  // сервер телефона пересматривает адрес только в потоке окна
        },
        this, FALSE, &addressNotify_);
    routeDirty_ = true;
    NotifyRouteChange2(
        AF_INET,
        [](void* ctx, MIB_IPFORWARD_ROW2*, MIB_NOTIFICATION_TYPE) {
            auto* watch = static_cast<Watch*>(ctx);
            watch->routeDirty_ = true;
            SetEvent(watch->wake_);
        },
        this, FALSE, &routeNotify_);
    thread_ = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
}

void Watch::Stop() {
    if (routeNotify_) {
        CancelMibChangeNotify2(routeNotify_);
        routeNotify_ = nullptr;
    }
    if (addressNotify_) {
        CancelMibChangeNotify2(addressNotify_);
        addressNotify_ = nullptr;
    }
    if (!thread_) return;
    stop_ = true;
    SetEvent(wake_);
    // запрос мог зависнуть на таймауте - не держим выход дольше; но событие поток ещё ждёт: закрытое, его номер
    // достался бы чужому объекту - тогда оставляем (выход процесса всё равно всё закроет)
    bool done = WaitForSingleObject(thread_, 3000) == WAIT_OBJECT_0;
    CloseHandle(thread_);
    if (done) CloseHandle(wake_);
    wake_ = nullptr;
    thread_ = nullptr;
}

void Watch::Configure(bool on, bool exitWatch, int intervalSec, bool security) {
    on_ = on;
    exit_ = exitWatch;
    security_ = security;
    interval_ = std::clamp(intervalSec, 5, 600);
    fresh_ = true;
    if (wake_) SetEvent(wake_);
}

void Watch::Kick(int graceMs) {
    if (graceMs > 0) {
        graceUntil_ = GetTickCount64() + graceMs;
        recoveryUntil_ = graceUntil_.load();
    }  // открытие кольца (Kick(0)) не отменяет защиту от ложной тревоги после сна
    fresh_ = true;
    if (wake_) SetEvent(wake_);
}

void Watch::GuardOptions(bool changes, bool vpn, const std::wstring& expected) {
    changes_ = changes;
    vpn_ = vpn;
    routeDirty_ = true;
    AcquireSRWLockExclusive(&lock_);
    expectedRoute_ = expected;
    ReleaseSRWLockExclusive(&lock_);
    if (wake_) SetEvent(wake_);
}

Light Watch::Now() const {
    AcquireSRWLockShared(&lock_);
    Light l = light_;
    ReleaseSRWLockShared(&lock_);
    return l;
}

DWORD WINAPI Watch::Thread(void* self) {
    ((Watch*)self)->Loop();
    return 0;
}

void Watch::Post(Event* e) {
    if (!PostMessageW(notify_, msg_, 0, (LPARAM)e)) delete e;
}

void Watch::Loop() {
    InitSockets();
    int state = -1;  // -1 неизвестно, 0 нет связи, 1 есть
    ULONGLONG downAt = 0, exitAt = 0, rttAt = 0;
    int rtt = -1;
    std::wstring country;
    SecurityLatch safety;
    SettingsLatch settings;
    settings.Seed(LoadSettingBaseline());
    std::wstring savedBaseline;  // что последний раз записали в settings-baseline.tsv
    SecurityLatch vpnState;
    std::wstring expectedWas;
    ULONGLONG settingsAt = 0, vpnAt = 0;
    ULONGLONG safetyAt = 0;
    ULONGLONG probeAt = 0, budgetAt = 0;
    bool constrained = true, vpnRetry = false;
    while (!stop_) {
        ULONGLONG tick = GetTickCount64();
        if (changes_ && tick >= graceUntil_ && (!settingsAt || tick - settingsAt >= 30000)) {
            settingsAt = tick;
            auto stamps = ReadSettingStamps();
            if (GetTickCount64() >= graceUntil_) {
                auto diff = settings.Observe(stamps);
                // на диск - только когда отпечатки поменялись: раньше файл с WRITE_THROUGH писался каждые 30 с
                std::wstring saving;
                for (const auto& s : settings.Stable()) saving += s.name + L"\t" + s.digest + L"\t" + s.summary + L"\n";
                if (saving != savedBaseline && SaveSettingBaseline(settings.Stable())) savedBaseline = saving;
                AcquireSRWLockExclusive(&lock_);
                light_.settingsChecked = GetTickCount64();
                light_.settingsKnown = 0;
                for (const auto& s : stamps)
                    light_.settingsKnown |= s.name == L"DNS" ? 1 : s.name == L"Прокси" ? 2 : s.name == L"hosts" ? 4 : 0;
                ReleaseSRWLockExclusive(&lock_);
                for (const auto& d : diff) {
                    auto* ev = new Event;
                    ev->kind = Event::SETTINGS;
                    ev->text = d.name + L": было «" + d.before + L"», стало «" + d.after +
                               L"». Это изменение настройки, а не доказательство атаки.";
                    Remember(L"settings", ev->text);
                    Post(ev);
                }
            }
        }
        const DWORD vpnInterval = (!routeNotify_ || vpnRetry) ? 3000 : 60000;
        if (vpn_ && (routeDirty_.exchange(false) || !vpnAt || tick - vpnAt >= vpnInterval)) {
            vpnAt = tick;
            AcquireSRWLockShared(&lock_);
            auto expected = expectedRoute_;
            bool wasLost = light_.vpnLost;
            ReleaseSRWLockShared(&lock_);
            if (expected != expectedWas) {
                vpnState = SecurityLatch{};
                expectedWas = expected;
            }
            std::wstring route;
            bool tunnel = RouteTo(L"1.1.1.1", &route);
            bool known = tick >= graceUntil_ && !expected.empty() && !route.empty();
            bool lost = !ExpectedTunnel(Lower(expected), Lower(route), tunnel);
            unsigned added = vpnState.Observe({known ? 1u : 0u, lost ? 1u : 0u});
            // Confirm a new loss promptly; a single route event must suffice.
            // туннель не выбран - сверять не с чем: без частого опроса, хватит событий маршрута
            vpnRetry = !expected.empty() && (!known || (lost && !vpnState.Risks()));
            AcquireSRWLockExclusive(&lock_);
            light_.vpnChecked = GetTickCount64();
            light_.vpnKnown = known;
            light_.vpnLost = vpnState.Risks() != 0;
            ReleaseSRWLockExclusive(&lock_);
            if (added || (wasLost && known && !lost)) {
                auto* ev = new Event;
                ev->kind = Event::VPN;
                ev->text = added ? L"Маршрут к интернету больше не идёт через выбранный туннель «" + expected + L"»."
                                 : L"Маршрут через выбранный туннель восстановлен.";
                Remember(L"vpn", ev->text);
                Post(ev);
            }
        }
        if (!security_) {
            safety = SecurityLatch{};
            safetyAt = 0;
            AcquireSRWLockExclusive(&lock_);
            light_.securityRisks = 0;
            ReleaseSRWLockExclusive(&lock_);
        } else if (tick >= graceUntil_ && (!safetyAt || tick - safetyAt >= 30000)) {
            safetyAt = tick;
            SecurityState sample = ReadSecurity();
            // Не публиковать результат предыдущей сети после сна или смены адреса.
            if (!stop_ && security_ && GetTickCount64() >= graceUntil_) {
                unsigned added = safety.Observe(sample);
                AcquireSRWLockExclusive(&lock_);
                light_.securityRisks = safety.Risks();
                light_.securityKnown = sample.known;
                light_.securityObserved = sample.risks;
                light_.securityChecked = GetTickCount64();
                ReleaseSRWLockExclusive(&lock_);
                if (added) {
                    auto* ev = new Event;
                    ev->kind = Event::SECURITY;
                    ev->risks = added;
                    Remember(L"security", SecurityAdvice(added));  // без SSID, адресов и паролей
                    Post(ev);
                }
            }
        }
        if (!on_) {
            WaitForSingleObject(wake_, vpn_ && (!routeNotify_ || vpnRetry) ? 3000 : 30000);
            continue;
        }
        if (tick < probeAt && !fresh_) {
            auto cap = vpn_                      ? ((!routeNotify_ || vpnRetry) ? 3000u : 30000u)
                       : (security_ || changes_) ? 30000u
                                                 : 600000u;
            WaitForSingleObject(wake_, (DWORD)std::min<ULONGLONG>(probeAt - tick, cap));
            continue;
        }
        if (!budgetAt || tick - budgetAt >= 30000) {
            SYSTEM_POWER_STATUS power{};
            constrained = !GetSystemPowerStatus(&power) || power.ACLineStatus != 1 || !BackgroundTrafficAllowed();
            budgetAt = tick;
        }
        ULONGLONG recovering = recoveryUntil_.load();
        bool refreshRtt = fresh_.exchange(false);
        bool ok = HttpsOk(6000);
        if (!ok && !stop_ && GetTickCount64() >= recoveryUntil_) {
            WaitForSingleObject(wake_, 3000);  // одиночный сбой - не повод
            ok = HttpsOk(6000);
        }
        if (stop_) break;
        ULONGLONG now = GetTickCount64();
        Light l = Now();  // сохранить результаты независимых локальных проверок
        l.securityRisks = safety.Risks();
        l.known = true;
        l.probedAt = now;
        l.online = ok;
        l.tunnel = RouteTo(L"1.1.1.1", &l.route);
        l.pingMs = ok ? Ping(L"1.1.1.1", 1, 1000) : -1;
        if (ok && l.tunnel && (l.pingMs < 0 || Faked(l.pingMs))) {
            // TUN врёт на пинг; честный замер - четыре HTTPS-запроса, поэтому раз в минуту или по просьбе кольца
            if (refreshRtt || rtt < 0 || now - rttAt > 60000) {
                rtt = HttpRtt(4000);
                rttAt = now;
            }
            l.pingMs = rtt;
        }
        // выходной узел: раз в 5 минут и сразу после восстановления связи
        if (ok && (exitAt == 0 || now - exitAt > (constrained ? 30ull : 5ull) * 60 * 1000 || state == 0)) {
            Exit e = ExitNode(5000);
            exitAt = now;
            if (e.ok) {
                if (exit_ && !country.empty() && e.country != country) {
                    auto* ev = new Event;
                    ev->kind = Event::EXIT;
                    ev->text = CountryName(country) + L" -> " + CountryName(e.country);
                    Remember(L"exit", L"Выход сменился: " + ev->text + L" (" + e.ip + L")");
                    Post(ev);
                }
                country = e.country;
            }
        }
        l.country = country;
        AcquireSRWLockExclusive(&lock_);
        light_ = l;
        ReleaseSRWLockExclusive(&lock_);

        if (!ok && state != 0 && now >= graceUntil_) {
            ULONGLONG grace = graceUntil_.load();
            auto* ev = new Event;
            ev->kind = Event::DOWN;
            ev->report = Diagnose(false);
            // Сеть могла смениться/проснуться уже во время диагностики: её старый результат не тревожит.
            if (stop_ || graceUntil_ != grace || GetTickCount64() < graceUntil_) {
                delete ev;
                continue;
            }
            state = 0;
            downAt = now;
            Remember(L"down", L"Пропал интернет. " +
                                  (ev->report.fixReason.empty() ? Summary(ev->report) : ev->report.fixReason));
            Post(ev);
        } else if (ok && state == 0) {
            state = 1;
            int mins = (int)((now - downAt) / 60000);
            auto* ev = new Event;
            ev->kind = Event::UP;
            ev->text = mins >= 1 ? L"Не было " + std::to_wstring(mins) + L" мин." : L"Перерыв был меньше минуты.";
            Remember(L"up", L"Интернет вернулся. " + ev->text);
            Post(ev);
        } else if (ok) {
            state = 1;
        }
        // Пока сеть просыпается, не ждать обычные 20–600 секунд после первой неудачной пробы.
        if (ok) { recoveryUntil_.compare_exchange_strong(recovering, 0); }
        DWORD normal = BackgroundProbeInterval((DWORD)interval_ * 1000, constrained);
        DWORD delay = RecoveryWait(GetTickCount64(), recoveryUntil_, normal);
        probeAt = GetTickCount64() + delay;
        DWORD cap = vpn_ ? ((!routeNotify_ || vpnRetry) ? 3000 : 30000) : (security_ || changes_) ? 30000 : delay;
        WaitForSingleObject(wake_, std::min<DWORD>(delay, cap));
    }
}

// ---- игра ----

namespace {

struct GameJob {
    std::vector<DWORD> pids;
    std::wstring name;
    HWND notify;
    UINT msg;
};

bool PublicV4(DWORD addr) {
    const BYTE* b = (const BYTE*)&addr;
    if (b[0] == 10 || b[0] == 127 || b[0] == 0 || b[0] >= 224) return false;
    if (b[0] == 172 && b[1] >= 16 && b[1] < 32) return false;
    if (b[0] == 192 && b[1] == 168) return false;
    if (b[0] == 100 && b[1] >= 64 && b[1] < 128) return false;
    if (b[0] == 169 && b[1] == 254) return false;
    if (b[0] == 198 && (b[1] == 18 || b[1] == 19)) return false;  // адреса-заглушки TUN-режима (sing-box fake-ip)
    return true;
}

void GameWork(const GameJob* job) {
    if (bg::Quitting()) return;
    InitSockets();
    // соединения игры: какой адрес встречается чаще - тот и сервер
    std::map<std::pair<DWORD, int>, int> seen;
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    std::vector<BYTE> buf(size + 8192);
    size = (ULONG)buf.size();
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) == NO_ERROR) {
        auto* t = (MIB_TCPTABLE_OWNER_PID*)buf.data();
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            if (r.dwState != MIB_TCP_STATE_ESTAB || !PublicV4(r.dwRemoteAddr)) continue;
            if (std::find(job->pids.begin(), job->pids.end(), r.dwOwningPid) == job->pids.end()) continue;
            ++seen[{r.dwRemoteAddr, ntohs((u_short)r.dwRemotePort)}];
        }
    }
    auto* ev = new Event;
    ev->kind = Event::GAME;
    std::wstring route;
    if (!seen.empty()) {
        auto best = std::max_element(seen.begin(), seen.end(),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
        wchar_t ip[32];
        const BYTE* b = (const BYTE*)&best->first.first;
        swprintf(ip, 32, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        bool tunnel = RouteTo(ip, &route);
        int ms = Ping(ip, 3, 1000);
        if (ms < 0) ms = TcpRtt(ip, best->first.second, 2000);  // сервер не отвечает на пинг - время TCP
        bool exitOnly = false;
        if (tunnel && (ms < 0 || Faked(ms))) {  // TUN отвечает сам: настоящее время - хотя бы до выхода туннеля
            ms = HttpRtt(4000);
            exitOnly = true;
        }
        ev->text = job->name + L": сервер " + ip +
                   (ms >= 0 ? (exitOnly ? L", до выхода туннеля " : L", ") + std::to_wstring(ms) + L" мс" : L"") +
                   (tunnel ? L", идёт через туннель " + route : L", напрямую");
        ev->worth = tunnel || ms > 100;
        if (tunnel) {
            ev->text += L". Для игры лучше правило «мимо туннеля» в клиенте или выход ближе.";
            // Ж3: напрямую через Wi-Fi, страна выхода получше, что уже идёт мимо VPN
            int direct = vpnexit::DirectRtt(ip, best->first.second, 2000);
            Exit ex = ExitNode(4000);
            std::wstring better = ex.ok && ms > 0 ? vpnexit::Note(job->name, ex.country, ms) : L"";
            if (!better.empty())
                ev->text = job->name + L": " + std::to_wstring(ms) + L" мс через выход «" + CountryName(ex.country) +
                           L"». " + better;
            std::wstring more;
            if (ex.ok) more += L"Выход сейчас: " + CountryName(ex.country) + L". ";
            if (direct > 0)
                more += L"Напрямую через Wi-Fi до сервера " + std::to_wstring(direct) + L" мс, через туннель " +
                        std::to_wstring(ms) + L" мс. ";
            std::vector<std::wstring> by = vpnexit::Bypass();
            std::wstring list;
            for (const std::wstring& b : by) list += (list.empty() ? L"" : L", ") + b;
            more += by.empty() ? L"Мимо VPN в Happ сейчас ничего не идёт. "
                               : L"Мимо VPN в Happ сейчас идут: " + list + L". ";
            more +=
                L"Пустить игру мимо VPN: Happ - Маршрутизация - правило «напрямую» для её exe. Пинг по "
                L"странам выхода копится от игры к игре - когда другая страна окажется быстрее, скажу.";
            ev->more = more;
        } else if (ms > 100)
            ev->text += L". Пинг высокий: сервер далеко или канал занят.";
    } else {
        // соединений по TCP нет (игра на UDP): хотя бы куда сейчас идёт весь трафик
        bool tunnel = RouteTo(L"1.1.1.1", &route);
        ev->worth = tunnel;
        ev->text = job->name + (tunnel ? L": весь трафик идёт через туннель " + route +
                                             L" - пинг в игре будет до его выхода и обратно."
                                       : L": туннель не мешает, трафик идёт напрямую.");
    }
    Remember(L"game", ev->text);
    if (!PostMessageW(job->notify, job->msg, 0, (LPARAM)ev)) delete ev;
}

}  // namespace

void CheckGame(const std::vector<DWORD>& pids, const std::wstring& name, HWND notify, UINT msg) {
    GameJob job{pids, name, notify, msg};
    bg::Run([job] { GameWork(&job); });
}

}  // namespace net
