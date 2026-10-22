// Забытые серверы разработки (Л19): решение по снимку - без окон и процессов, под тестами.
#include "tools/devserv.h"

#include <cwctype>

namespace tools {

namespace {

std::wstring Low(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring LeafOf(const std::wstring& p) {
    std::wstring s = p;
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    size_t cut = s.find_last_of(L"\\/");
    return cut == std::wstring::npos ? s : s.substr(cut + 1);
}

bool WordChar(wchar_t c) { return iswalnum(c) || c == L'_' || c == L'-' || c == L'.'; }

}  // namespace

bool DevServerName(const std::wstring& exe) {
    std::wstring n = Low(exe);
    if (n.size() > 4 && n.compare(n.size() - 4, 4, L".exe") == 0) n.resize(n.size() - 4);
    static const wchar_t* kNames[] = {L"node", L"python", L"pythonw", L"py",   L"deno",   L"bun",     L"php",
                                      L"ruby", L"dotnet", L"java",    L"go",   L"cargo",  L"uvicorn", L"flask",
                                      L"hugo", L"caddy",  L"nginx",   L"vite", L"esbuild"};
    for (const wchar_t* k : kNames)
        if (n == k) return true;
    return n.rfind(L"python3", 0) == 0;  // python3.11 и подобные
}

bool ProjectInTitle(const std::wstring& project, const std::wstring& title) {
    std::wstring leaf = Low(LeafOf(project)), t = Low(title);
    if (leaf.size() < 2) return false;
    for (size_t at = t.find(leaf); at != std::wstring::npos; at = t.find(leaf, at + 1)) {
        bool left = at == 0 || !WordChar(t[at - 1]);
        size_t end = at + leaf.size();
        bool right = end >= t.size() || !WordChar(t[end]);
        if (left && right) return true;
    }
    return false;
}

std::vector<Forgotten> ForgottenServers(const std::vector<DevServer>& servers, const std::vector<std::wstring>& titles,
                                        std::map<std::wstring, ULONGLONG>& lastSeen, ULONGLONG now, ULONGLONG idleMs,
                                        ULONGLONG minBytes) {
    std::map<std::wstring, Forgotten> by;  // ключ - проект в нижнем регистре
    for (const DevServer& s : servers) {
        if (s.project.empty()) continue;
        Forgotten& f = by[Low(s.project)];
        f.project = s.project;
        // один процесс может слушать несколько портов - память считаем один раз
        bool again = false;
        for (const DevServer& o : f.servers) again = again || o.pid == s.pid;
        if (!again) f.bytes += s.bytes;
        f.servers.push_back(s);
    }
    std::vector<Forgotten> out;
    for (auto& [key, f] : by) {
        bool seen = false;
        for (const std::wstring& t : titles) seen = seen || ProjectInTitle(f.project, t);
        auto it = lastSeen.find(key);
        if (seen || it == lastSeen.end()) {
            lastSeen[key] = now;
            continue;
        }
        ULONGLONG gone = now > it->second ? now - it->second : 0;
        if (gone < idleMs || f.bytes < minBytes) continue;
        f.minutes = (int)(gone / 60000);
        out.push_back(f);
    }
    return out;
}

}  // namespace tools
