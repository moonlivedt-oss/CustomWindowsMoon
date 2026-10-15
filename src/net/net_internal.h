#pragma once
// Общее для файлов src/net: пробы, которые не нужны снаружи.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <string>
#include <vector>

#include "net/net.h"

namespace net {

void InitSockets();
int TcpConnectMs(const std::wstring& host, int port, int timeoutMs);
Proxy ReadProxy();
std::vector<Adapter> ReadAdapters();
bool HttpGet(const wchar_t* host, const wchar_t* path, int timeoutMs, std::string* body, int* status);

struct StaleRoute {
    std::wstring prefix, adapter;
    MIB_IPFORWARD_ROW2 row{};
};
std::vector<StaleRoute> StaleRoutes();  // маршруты на адаптерах, которые сейчас не подключены

// записи hosts, уводящие настоящие сайты на 0.0.0.0 или loopback; all - все нестандартные записи
std::vector<std::wstring> SuspiciousHosts(std::vector<std::wstring>* all = nullptr);

struct FwRule {
    std::wstring name;
    bool vpn = false;  // похоже на kill-switch VPN
};
std::vector<FwRule> FirewallBlocks(bool* readOk = nullptr);       // включённые исходящие блок-правила
int RemoveFirewallRules(const std::vector<std::wstring>& names);  // только с правами

// адаптер, через который идёт интернет мимо туннелей (для DNS, MTU, перезапуска)
bool PhysicalAdapter(Adapter& out);

// запустить системную утилиту из System32 без окна; вывод в out (OEM-кодировка)
int Exec(const std::wstring& exe, const std::wstring& args, std::wstring* out = nullptr, int timeoutMs = 60000);

std::wstring BackupDir();  // %APPDATA%\MoonDesk\net\backups

}  // namespace net
