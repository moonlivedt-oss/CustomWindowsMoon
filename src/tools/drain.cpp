// Что посадило батарею (Л38): группы программ и итог - без окон и драйвера, под тестами.
#include "tools/drain.h"

#include <algorithm>
#include <cwctype>

namespace tools {

namespace {

std::wstring Low(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Duration(int minutes) {
    if (minutes < 60) return std::to_wstring(minutes) + L" мин";
    return std::to_wstring(minutes / 60) + L" ч" +
           (minutes % 60 ? L" " + std::to_wstring(minutes % 60) + L" мин" : L"");
}

}  // namespace

std::wstring DrainGroup(const std::wstring& exe) {
    std::wstring n = Low(exe);
    if (n.size() > 4 && n.compare(n.size() - 4, 4, L".exe") == 0) n.resize(n.size() - 4);
    struct G {
        const wchar_t* name;
        const wchar_t* group;
    } kGroups[] = {{L"chrome", L"браузер"},
                   {L"msedge", L"браузер"},
                   {L"firefox", L"браузер"},
                   {L"opera", L"браузер"},
                   {L"brave", L"браузер"},
                   {L"browser", L"браузер"},
                   {L"vivaldi", L"браузер"},
                   {L"unrealeditor", L"Unreal"},
                   {L"shadercompileworker", L"Unreal"},
                   {L"unrealbuildtool", L"Unreal"},
                   {L"ubahost", L"Unreal"},
                   {L"javaw", L"Minecraft"},
                   {L"code", L"VS Code"},
                   {L"discord", L"Discord"},
                   {L"telegram", L"Telegram"},
                   {L"wallpaper64", L"Wallpaper Engine"},
                   {L"wallpaper32", L"Wallpaper Engine"},
                   {L"msedgewebview2", L"встроенный браузер"},
                   {L"blender", L"Blender"},
                   {L"obs64", L"OBS"},
                   {L"steam", L"Steam"},
                   {L"steamwebhelper", L"Steam"}};
    for (const G& g : kGroups)
        if (n == g.name) return g.group;
    return exe.size() > 4 && Low(exe.substr(exe.size() - 4)) == L".exe" ? exe.substr(0, exe.size() - 4) : exe;
}

bool DrainSummary(const DrainIn& in, DrainOut& out) {
    out = {};
    int drop = in.fromPct >= 0 && in.toPct >= 0 ? in.fromPct - in.toPct : -1;
    if (in.minutes < 20 || drop < 5) return false;
    std::map<std::wstring, ULONGLONG> by;
    ULONGLONG total = 0;
    for (const auto& [exe, t] : in.cpu) {
        by[DrainGroup(exe)] += t;
        total += t;
    }
    std::vector<std::pair<std::wstring, ULONGLONG>> v(by.begin(), by.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [g, t] : v) {
        int share = total ? (int)(t * 100 / total) : 0;
        if (share < 5 || out.top.size() >= 3) break;
        out.top.push_back({g, share});
    }
    out.title = L"От батареи " + Duration(in.minutes) + L": ушло " + std::to_wstring(drop) + L"%";
    if (out.top.empty()) {
        out.text = L"Ни одна программа заметно не выделялась - скорее экран и видеокарта.";
    } else {
        std::wstring list;
        for (size_t i = 0; i < out.top.size(); ++i)
            list += (i ? (i + 1 == out.top.size() ? L" и " : L", ") : L"") + out.top[i].first + L" (" +
                    std::to_wstring(out.top[i].second) + L"%)";
        out.text = L"Больше всего процессора: " + list + L".";
    }
    int perHour = in.minutes ? drop * 60 / in.minutes : 0;
    out.text += L" Расход " + std::to_wstring(perHour) + L"% в час.";
    return true;
}

}  // namespace tools
