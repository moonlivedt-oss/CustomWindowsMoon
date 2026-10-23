// Забытые серверы разработки (Л19): решение по снимку - без окон и процессов, под тестами.
#include "tools/devserv.h"

#include <algorithm>
#include <cwchar>
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

namespace {

// явные порты в тексте команды: --port 3000, --port=3000, -p 3000, PORT=3000, host:8000 после runserver,
// число после http.server
void FlagPorts(const std::wstring& text, std::vector<int>& out) {
    std::wstring t = Low(text);
    auto add = [&](int p) {
        if (p >= 80 && p < 65536 && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    auto numAt = [&](size_t at) {
        while (at < t.size() && (t[at] == L' ' || t[at] == L'=' || t[at] == L'"' || t[at] == L'\'')) ++at;
        int n = 0, d = 0;
        while (at < t.size() && iswdigit(t[at]) && d < 6) n = n * 10 + (t[at++] - L'0'), ++d;
        return d ? n : 0;
    };
    for (const wchar_t* f : {L"--port", L"port=", L" -p ", L"http.server", L"--listen"})
        for (size_t at = t.find(f); at != std::wstring::npos; at = t.find(f, at + 1)) add(numAt(at + wcslen(f)));
    for (size_t at = t.find(L"runserver"); at != std::wstring::npos; at = t.find(L"runserver", at + 1)) {
        size_t end = t.find_first_of(L" \"'&|", at + 10);
        std::wstring arg = t.substr(at + 9, end == std::wstring::npos ? std::wstring::npos : end - at - 9);
        size_t colon = arg.rfind(L':');  // «0.0.0.0:8000» или просто «8000»
        int p = 0;
        for (wchar_t c : arg.substr(colon == std::wstring::npos ? 0 : colon + 1))
            if (iswdigit(c)) p = p * 10 + (c - L'0');
        add(p);
    }
}

// порт по умолчанию у знакомых запусков
void DefaultPorts(const std::wstring& text, std::vector<int>& out) {
    std::wstring t = Low(text);
    struct D {
        const wchar_t* word;
        int port;
    } kDefs[] = {{L"vite", 5173},          {L"svelte-kit", 5173},
                 {L"next", 3000},          {L"react-scripts", 3000},
                 {L"nuxt", 3000},          {L"remix", 3000},
                 {L"astro", 4321},         {L"ng serve", 4200},
                 {L"webpack serve", 8080}, {L"webpack-dev-server", 8080},
                 {L"gatsby", 8000},        {L"expo", 8081},
                 {L"runserver", 8000},     {L"flask", 5000},
                 {L"uvicorn", 8000},       {L"http.server", 8000},
                 {L"streamlit", 8501},     {L"jupyter", 8888},
                 {L"hugo server", 1313},   {L"parcel", 1234}};
    for (const D& d : kDefs)
        if (t.find(d.word) != std::wstring::npos && std::find(out.begin(), out.end(), d.port) == out.end())
            out.push_back(d.port);
}

// текст скрипта "имя": "..." из package.json (без разбора JSON целиком: скрипты - простые строки)
std::wstring Script(const std::wstring& json, const wchar_t* name) {
    std::wstring key = std::wstring(L"\"") + name + L"\"";
    size_t scripts = json.find(L"\"scripts\"");
    if (scripts == std::wstring::npos) return L"";
    size_t at = json.find(key, scripts);
    size_t close = json.find(L'}', scripts);
    if (at == std::wstring::npos || (close != std::wstring::npos && at > close)) return L"";
    size_t q1 = json.find(L'"', json.find(L':', at + key.size()) + 1);
    if (q1 == std::wstring::npos) return L"";
    size_t q2 = q1 + 1;
    while (q2 < json.size() && (json[q2] != L'"' || json[q2 - 1] == L'\\')) ++q2;
    return json.substr(q1 + 1, q2 - q1 - 1);
}

}  // namespace

std::vector<int> ProjectPorts(const std::wstring& cmdline, const std::wstring& packageJson, const std::wstring& env) {
    std::vector<int> out;
    std::wstring scripts;
    for (const wchar_t* s : {L"dev", L"start", L"serve", L"develop"}) scripts += Script(packageJson, s) + L" ";
    FlagPorts(L" " + cmdline + L" ", out);
    FlagPorts(L" " + scripts + L" ", out);
    // PORT=3000 в .env (строка целиком)
    std::wstring e = L"\n" + env;
    for (size_t at = e.find(L"\nPORT"); at != std::wstring::npos; at = e.find(L"\nPORT", at + 1)) {
        size_t p = at + 5;
        while (p < e.size() && (e[p] == L' ' || e[p] == L'=')) ++p;
        int n = 0, d = 0;
        while (p < e.size() && iswdigit(e[p]) && d < 6) n = n * 10 + (e[p++] - L'0'), ++d;
        if (n >= 80 && n < 65536 && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    DefaultPorts(cmdline + L" " + scripts, out);
    return out;
}

LaunchPlan PlanLaunch(const LaunchIn& in) {
    LaunchPlan p;
    p.uproject = in.uproject;
    if (!in.packageJson.empty()) {
        for (const wchar_t* s : {L"dev", L"start", L"serve", L"develop"}) {
            if (Script(in.packageJson, s).empty()) continue;
            std::wstring name = s;
            if (in.pnpm)
                p.cmd = L"pnpm " + name;
            else if (in.yarn)
                p.cmd = L"yarn " + name;
            else if (in.bun)
                p.cmd = L"bun run " + name;
            else
                p.cmd = name == L"start" ? L"npm start" : L"npm run " + name;
            std::vector<int> ports = ProjectPorts(L"", in.packageJson, in.env);
            if (!ports.empty()) p.port = ports[0];
            return p;
        }
    }
    if (in.django) {
        p.cmd = L"python manage.py runserver";
        std::vector<int> ports = ProjectPorts(p.cmd, L"", in.env);
        p.port = ports.empty() ? 8000 : ports[0];
    } else if (in.cargo) {
        p.cmd = L"cargo run";
    } else if (in.go) {
        p.cmd = L"go run .";
    }
    return p;
}

}  // namespace tools
