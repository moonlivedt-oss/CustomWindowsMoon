// Служебные режимы: сеть и скорость без окна (net_out.txt, speed_out.txt).
#include <windows.h>

#include <string>

#include "app/devcli_internal.h"
#include "core/files.h"
#include "net/net.h"
#include "net/speed.h"

namespace devcli {

// Проверка сети без окна: отчёт в net_out.txt (то же, что раздел «Сеть» в студии)
int NetDiag() {
    net::Report r = net::Diagnose(true);
    static const wchar_t* kLevel[] = {L"ok  ", L"info", L"WARN", L"BAD "};
    const std::wstring nl = L"\r\n";
    std::wstring s = net::Summary(r) + nl + nl;
    for (const net::Check& c : r.checks) s += std::wstring(kLevel[c.level]) + L"  " + c.title + L": " + c.detail + nl;
    s += nl + L"Совет: " + (r.fixLabel.empty() ? std::wstring(L"нет") : r.fixLabel + L" - " + r.fixReason) + nl;
    s += nl + L"Прокси-процессы:" + nl;
    for (const net::ProxyProc& p : net::ProxyProcesses()) {
        s += L"  " + p.name + L" pid " + std::to_wstring(p.pid) + L" порты";
        for (int port : p.ports) s += L" " + std::to_wstring(port);
        s += nl;
    }
    s += nl + L"DNS:" + nl;
    for (const net::DnsResult& d : net::TestDns())
        s += L"  " + d.name + L" " + d.ip + L": " + std::to_wstring(d.ms) + L" мс" + nl;
    s += nl + L"Задача с правами: " + std::wstring(net::AdminReady() ? L"есть" : L"нет") + nl;
    WriteOut(L"net_out.txt", s);
    return 0;
}

// Скорость (И26) без окна: замер в speed_out.txt; в историю не пишется
int SpeedDiag() {
    std::wstring s;
    const std::wstring nl = L"\r\n";
    net::Speed r = net::MeasureSpeed([&](const std::wstring& step) { s += step + nl; });
    s += net::SpeedText(r) + nl + net::SpeedCompare(net::SpeedHistory()) + nl;
    WriteOut(L"speed_out.txt", s);
    return r.down > 0 ? 0 : 1;
}

}  // namespace devcli
