#pragma once
// Забытые серверы разработки (Л19): node, python, vite и подобные из проекта, которого больше часа не видно ни в
// одном окне (VS Code и терминал пишут папку в заголовке), ещё работают и едят память - тихо предложить завершить.
// Здесь - только решение (под тестами); снимок процессов, портов и заголовков окон собирает слежка toolsui.
#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace tools {

struct DevServer {
    DWORD pid = 0;
    std::wstring process;  // node.exe
    std::wstring project;  // папка проекта (по командной строке)
    int port = 0;          // 0 - не слушает
    ULONGLONG bytes = 0;   // личная память вместе с дочерними
};

struct Forgotten {
    std::wstring project;
    std::vector<DevServer> servers;  // все серверы этого проекта
    ULONGLONG bytes = 0;
    int minutes = 0;  // сколько проекта не видно в окнах
};

// Процесс похож на сервер разработки по имени (node, python, deno, bun, php, ruby, dotnet, java...)
bool DevServerName(const std::wstring& exe);
// Проект виден в окне: имя его папки целым словом в заголовке (без учёта регистра)
bool ProjectInTitle(const std::wstring& project, const std::wstring& title);
// lastSeen - когда проект последний раз был виден (мс GetTickCount64): обновляется здесь; проект, увиденный
// впервые, считается видимым сейчас. Возвращает проекты, которых не видно idleMs, а серверы едят от minBytes.
std::vector<Forgotten> ForgottenServers(const std::vector<DevServer>& servers, const std::vector<std::wstring>& titles,
                                        std::map<std::wstring, ULONGLONG>& lastSeen, ULONGLONG now,
                                        ULONGLONG idleMs = 60 * 60 * 1000, ULONGLONG minBytes = 100ull << 20);

// Порт занят (Л18): на каких портах сервер проекта собирался слушать - из командной строки (--port 3000, -p 8080,
// PORT=, http.server 8000, runserver 0.0.0.0:8000), скриптов package.json (dev, start, serve: флаги и порты по
// умолчанию vite 5173, next 3000, ng 4200...), PORT= из .env и обычных портов Python (Django 8000, Flask 5000).
// Первым - самое точное (явный флаг).
std::vector<int> ProjectPorts(const std::wstring& cmdline, const std::wstring& packageJson, const std::wstring& env);

// Проект одной кнопкой (Л20): чем запускать. Файлы проекта - их тексты или признак «есть».
struct LaunchIn {
    std::wstring packageJson, env;
    bool pnpm = false, yarn = false, bun = false;  // замок менеджера пакетов рядом
    bool django = false;                           // manage.py
    bool cargo = false, go = false;
    std::wstring uproject;  // имя .uproject, если есть
};
struct LaunchPlan {
    std::wstring cmd;       // «npm run dev», «python manage.py runserver»; пусто - сервер не запускаем
    int port = 0;           // куда открыть браузер (0 - не открывать)
    std::wstring uproject;  // открыть в Unreal
    bool Any() const { return !cmd.empty() || !uproject.empty(); }
};
LaunchPlan PlanLaunch(const LaunchIn& in);

}  // namespace tools
