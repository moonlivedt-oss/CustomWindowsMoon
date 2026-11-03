#include "core/usage.h"
// «Телефон рядом»: пульт, тачпад и клавиатура с телефона, «открыть на ПК», картинки буфера, запомнить телефон.
#include <winsock2.h>

#include <algorithm>

#include "core/text.h"
#include "phone/phone_internal.h"
#include "tools/tools.h"

namespace phone::detail {

namespace {

std::string Q(const Request& r, const char* k) {
    auto it = r.query.find(k);
    return it == r.query.end() ? std::string() : it->second;
}

void Mouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.mouseData = data;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

void Key(WORD vk) {
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void Type(const std::wstring& t) {  // символами Юникода: не зависит от раскладки
    std::vector<INPUT> in;
    for (wchar_t c : t) {
        if (c == L'\r') continue;
        if (c == L'\n') {
            Key(VK_RETURN);
            continue;
        }
        INPUT d{}, u{};
        d.type = u.type = INPUT_KEYBOARD;
        d.ki.wScan = u.ki.wScan = c;
        d.ki.dwFlags = KEYEVENTF_UNICODE;
        u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        in.push_back(d);
        in.push_back(u);
        if (in.size() >= 64) {
            SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
            in.clear();
        }
    }
    if (!in.empty()) SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

}  // namespace

// клавиши геймпада: скан-кодами (игры на DirectInput не видят виртуальные коды); зажатые - помним, чтобы отпустить
SRWLOCK g_heldLock = SRWLOCK_INIT;
std::vector<WORD> g_held;

void HoldKey(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    switch (vk) {  // стрелки и их соседи - «расширенные» клавиши
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_HOME:
        case VK_END:
        case VK_INSERT:
        case VK_DELETE:
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    AcquireSRWLockExclusive(&g_heldLock);
    auto it = std::find(g_held.begin(), g_held.end(), vk);
    if (down && it == g_held.end()) g_held.push_back(vk);
    if (!down && it != g_held.end()) g_held.erase(it);
    ReleaseSRWLockExclusive(&g_heldLock);
    SendInput(1, &in, sizeof(in));
}

void ReleaseKeys() {
    AcquireSRWLockExclusive(&g_heldLock);
    std::vector<WORD> held = g_held;
    ReleaseSRWLockExclusive(&g_heldLock);
    for (WORD vk : held) HoldKey(vk, false);
}

namespace {

bool Input(const Request& r) {
    std::string t = Q(r, "t");
    if (t == "m") {
        Mouse(MOUSEEVENTF_MOVE, atoi(Q(r, "x").c_str()), atoi(Q(r, "y").c_str()));
    } else if (t == "c") {
        std::string b = Q(r, "b");
        if (b == "r") {
            Mouse(MOUSEEVENTF_RIGHTDOWN);
            Mouse(MOUSEEVENTF_RIGHTUP);
        } else if (b == "m") {
            Mouse(MOUSEEVENTF_MIDDLEDOWN);
            Mouse(MOUSEEVENTF_MIDDLEUP);
        } else {
            for (int i = 0; i < (b == "d" ? 2 : 1); ++i) {
                Mouse(MOUSEEVENTF_LEFTDOWN);
                Mouse(MOUSEEVENTF_LEFTUP);
            }
        }
    } else if (t == "dn") {
        Mouse(MOUSEEVENTF_LEFTDOWN);
    } else if (t == "up") {
        Mouse(MOUSEEVENTF_LEFTUP);
    } else if (t == "w" || t == "h") {
        Mouse(t == "w" ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)atoi(Q(r, "d").c_str()));
    } else if (t == "k") {
        if (WORD vk = KeyVk(Q(r, "k"))) Key(vk);
    } else if (t == "kd" || t == "ku") {  // геймпад: держать клавишу, пока палец на кнопке
        WORD vk = KeyVk(Q(r, "k"));
        if (!vk) return false;
        HoldKey(vk, t == "kd");
    } else if (t == "ka") {  // отпустить всё (страница ушла в фон)
        ReleaseKeys();
    } else {
        return false;
    }
    return true;
}

}  // namespace

bool HandleMore(Conn& cn, const Request& r, const std::string& rest) {
    bool post = r.method == "POST";
    if (rest == "/cmd" && post) {
        Seen();
        float v = Command(Q(r, "c"));
        char j[64];
        snprintf(j, sizeof(j), "{\"vol\":%.3f}", v);
        JsonReply(cn, j);
        return true;
    }
    if ((rest == "/in" || rest == "/type") && post) {
        {
            Lock l;
            S().seen = GetTickCount64();
        }
        if (!InputAllowed()) {
            Text(cn, 403, "тачпад выключен (phone_input)");
            return true;
        }
        if (rest == "/type") {
            std::string body;
            if (!ReadAllBody(cn, r, body, 64 << 10)) {
                Text(cn, 413, "слишком длинно");
                return true;
            }
            Type(Widen(body));
        } else if (!Input(r)) {
            Text(cn, 400, "не понял");
            return true;
        }
        usage::Hit(L"phone_input");
        Text(cn, 200, "ok");
        return true;
    }
    if (rest == "/open" && post) {  // ссылка с телефона - в браузер ПК
        Seen();
        std::string body;
        std::wstring url = ReadAllBody(cn, r, body, 8 << 10) ? tools::SafeUrl(Trim(Widen(body))) : L"";
        if (url.empty()) {
            Text(cn, 400, "только ссылки http и https");
            return true;
        }
        Post(EV_OPEN, url);
        Text(cn, 200, "ok");
        return true;
    }
    if (rest == "/clipimg" && (r.method == "GET" || r.method == "HEAD")) {  // картинка из буфера ПК
        Seen();
        std::string png;
        {
            Lock l;
            png = S().clipPng;
        }
        if (png.empty())
            Text(cn, 404, "в буфере нет картинки");
        else
            Reply(cn, 200, "image/png", png, "Content-Disposition: inline; filename=\"clipboard.png\"\r\n");
        return true;
    }
    if (rest == "/clipimg" && post) {  // картинка с телефона - в буфер ПК
        Seen();
        std::string body;
        if (!ReadAllBody(cn, r, body, 30 << 20)) {
            Text(cn, 413, "больше 30 МБ");
            return true;
        }
        PostBytes(EV_CLIPIMG, std::move(body));
        Text(cn, 200, "ok");
        return true;
    }
    if (rest == "/pair" && post) {  // запомнить этот телефон: постоянная ссылка без QR
        Seen();
        if (!cn.tls) {  // постоянный ключ по http увидел бы любой в этой Wi-Fi (https не открылся - не запоминаем
                        // вовсе)
            Text(cn, 403, "запомнить можно только на защищённой странице (https)");
            return true;
        }
        std::string body;
        ReadAllBody(cn, r, body, 256);
        std::wstring name = Trim(Widen(body)).substr(0, 60);
        // Постоянный ключ - только после «Запомнить» на ПК: ключ сессии из QR ходит по http, и подсмотревший его в
        // этой Wi-Fi иначе выдал бы себе вечный пропуск. Ответа ждём до минуты, пока карточка на экране
        {
            Lock l;
            ULONGLONG now = GetTickCount64();
            if (S().pairState == 1 && now - S().pairAt < 65000) {
                Text(cn, 409, "на ПК уже спрашивают про другой телефон - ответьте там");
                return true;
            }
            S().pairState = 1;
            S().pairAt = now;
        }
        Post(EV_PAIR_ASK, name.empty() ? L"телефон" : name);
        int st = 1;
        for (int i = 0; i < 600 && st == 1 && !Stopping(); ++i) {
            Sleep(100);
            Lock l;
            st = S().pairState;
        }
        {
            Lock l;
            S().pairState = 0;
        }
        if (st != 2) {
            Text(cn, 403,
                 st == 3 ? "на ПК запретили запоминать этот телефон" : "на ПК не ответили - попробуйте ещё раз");
            return true;
        }
        std::string key = NewDevice(name.empty() ? L"телефон" : name);
        if (key.empty()) {
            Text(cn, 500, "не записать");
            return true;
        }
        Post(EV_PAIRED, name);
        JsonReply(cn, "{\"url\":\"/d/" + key + "/\"}");
        return true;
    }
    return false;
}

}  // namespace phone::detail
