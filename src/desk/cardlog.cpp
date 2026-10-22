// Пропущенные карточки (Л10) и заглушённые виды (Л11): список в памяти, заглушки - в %APPDATA%\MoonDesk\cards.tsv.
#include "desk/cardlog.h"

#include <algorithm>

#include "core/text.h"

namespace cardlog {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Missed> g_list;  // новые сверху
int g_next = 1;
unsigned g_gen = 1;
Strikes g_strikes;
std::set<std::wstring> g_mutes;
bool g_mutesRead = false;
std::function<void(const std::wstring&)> g_ask;
std::function<bool(const std::wstring&, const std::wstring&)> g_digest;

std::wstring MutesFile() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%\\MoonDesk", p, MAX_PATH);
    CreateDirectoryW(p, nullptr);
    return std::wstring(p) + L"\\cards.tsv";
}

// под g_lock
void ReadMutes() {
    if (g_mutesRead) return;
    g_mutesRead = true;
    HANDLE f = CreateFileW(MutesFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string raw(64 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, raw.data(), (DWORD)raw.size(), &got, nullptr);
    CloseHandle(f);
    raw.resize(got);
    g_mutes = ParseMutes(Widen(raw));
}

void WriteMutes() {
    std::string s = Narrow(MutesText(g_mutes));
    HANDLE f = CreateFileW(MutesFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &put, nullptr);
    CloseHandle(f);
}

void Prune() {  // под g_lock: старше суток и сверх 12 - прочь
    ULONGLONG now = GetTickCount64();
    g_list.erase(std::remove_if(g_list.begin(), g_list.end(),
                                [&](const Missed& m) { return now - m.tick > 24ull * 3600 * 1000; }),
                 g_list.end());
    if (g_list.size() > 12) g_list.resize(12);
}

}  // namespace

void Record(Missed m) {
    if (m.title.empty() || !m.onYes) return;
    if (m.kind.empty()) m.kind = KindOf(m.title);
    GetLocalTime(&m.at);
    m.tick = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    g_list.erase(std::remove_if(g_list.begin(), g_list.end(),
                                [&](const Missed& o) { return o.title == m.title && o.sub == m.sub; }),
                 g_list.end());
    m.id = g_next++;
    g_list.insert(g_list.begin(), std::move(m));
    Prune();
    ++g_gen;
    ReleaseSRWLockExclusive(&g_lock);
}

std::vector<Missed> List() {
    AcquireSRWLockExclusive(&g_lock);
    Prune();
    std::vector<Missed> v = g_list;
    ReleaseSRWLockExclusive(&g_lock);
    return v;
}

int Count() {
    AcquireSRWLockShared(&g_lock);
    int n = (int)g_list.size();
    ReleaseSRWLockShared(&g_lock);
    return n;
}

unsigned Gen() { return g_gen; }

bool Press(int id, bool yes) {
    std::function<void()> f;
    std::wstring kind;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_list.size(); ++i)
        if (g_list[i].id == id) {
            f = yes ? g_list[i].onYes : g_list[i].onNo;
            kind = g_list[i].kind;
            g_list.erase(g_list.begin() + (long long)i);
            ++g_gen;
            break;
        }
    ReleaseSRWLockExclusive(&g_lock);
    if (kind.empty()) return false;
    if (yes) Closed(kind, true);  // всё-таки нужна: счёт закрытий с нуля
    if (f) f();                   // вне замка: действие может показать новую карточку
    return true;
}

void Forget(int id) {
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_list.size(); ++i)
        if (g_list[i].id == id) {
            g_list.erase(g_list.begin() + (long long)i);
            ++g_gen;
            break;
        }
    ReleaseSRWLockExclusive(&g_lock);
}

void Closed(const std::wstring& kind, bool acted) {
    AcquireSRWLockExclusive(&g_lock);
    ReadMutes();
    bool ask = !g_mutes.count(kind) && g_strikes.Closed(kind, acted);
    ReleaseSRWLockExclusive(&g_lock);
    if (ask && g_ask) g_ask(kind);
}

bool Muted(const std::wstring& kind) {
    AcquireSRWLockExclusive(&g_lock);
    ReadMutes();
    bool m = g_mutes.count(kind) > 0;
    ReleaseSRWLockExclusive(&g_lock);
    return m;
}

void Mute(const std::wstring& kind, bool on) {
    AcquireSRWLockExclusive(&g_lock);
    ReadMutes();
    if (on)
        g_mutes.insert(kind);
    else
        g_mutes.erase(kind);
    g_strikes.n.erase(kind);
    WriteMutes();
    ++g_gen;
    ReleaseSRWLockExclusive(&g_lock);
}

std::set<std::wstring> MutedKinds() {
    AcquireSRWLockExclusive(&g_lock);
    ReadMutes();
    std::set<std::wstring> m = g_mutes;
    ReleaseSRWLockExclusive(&g_lock);
    return m;
}

void SetAsk(std::function<void(const std::wstring&)> ask) { g_ask = std::move(ask); }

void SetDigest(std::function<bool(const std::wstring&, const std::wstring&)> digest) { g_digest = std::move(digest); }

bool ToDigest(const std::wstring& title, const std::wstring& text) { return g_digest && g_digest(title, text); }

}  // namespace cardlog
