// Проект одной кнопкой (Л20): VS Code с папкой проекта, терминал проекта (окружение из DetectEnv) с командой запуска
// (npm run dev, pnpm dev, python manage.py runserver, cargo run...), .uproject - в Unreal, браузер - когда порт
// проекта заработает (до 90 с). Сервер уже слушает этот порт - второй раз не запускаем, только браузер.
#include "app/toolsui_internal.h"

#include <shellapi.h>

#include "core/launch.h"
#include "tools/devserv.h"

namespace toolsui {

std::vector<std::wstring> g_launch;
std::vector<tools::Contest> g_contests;

namespace {

bool Has(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring ReadSmall(const std::wstring& path) {
    HANDLE f =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string s(256 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return Widen(s);
}

tools::LaunchIn In(const std::wstring& dir) {
    tools::LaunchIn in;
    in.packageJson = ReadSmall(dir + L"\\package.json");
    in.env = ReadSmall(dir + L"\\.env");
    in.pnpm = Has(dir + L"\\pnpm-lock.yaml");
    in.yarn = Has(dir + L"\\yarn.lock");
    in.bun = Has(dir + L"\\bun.lockb") || Has(dir + L"\\bun.lock");
    in.django = Has(dir + L"\\manage.py");
    in.cargo = Has(dir + L"\\Cargo.toml");
    in.go = Has(dir + L"\\go.mod");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.uproject").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        in.uproject = dir + L"\\" + fd.cFileName;
        FindClose(h);
    }
    return in;
}

bool Launchable(const std::wstring& dir) {
    return Has(dir + L"\\package.json") || Has(dir + L"\\manage.py") || Has(dir + L"\\Cargo.toml") ||
           Has(dir + L"\\go.mod") || tools::PlanLaunch(In(dir)).Any();
}

bool Listening(int port) {
    for (const tools::Listener& l : tools::Listeners())
        if (l.port == port && !l.udp) return true;
    return false;
}

}  // namespace

std::vector<std::wstring> LaunchableProjects() {
    std::vector<std::wstring> out;
    auto add = [&](const std::wstring& d) {
        for (const std::wstring& o : out)
            if (!_wcsicmp(o.c_str(), d.c_str())) return;
        if (out.size() < 24 && Launchable(d)) out.push_back(d);
    };
    // папки колец; у папки, что сама не проект, - её прямые подпапки (кольцо «Проекты» часто указывает на общую папку)
    std::function<void(const std::vector<Item>&)> walk = [&](const std::vector<Item>& items) {
        for (const Item& it : items) {
            if (it.dir && !it.full.empty()) {
                if (Launchable(it.full)) {
                    add(it.full);
                } else {
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW((it.full + L"\\*").c_str(), &fd);
                    int n = 0;
                    if (h != INVALID_HANDLE_VALUE) {
                        do {
                            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && ++n < 60)
                                add(it.full + L"\\" + fd.cFileName);
                        } while (FindNextFileW(h, &fd));
                        FindClose(h);
                    }
                }
            }
            walk(it.children);
        }
    };
    if (g_cfg) {
        walk(g_cfg->left);
        walk(g_cfg->right);
    }
    return out;
}

void LaunchProject(const std::wstring& dir) {
    tools::LaunchPlan p = tools::PlanLaunch(In(dir));
    ShellExecuteW(nullptr, nullptr, FindVsCode(g_cfg).c_str(), (L"\"" + dir + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    if (!p.uproject.empty()) ShellExecuteW(nullptr, L"open", p.uproject.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    bool running = p.port && Listening(p.port);  // уже запущен - не второй раз
    if (!p.cmd.empty() && !running) {
        tools::TermEnv env = tools::DetectEnv(dir);
        env.run = p.cmd;
        tools::OpenEnvTerminal(env);
    }
    std::wstring what = L"VS Code" + std::wstring(p.uproject.empty() ? L"" : L", Unreal") +
                        (p.cmd.empty() ? L""
                         : running     ? L", сервер уже работает"
                                       : L", " + p.cmd) +
                        (p.port ? L", браузер на :" + std::to_wstring(p.port) : L"");
    autos::Note(L"Запуск проекта " + Leaf(dir) + L": " + what);
    g_card.Show(BoostCard::DONE, L"Запускаю " + Leaf(dir), what + L".", Theme(), 6000, Toast::G_OK);
    if (!p.port) return;
    int port = p.port;
    bg::Run([port, running] {
        // ждём, пока сервер поднимет порт (npm install и сборка бывают долгими), потом - браузер
        for (int i = 0; i < 90 && !bg::Quitting(); ++i) {
            if (running || Listening(port)) {
                std::wstring url = L"http://localhost:" + std::to_wstring(port) + L"/";
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            Sleep(1000);
        }
    });
}

}  // namespace toolsui
