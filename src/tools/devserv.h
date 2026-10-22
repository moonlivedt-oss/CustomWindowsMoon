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

}  // namespace tools
