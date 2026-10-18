// Сторож связи и проверка сети для игры.
#include "net/net_internal.h"

#include "net/watch.h"

#include <algorithm>
#include <map>
#include <memory>

#include "core/bg.h"
#include "core/text.h"

namespace net {

void Watch::Start(HWND notify, UINT msg) {
    notify_ = notify;
    msg_ = msg;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    graceUntil_ = GetTickCount64() + 30000;  // при входе в Windows сеть поднимается не сразу
    thread_ = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
}

void Watch::Stop() {
    if (!thread_) return;
    stop_ = true;
    SetEvent(wake_);
    WaitForSingleObject(thread_, 3000);  // запрос мог зависнуть на таймауте - не держим выход дольше
    CloseHandle(thread_);
    CloseHandle(wake_);
    thread_ = nullptr;
}

void Watch::Configure(bool on, bool exitWatch, int intervalSec) {
    on_ = on;
    exit_ = exitWatch;
    interval_ = std::clamp(intervalSec, 5, 600);
}

void Watch::Kick(int graceMs) {
    graceUntil_ = GetTickCount64() + graceMs;
    fresh_ = true;
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
    while (!stop_) {
        if (!on_) {
            WaitForSingleObject(wake_, 5000);
            continue;
        }
        bool ok = HttpsOk(6000);
        if (!ok && !stop_) {
            WaitForSingleObject(wake_, 3000);  // одиночный сбой - не повод
            ok = HttpsOk(6000);
        }
        if (stop_) break;
        ULONGLONG now = GetTickCount64();
        Light l;
        l.known = true;
        l.online = ok;
        l.tunnel = RouteTo(L"1.1.1.1", &l.route);
        l.pingMs = ok ? Ping(L"1.1.1.1", 1, 1000) : -1;
        if (ok && l.tunnel && (l.pingMs < 0 || Faked(l.pingMs))) {
            // TUN врёт на пинг; честный замер - четыре HTTPS-запроса, поэтому раз в минуту или по просьбе кольца
            if (fresh_ || rtt < 0 || now - rttAt > 60000) {
                rtt = HttpRtt(4000);
                rttAt = now;
            }
            l.pingMs = rtt;
        }
        fresh_ = false;
        // выходной узел: раз в 5 минут и сразу после восстановления связи
        if (ok && (exitAt == 0 || now - exitAt > 5 * 60 * 1000 || state == 0)) {
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
            state = 0;
            downAt = now;
            auto* ev = new Event;
            ev->kind = Event::DOWN;
            ev->report = Diagnose(false);
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
        WaitForSingleObject(wake_, (DWORD)interval_ * 1000);
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
    Sleep(20000);  // игра успевает подключиться к серверу
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
        if (tunnel)
            ev->text += L". Для игры лучше правило «мимо туннеля» в клиенте или выход ближе.";
        else if (ms > 100)
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
