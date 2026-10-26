// «Телефон рядом»: пульт, тачпад и клавиатура с телефона, «открыть на ПК», картинки буфера, запомнить телефон.
#include <winsock2.h>

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
        static const struct {
            const char* name;
            WORD vk;
        } kKeys[] = {{"enter", VK_RETURN}, {"back", VK_BACK},   {"esc", VK_ESCAPE}, {"tab", VK_TAB},
                     {"left", VK_LEFT},    {"right", VK_RIGHT}, {"up", VK_UP},      {"down", VK_DOWN},
                     {"space", VK_SPACE},  {"del", VK_DELETE},  {"home", VK_HOME},  {"end", VK_END},
                     {"win", VK_LWIN},     {"f5", VK_F5},       {"f11", VK_F11},    {"pgup", VK_PRIOR},
                     {"pgdn", VK_NEXT}};
        std::string k = Q(r, "k");
        for (const auto& e : kKeys)
            if (k == e.name) Key(e.vk);
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
        std::string body;
        ReadAllBody(cn, r, body, 256);
        std::wstring name = Trim(Widen(body)).substr(0, 60);
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
