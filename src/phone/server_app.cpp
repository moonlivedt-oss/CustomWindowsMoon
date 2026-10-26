// «Телефон рядом», приложение «Компьютер рядом» на телефоне (android/): то, чего браузер не даёт. Уведомления, SMS и
// звонки телефона - карточкой на ПК (POST /notice); что нужно для Wake-on-LAN (GET /pcinfo: MAC и широковещательный
// адрес настоящих адаптеров - телефон запоминает их и будит ПК, когда сервер молчит). Буфер в фоне и автозагрузка
// новых фото идут старыми путями: /poll, /clip, PUT /up?kind=photo.
#include <winsock2.h>

#include <algorithm>

#include "core/text.h"
#include "phone/phone_internal.h"

namespace phone::detail {

namespace {

SRWLOCK g_gateLock = SRWLOCK_INIT;
NoticeGate g_gate;  // телефон шлёт всё подряд - ПК не заваливаем

std::string PcInfoJson() {
    wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(pc, &n);
    std::string j = "{\"pc\":" + phone::Json(pc) + ",\"v\":1,\"wol\":[";
    std::vector<std::string> seen;
    for (const Adapter& a : LocalAdapters()) {
        // будит только сетевая карта: провод (6) или Wi-Fi (71, будит редко - зависит от карты)
        // свой MAC (второй бит первого байта) - раздача с телефона по USB, случайный MAC Wi-Fi: будить нечего
        if (a.virt || a.mac.empty() || (a.type != 6 && a.type != 71) ||
            (strtoul(a.mac.substr(0, 2).c_str(), nullptr, 16) & 2))
            continue;
        if (std::find(seen.begin(), seen.end(), a.mac) != seen.end()) continue;
        j += std::string(seen.empty() ? "" : ",") + "{\"mac\":\"" + a.mac +
             "\",\"bc\":" + phone::Json(IpText(BroadcastOf(a.ip, a.prefix))) +
             ",\"wifi\":" + (a.type == 71 ? "true" : "false") + "}";
        seen.push_back(a.mac);
    }
    return j + "]}";
}

}  // namespace

bool HandleApp(Conn& cn, const Request& r, const std::string& rest) {
    if (rest == "/pcinfo" && (r.method == "GET" || r.method == "HEAD")) {
        Seen();
        JsonReply(cn, PcInfoJson());
        return true;
    }
    if (rest == "/notice" && r.method == "POST") {
        Seen();
        std::string body;
        if (!ReadAllBody(cn, r, body, 16 << 10)) {
            Text(cn, 413, "слишком длинно или оборвалось");
            return true;
        }
        Notice n;
        if (!ParseNotice(body, n)) {
            Text(cn, 400, "не понять");
            return true;
        }
        AcquireSRWLockExclusive(&g_gateLock);
        bool pass = n.kind == "end" || g_gate.Pass(n.key, GetTickCount64());  // конец звонка убирает карточку всегда
        ReleaseSRWLockExclusive(&g_gateLock);
        if (pass) PostNotice(n);
        JsonReply(cn, std::string("{\"ok\":true,\"shown\":") + (pass ? "true" : "false") + "}");
        return true;
    }
    return false;
}

}  // namespace phone::detail
