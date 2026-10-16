#pragma once
// Сеть: что сломалось и как починить. Наследник NetworkDoctor, но без Python и PowerShell -
// всё на функциях Windows (WinHTTP, IP Helper, ICMP, COM брандмауэра), поэтому быстро и тихо.
// Всё здесь блокирующее: зовётся из рабочих потоков, не из потока окон.
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

namespace net {

enum Level { OK, INFO, WARN, BAD };

// одна строка диагностики: что проверяли и что нашли
struct Check {
    Level level = OK;
    std::wstring title, detail;
};

// что лучше всего сделать по итогам проверки
enum Fix { FIX_NONE, FIX_SMART, FIX_PROXY, FIX_VPN, FIX_IP, FIX_EXIT };

struct Proxy {
    bool on = false;                // WinINET: ProxyEnable и адрес
    std::wstring server;            // как записано: 127.0.0.1:10808 или http=...;https=...
    std::wstring pac;               // AutoConfigURL
    std::wstring machine;           // прокси уровня машины (HKLM)
    std::wstring winhttp;           // WinHTTP-прокси, пусто - прямой доступ
    std::vector<std::wstring> env;  // HTTP_PROXY=... и т.п.
    bool alive = false;             // по адресу прокси кто-то слушает
    bool local = false;             // прокси на этом компьютере (клиент вроде Happ)
};

struct Adapter {
    std::wstring name, desc;
    bool up = false, vpn = false;
    ULONG mtu = 0, index = 0;
    bool gateway = false;
};

struct Report {
    std::vector<Check> checks;
    Fix fix = FIX_NONE;
    std::wstring fixLabel, fixReason;
    bool tcp = false, dns = false, https = false;  // сырой TCP, имена, HTTPS через системные настройки
    Proxy proxy;
    std::vector<Adapter> adapters;
    std::wstring route;              // через какой адаптер идёт интернет (маршрут до 1.1.1.1)
    bool viaTunnel = false;          // и это туннель (Happ, WireGuard...)
    std::wstring ip, country, colo;  // выходной узел
    int pingMs = -1, lossPct = 0;    // до 1.1.1.1
    bool Online() const { return https || (tcp && dns && !proxy.on); }
};

// Полная проверка. remnants - ещё и остатки VPN (брандмауэр, маршруты, hosts): это дольше.
Report Diagnose(bool remnants);
// Короткая строка состояния для кольца и трея: «Интернет есть | Германия | 48 мс | туннель happ-tun».
std::wstring Summary(const Report& r);

// --- отдельные пробы ---
Proxy ReadProxy();  // системный прокси сейчас и жив ли он (быстро: реестр и одно соединение с localhost)
bool HttpsOk(int budgetMs = 6000);  // HTTPS так, как ходят браузеры (системный прокси учитывается)
struct Exit {
    bool ok = false;
    std::wstring ip, country, colo;
};
Exit ExitNode(int timeoutMs = 5000);                 // cloudflare.com/cdn-cgi/trace: ip, страна, узел CF
std::wstring CountryName(const std::wstring& code);  // DE -> «Германия» на языке Windows
int Ping(const std::wstring& host, int count, int timeoutMs, int* lossPct = nullptr);  // ICMP, мс; -1 - нет ответа
int TcpRtt(const std::wstring& ip, int port, int timeoutMs);  // время установки TCP, мс; -1 - нет
// Настоящая задержка через туннель: TUN-режим (Happ, sing-box) сам отвечает на пинг и DNS за 0 мс,
// поэтому меряем повтор HTTPS-запроса по уже открытому соединению - это примерно один круг до выхода и обратно.
int HttpRtt(int timeoutMs = 5000);
inline bool Faked(int ms) { return ms >= 0 && ms <= 2; }  // ответ «за 0 мс» через туннель - подделка TUN
// через какой адаптер Windows отправит пакет до этого адреса; true - это туннель
bool RouteTo(const std::wstring& ip, std::wstring* adapter);
bool VpnName(const std::wstring& nameOrDesc);  // похоже на VPN или туннель

struct ProxyProc {
    DWORD pid = 0;
    std::wstring name;
    std::vector<int> ports;  // на каких портах слушает
    bool byName = false;     // распознан по имени (Happ, sing-box...)
};
std::vector<ProxyProc> ProxyProcesses();
bool Kill(DWORD pid);

struct DnsResult {
    std::wstring name, ip;
    int ms = -1;
};
std::vector<DnsResult> TestDns();  // публичные DNS, сырым UDP-запросом; быстрые первыми

// --- починка ---
using Log = std::function<void(const std::wstring&)>;
void ResetProxy(const Log& log);  // системный прокси, PAC/WPAD, переменные окружения, кэш DNS (без прав)
void FlushDns(const Log& log);

// Действия с правами администратора идут через задачу планировщика MoonRadial\RadialMenuNet:
// один раз согласие UAC, дальше без вопросов. Задача выполняет только эти действия.
enum Job {
    JOB_SMART,       // шаг «Починить умно»: WinHTTP-прокси и зависшие маршруты
    JOB_WINHTTP,     // сбросить WinHTTP-прокси
    JOB_ROUTES,      // удалить маршруты отключённых адаптеров
    JOB_IP,          // обновить IP физического адаптера
    JOB_VPN,         // остатки VPN: kill-switch (с бэкапом), маршруты, MTU, ARP, hosts (с бэкапом)
    JOB_DNS_CF,      // DNS 1.1.1.1 / 1.0.0.1
    JOB_DNS_GOOGLE,  // DNS 8.8.8.8 / 8.8.4.4
    JOB_DNS_AUTO,    // DNS от роутера (DHCP)
    JOB_FW_RESTORE,  // вернуть брандмауэр из последнего бэкапа
    JOB_ADAPTER,     // перезапустить физический адаптер
    JOB_DEEP,
    JOB_DISK_HEALTH,    // утилиты: прочитать износ и температуру дисков (только чтение)
    JOB_SYSPATH_CLEAN,  // утилиты: убрать из системного PATH мёртвые папки и дубли (ничего не добавляет)
    JOB_NET_TOP,        // утилиты: трафик TCP по процессам за 3 секунды (только чтение)
    JOB_POWER,          // утилиты: что держит компьютер без сна и что его будит (только чтение)
    JOB_RESTORE_POINT,  // утилиты: создать точку восстановления системы
    kJobs
};
const wchar_t* JobKey(Job j);
bool AdminReady();  // задача есть
bool SetupAdmin();  // создать задачу (Windows спросит разрешение)
bool RunAdmin(Job j, const Log& log, int timeoutMs = 90000);
int AdminMain();  // RadialMenu.exe --net-job: выполнить задание, уже с правами

// «Починить умно»: шаги по порядку, пока интернет не вернётся. true - работает.
bool SmartFix(const Log& log);

// --- история: обрывы, починки, смены выхода (%APPDATA%\MoonDesk\net-history.txt) ---
void Remember(const std::wstring& kind, const std::wstring& text);
std::vector<std::pair<std::wstring, std::wstring>> History(int max);  // время, текст; новые первыми

std::wstring DataDir();  // %APPDATA%\MoonDesk\net

}  // namespace net
