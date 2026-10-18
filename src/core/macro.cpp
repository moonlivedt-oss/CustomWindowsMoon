#include "core/macro.h"

#include <algorithm>

#include "core/bg.h"
#include "core/text.h"

namespace macro {

namespace {

struct KeyTok {
    UINT vk;
    const wchar_t* tok;
};
const KeyTok kKeys[] = {
    {VK_CONTROL, L"ctrl"},     {VK_SHIFT, L"shift"},
    {VK_MENU, L"alt"},         {VK_LWIN, L"win"},
    {VK_ESCAPE, L"esc"},       {VK_TAB, L"tab"},
    {VK_RETURN, L"enter"},     {VK_SPACE, L"space"},
    {VK_BACK, L"backspace"},   {VK_DELETE, L"del"},
    {VK_INSERT, L"ins"},       {VK_HOME, L"home"},
    {VK_END, L"end"},          {VK_PRIOR, L"pgup"},
    {VK_NEXT, L"pgdn"},        {VK_LEFT, L"left"},
    {VK_RIGHT, L"right"},      {VK_UP, L"up"},
    {VK_DOWN, L"down"},        {VK_SNAPSHOT, L"prtsc"},
    {VK_OEM_3, L"`"},          {VK_OEM_MINUS, L"-"},
    {VK_OEM_PLUS, L"="},       {VK_OEM_PLUS, L"plus"},
    {VK_OEM_PLUS, L"+"},       {VK_OEM_4, L"["},
    {VK_OEM_6, L"]"},          {VK_OEM_1, L";"},
    {VK_OEM_7, L"'"},          {VK_OEM_COMMA, L","},
    {VK_OEM_PERIOD, L"."},     {VK_OEM_2, L"/"},
    {VK_OEM_5, L"\\"},         {VK_MEDIA_PLAY_PAUSE, L"play"},
    {VK_VOLUME_MUTE, L"mute"}, {VK_APPS, L"menu"},
};

bool Modifier(WORD vk) { return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN; }

// клавиши, которым нужен флаг «расширенная» (иначе стрелки уходят как цифры цифрового блока)
bool Extended(WORD vk) {
    switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_INSERT:
        case VK_DELETE:
        case VK_LWIN:
        case VK_APPS:
            return true;
    }
    return false;
}

INPUT Key(WORD vk, bool up) {
    INPUT i{};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = vk;
    i.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (Extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    return i;
}

void SendKeys(const std::vector<WORD>& vk) {
    std::vector<INPUT> in;
    for (WORD k : vk) in.push_back(Key(k, false));
    for (auto it = vk.rbegin(); it != vk.rend(); ++it) in.push_back(Key(*it, true));
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void SendText(const std::wstring& s) {
    std::vector<INPUT> in;
    for (wchar_t ch : s) {
        if (ch == L'\n') {
            in.push_back(Key(VK_RETURN, false));
            in.push_back(Key(VK_RETURN, true));
            continue;
        }
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wScan = ch;
        i.ki.dwFlags = KEYEVENTF_UNICODE;
        in.push_back(i);
        i.ki.dwFlags |= KEYEVENTF_KEYUP;
        in.push_back(i);
    }
    if (!in.empty()) SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

std::vector<WORD> Combo(const std::wstring& s) {
    std::vector<WORD> mods, keys;
    std::wstring low = Lower(Trim(s));
    // «ctrl++» - плюс как клавиша: последний пустой кусок после «+»
    std::vector<std::wstring> parts;
    size_t from = 0;
    for (size_t i = 0; i <= low.size(); ++i) {
        if (i == low.size() || (low[i] == L'+' && i > from)) {
            parts.push_back(Trim(low.substr(from, i - from)));
            from = i + 1;
        }
    }
    for (const std::wstring& t : parts) {
        if (t.empty()) continue;
        UINT v = TokenVk(t);
        if (!v) return {};  // незнакомая клавиша - лучше ничего, чем не то
        (Modifier((WORD)v) ? mods : keys).push_back((WORD)v);
    }
    if (keys.empty() && mods.empty()) return {};
    mods.insert(mods.end(), keys.begin(), keys.end());
    return mods;
}

}  // namespace

bool Is(const std::wstring& p) { return p.rfind(L"keys:", 0) == 0 || p.rfind(L"macro:", 0) == 0; }

UINT TokenVk(const std::wstring& t) {
    for (auto& k : kKeys)
        if (t == k.tok) return k.vk;
    if (t == L"control") return VK_CONTROL;
    if (t == L"escape") return VK_ESCAPE;
    if (t == L"return") return VK_RETURN;
    if (t == L"delete") return VK_DELETE;
    if (t.size() == 1 && ((t[0] >= L'a' && t[0] <= L'z') || (t[0] >= L'0' && t[0] <= L'9'))) return towupper(t[0]);
    if (t.size() >= 2 && t[0] == L'f' && iswdigit(t[1])) {
        int n = _wtoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (t.size() > 2 && t.rfind(L"vk", 0) == 0) return (UINT)wcstoul(t.c_str() + 2, nullptr, 16);
    return 0;
}

std::wstring KeyName(UINT vk) {
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
    if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
    if (vk == VK_RWIN) vk = VK_LWIN;
    for (auto& k : kKeys)
        if (k.vk == vk) return k.tok;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)towlower(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"f" + std::to_wstring(vk - VK_F1 + 1);
    wchar_t b[16];
    swprintf(b, 16, L"vk%02X", vk);
    return b;
}

std::vector<Step> Parse(const std::wstring& path) {
    std::vector<Step> out;
    if (path.rfind(L"keys:", 0) == 0) {
        Step s;
        s.vk = Combo(path.substr(5));
        if (!s.vk.empty()) out.push_back(s);
        return out;
    }
    if (path.rfind(L"macro:", 0) != 0) return out;
    std::wstring body = path.substr(6);
    size_t from = 0;
    while (from <= body.size()) {
        size_t semi = body.find(L';', from);
        std::wstring part = body.substr(from, semi == std::wstring::npos ? std::wstring::npos : semi - from);
        from = semi == std::wstring::npos ? body.size() + 1 : semi + 1;
        std::wstring t = Trim(part);
        if (t.empty()) continue;
        std::wstring low = Lower(t);
        Step s;
        if (low.rfind(L"text ", 0) == 0 || low.rfind(L"text:", 0) == 0) {
            s.kind = Step::TEXT;
            // текст - как набран, с ведущим пробелом после «text» срезанным, остальные пробелы сохраняются
            size_t at = part.find_first_not_of(L" \t");
            s.text = part.substr(at + 5);
            if (s.text.empty()) continue;
        } else if (low.rfind(L"wait", 0) == 0) {
            s.kind = Step::WAIT;
            std::wstring n = Trim(low.substr(4));
            if (!n.empty() && (n[0] == L':' || n[0] == L' ')) n = Trim(n.substr(1));
            s.ms = std::clamp(_wtoi(n.c_str()), 0, 10000);
            if (!s.ms) continue;
        } else {
            s.vk = Combo(low);
            if (s.vk.empty()) return {};  // одна непонятная клавиша портит весь макрос: не шлём половину
        }
        out.push_back(s);
        if (out.size() >= 40) break;
    }
    return out;
}

std::wstring Describe(const std::wstring& path) {
    std::vector<Step> st = Parse(path);
    if (st.empty()) return L"не понял сочетание";
    auto combo = [](const Step& s) {
        std::wstring o;
        for (WORD v : s.vk) {
            std::wstring n = KeyName(v);
            if (n.size() > 1)
                n[0] = towupper(n[0]);
            else
                n = std::wstring(1, (wchar_t)towupper(n[0]));
            if (n == L"Pgup") n = L"PgUp";
            if (n == L"Pgdn") n = L"PgDn";
            o += (o.empty() ? L"" : L" + ") + n;
        }
        return o;
    };
    if (st.size() == 1 && st[0].kind == Step::KEYS) return combo(st[0]);
    std::wstring o = L"макрос: " + Plural((int)st.size(), L"шаг", L"шага", L"шагов");
    if (st[0].kind == Step::KEYS) o += L", сначала " + combo(st[0]);
    return o;
}

void Run(const std::wstring& path) {
    std::vector<Step> steps = Parse(path);
    if (steps.empty()) return;
    bg::Run([steps] {
        auto held = [] {
            for (int vk : {VK_MENU, VK_CONTROL, VK_SHIFT, VK_LWIN, VK_RWIN, VK_LBUTTON})
                if (GetAsyncKeyState(vk) & 0x8000) return true;
            return false;
        };
        for (int i = 0; i < 75 && held(); ++i) Sleep(20);
        Sleep(30);
        for (const Step& s : steps) {
            if (bg::Quitting()) return;
            if (s.kind == Step::KEYS)
                SendKeys(s.vk);
            else if (s.kind == Step::WAIT)
                Sleep(s.ms);
            else
                SendText(s.text);
            Sleep(25);  // программе - время на кадр между шагами (палитра команд открывается не мгновенно)
        }
    });
}

}  // namespace macro
