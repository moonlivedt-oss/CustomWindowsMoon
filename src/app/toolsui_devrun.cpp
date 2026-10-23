// Наблюдатель запусков разработки (свой поток): раз в секунду, пока впереди VS Code или терминал, иначе раз в 5 с.
// Сторож процессов (раз в 5 с) короткие запуски не застаёт, а сборка с ошибкой и сервер, которому не дали порт,
// падают за секунду-другую. Здесь держим ручки:
//  - cmake --build (Л31): ненулевой код - карточка «Найти ошибку» (BuildFailed);
//  - сервер разработки из проекта (node, python...) (Л18): упал в первую минуту, а порт, на котором проект слушает
//    (tools::ProjectPorts), держит другой процесс - карточка «Порт занят» с «Остановить».
#include "app/toolsui_internal.h"

#include "core/procsnap.h"
#include "tools/devserv.h"
#include "tools/projver.h"
#include "tools/guard.h"

namespace toolsui {

namespace {

HANDLE g_devStop = nullptr, g_devThread = nullptr;
SRWLOCK g_devLock = SRWLOCK_INIT;

struct BuildFail {
    std::wstring exe, dir;  // cmake.exe и папка сборки (полный путь)
};
struct PortBusy {
    int port;
    DWORD holder;
    std::wstring holderExe, holderProject, holderCmd;  // кто держит
    std::wstring project;                              // кому не дали
    int holderMin;                                     // сколько держатель работает, мин (-1 - неизвестно)
};
std::vector<BuildFail> g_buildFails;  // под g_devLock
std::vector<PortBusy> g_portBusy;     // под g_devLock
struct EnvAsk {                       // Л33: терминал открыт в проекте, а нужной версии нет первой в PATH
    std::wstring project;
    std::vector<tools::EnvPick> picks;
};
std::vector<EnvAsk> g_envAsk;      // под g_devLock
std::set<std::wstring> g_envSeen;  // проекты, про которые уже смотрели (до перезапуска программы)

// Л33: новый терминал - в каком он проекте и всё ли там с версиями (в фоне: --version у cmake - процесс)
void CheckEnv(const std::wstring& project) {
    AcquireSRWLockExclusive(&g_devLock);
    bool fresh = g_envSeen.insert(Lower(project)).second;
    ReleaseSRWLockExclusive(&g_devLock);
    if (!fresh) return;
    std::vector<tools::EnvPick> picks = tools::ProjectPicks(project);
    if (picks.empty()) return;
    AcquireSRWLockExclusive(&g_devLock);
    g_envAsk.push_back({project, picks});
    ReleaseSRWLockExclusive(&g_devLock);
}

std::wstring ReadSmall(const std::wstring& path) {
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string s(64 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return Widen(s);
}

// впереди - то, откуда запускают сборки и серверы
bool DevForeground() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (!w || !GetWindowThreadProcessId(w, &pid)) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t img[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    bool ok = QueryFullProcessImageNameW(h, 0, img, &n);
    CloseHandle(h);
    if (!ok) return false;
    std::wstring name = img;
    name = name.substr(name.find_last_of(L'\\') + 1);
    return guard::Match(name,
                        L"Code.exe|Cursor.exe|WindowsTerminal.exe|OpenConsole.exe|conhost.exe|cmd.exe|"
                        L"powershell.exe|pwsh.exe|devenv.exe|idea64.exe|rider64.exe|clion64.exe|"
                        L"pycharm64.exe|webstorm64.exe|mintty.exe|wezterm-gui.exe|alacritty.exe");
}

struct Run {
    HANDLE h = nullptr;
    bool cmake = false;
    ULONGLONG t0 = 0;
    std::wstring exe, dir, cmd, project;
};

// сервер проекта упал в первую минуту: не занят ли его порт другим
void CheckPort(DWORD pid, const Run& r) {
    std::vector<int> ports =
        tools::ProjectPorts(r.cmd, ReadSmall(r.project + L"\\package.json"), ReadSmall(r.project + L"\\.env"));
    if (ports.empty()) return;
    std::vector<tools::Listener> ls = tools::Listeners();
    for (int port : ports)
        for (tools::Listener& l : ls) {
            if (l.port != port || l.udp || l.pid == pid || !l.pid) continue;
            std::vector<tools::Listener> one{l};
            one[0].dev = true;  // проект ищется только у dev
            tools::ListenerProjects(one);
            PortBusy b{port, l.pid, l.process, one[0].project, one[0].command, r.project, tools::ProcessMinutes(l.pid)};
            AcquireSRWLockExclusive(&g_devLock);
            g_portBusy.push_back(b);
            ReleaseSRWLockExclusive(&g_devLock);
            return;
        }
}

DWORD WINAPI DevLoop(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DWORD self = GetCurrentProcessId();
    std::map<DWORD, Run> runs;
    std::map<DWORD, ULONGLONG> skip;  // не наши (pid -> время создания: номер мог достаться новому)
    bool fast = false;
    do {
        std::vector<procsnap::Info> ps = procsnap::Session();
        ULONGLONG now = GetTickCount64();
        for (const procsnap::Info& p : ps) {
            if (runs.count(p.pid)) continue;
            auto sk = skip.find(p.pid);
            if (sk != skip.end() && sk->second == p.created) continue;
            skip[p.pid] = p.created;  // по умолчанию - больше не смотрим; интересное ниже уйдёт в runs
            bool cmake = guard::Match(p.name, L"cmake.exe"), srv = tools::DevServerName(p.name);
            if (!g_game && p.parent != self && guard::Match(p.name, L"cmd.exe|powershell.exe|pwsh.exe|bash.exe")) {
                // Л33: терминал - в его папке проект? (папку читаем сразу: потом в нём сделают cd)
                if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, p.pid)) {
                    std::wstring proj = tools::ProjectOfPath(tools::ProcCurrentDir(h));
                    CloseHandle(h);
                    if (!proj.empty()) CheckEnv(proj);
                }
                continue;
            }
            if ((!cmake && !srv) || p.parent == self || g_game) continue;
            HANDLE h = OpenProcess(
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                p.pid);
            if (!h) continue;
            Run r;
            r.h = h;
            r.cmake = cmake;
            r.t0 = now;
            r.cmd = tools::ProcCommandLine(h);
            std::vector<std::wstring> t = tools::CmdTokens(r.cmd);
            std::wstring cwd = tools::ProcCurrentDir(h);
            if (cmake) {
                auto at = std::find(t.begin(), t.end(), L"--build");
                wchar_t img[MAX_PATH * 2];
                DWORD n = MAX_PATH * 2;
                if (at == t.end() || at + 1 == t.end() || !QueryFullProcessImageNameW(h, 0, img, &n)) {
                    CloseHandle(h);
                    continue;
                }
                std::wstring dir = *(at + 1);
                if (!(dir.size() > 2 && dir[1] == L':')) dir = cwd + L"\\" + dir;
                wchar_t full[MAX_PATH * 2];
                DWORD fn = GetFullPathNameW(dir.c_str(), MAX_PATH * 2, full, nullptr);
                r.exe = img;
                r.dir = fn && fn < MAX_PATH * 2 ? full : dir;
            } else {
                for (size_t i = 1; i < t.size() && r.project.empty(); ++i)
                    if (t[i].size() > 3 && t[i][1] == L':' && t[i][2] == L'\\') r.project = tools::ProjectOfPath(t[i]);
                if (r.project.empty()) r.project = tools::ProjectOfPath(cwd);
                if (r.project.empty()) {
                    CloseHandle(h);
                    continue;
                }
            }
            skip.erase(p.pid);
            runs[p.pid] = r;
        }
        for (auto it = runs.begin(); it != runs.end();) {
            Run& r = it->second;
            DWORD code = 0;
            if (WaitForSingleObject(r.h, 0) != WAIT_OBJECT_0 || !GetExitCodeProcess(r.h, &code)) {
                ++it;
                continue;
            }
            if (code != 0 && !g_game) {
                if (r.cmake) {
                    AcquireSRWLockExclusive(&g_devLock);
                    g_buildFails.push_back({r.exe, r.dir});
                    ReleaseSRWLockExclusive(&g_devLock);
                } else if (now - r.t0 < 60000) {
                    CheckPort(it->first, r);
                }
            }
            CloseHandle(r.h);
            it = runs.erase(it);
        }
        if (skip.size() > 4000) {  // ушедшие - прочь, раз в много проходов
            std::map<DWORD, ULONGLONG> keep;
            for (const procsnap::Info& p : ps)
                if (skip.count(p.pid)) keep[p.pid] = skip[p.pid];
            skip.swap(keep);
        }
        fast = DevForeground();
    } while (WaitForSingleObject(g_devStop, fast ? 1000 : 5000) == WAIT_TIMEOUT);
    for (auto& [pid, r] : runs) CloseHandle(r.h);
    return 0;
}

std::wstring LeafOf(const std::wstring& p) { return p.empty() ? L"" : Leaf(p); }

}  // namespace

void DevrunStart() {
    if (g_devThread) return;
    g_devStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_devThread = CreateThread(nullptr, 0, DevLoop, nullptr, 0, nullptr);
}

void DevrunStop() {
    if (g_devStop) SetEvent(g_devStop);
    if (g_devThread) WaitForSingleObject(g_devThread, 4000), CloseHandle(g_devThread);
    g_devThread = nullptr;
}

void DevrunTick() {
    AcquireSRWLockExclusive(&g_devLock);
    std::vector<BuildFail> fails = std::move(g_buildFails);
    std::vector<PortBusy> busy = std::move(g_portBusy);
    g_buildFails.clear();
    g_portBusy.clear();
    ReleaseSRWLockExclusive(&g_devLock);
    std::vector<EnvAsk> envs = std::move(g_envAsk);
    g_envAsk.clear();
    for (const BuildFail& f : fails) BuildFailed(f.exe, f.dir);
    for (const EnvAsk& e : envs) {
        std::wstring what, use;
        for (const tools::EnvPick& p : e.picks) {
            what += (what.empty() ? L"" : L"; ") + p.tool + L" " + tools::VerText(p.need.v) +
                    (p.need.atLeast ? L"+" : L"") + L" (" + p.need.from + L"), а первым в PATH - " +
                    (p.first.Known() ? tools::VerText(p.first) : std::wstring(L"не нашёлся"));
            use += (use.empty() ? L"" : L", ") + p.tool + L" " + tools::VerText(p.use.v);
        }
        std::wstring proj = e.project;
        autos::Note(L"Окружение проекта " + LeafOf(proj) + L": " + what);
        if (g_busy) continue;
        Ask(
            L"Проекту «" + LeafOf(proj) + L"» нужна другая версия",
            what + L". Открыть терминал проекта, где первой будет " + use + L"?", L"Открыть терминал", L"Не надо",
            [proj] { tools::OpenEnvTerminal(tools::DetectEnv(proj)); }, nullptr, 40000, Toast::G_INFO);
    }
    for (const PortBusy& b : busy) {
        std::wstring who =
            NoExe(b.holderExe) + (b.holderProject.empty() ? L"" : L" из «" + LeafOf(b.holderProject) + L"»");
        bool same = !b.holderProject.empty() && !_wcsicmp(b.holderProject.c_str(), b.project.c_str());
        autos::Note(L"Порт " + std::to_wstring(b.port) + L" занят: " + who + L" - «" + LeafOf(b.project) +
                    L"» не запустился");
        if (g_busy) continue;
        DWORD pid = b.holder;
        std::wstring exe = b.holderExe;
        int port = b.port;
        Ask(
            L"Порт " + std::to_wstring(port) + L" занят",
            L"Его держит " + who + (b.holderMin >= 0 ? L" (работает " + std::to_wstring(b.holderMin) + L" мин)" : L"") +
                (same ? L" - похоже, прошлый запуск этого же проекта" : L"") + L", поэтому «" + LeafOf(b.project) +
                L"» не запустился. Остановить его?",
            L"Остановить", L"Оставить",
            [pid, exe, port] {
                bg::Run([pid, exe, port] {
                    bool ok = tools::KillPid(pid, exe);
                    autos::Note(L"Порт " + std::to_wstring(port) +
                                (ok ? L": держатель остановлен" : L": остановить не вышло"));
                });
                g_card.Show(BoostCard::DONE, L"Порт " + std::to_wstring(port) + L" освобождается",
                            L"Запустите сервер снова через пару секунд.", Theme(), 7000, Toast::G_OK);
            },
            nullptr, 40000, Toast::G_WARN);
    }
}

}  // namespace toolsui
