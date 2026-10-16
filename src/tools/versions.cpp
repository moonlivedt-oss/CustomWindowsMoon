// Версии инструментов раз в неделю: что найдётся первым в PATH (python, node, cmake, git) против вышедшего
// (endoflife.date, релизы GitHub), пакеты MSYS2 - через checkupdates (базы пакетов не трогает). Ничего не ставит.
#include <windows.h>

#include <algorithm>

#include "core/text.h"
#include "net/net_internal.h"
#include "tools/tools.h"

namespace tools {

namespace {

// первое «число.число[.число]» в тексте
std::wstring FirstVersion(const std::wstring& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        // «v22.11.0» у node: буква v перед числом допустима
        if (!iswdigit(s[i]) || (i && ((iswalnum(s[i - 1]) && s[i - 1] != L'v') || s[i - 1] == L'.'))) continue;
        size_t j = i, dots = 0;
        while (j < s.size() && (iswdigit(s[j]) || (s[j] == L'.' && j + 1 < s.size() && iswdigit(s[j + 1]))))
            dots += s[j++] == L'.';
        if (dots >= 1) return s.substr(i, j - i);
    }
    return L"";
}

std::vector<int> Parts(const std::wstring& v) {
    std::vector<int> p;
    for (const std::wstring& x : Split(v, L'.')) p.push_back(_wtoi(x.c_str()));
    return p;
}

int Compare(const std::wstring& a, const std::wstring& b) {
    std::vector<int> x = Parts(a), y = Parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

std::wstring MajorMinor(const std::wstring& v) {
    std::vector<std::wstring> p = Split(v, L'.');
    return p.size() >= 2 ? p[0] + L"." + p[1] : v;
}

std::string Get(const wchar_t* host, const wchar_t* path) {
    std::string body;
    int status = 0;
    if (!net::HttpGet(host, path, 10000, &body, &status) || status != 200) body.clear();
    return body;
}

std::wstring Field(const std::string& obj, const char* key) {  // "key":"value" внутри одного объекта
    std::string k = std::string("\"") + key + "\":";
    size_t a = obj.find(k);
    if (a == std::string::npos) return L"";
    a += k.size();
    if (a < obj.size() && obj[a] == ' ') ++a;
    if (a >= obj.size() || obj[a] != '"') return L"";
    size_t b = obj.find('"', a + 1);
    return b == std::string::npos ? L"" : Widen(obj.substr(a + 1, b - a - 1));
}

// endoflife.date: массив циклов, новые первыми
struct Cycle {
    std::wstring cycle, latest, lts;  // lts - дата начала LTS или пусто
    std::wstring eol;                 // дата конца поддержки или пусто
};
std::vector<Cycle> Cycles(const wchar_t* product) {
    std::vector<Cycle> out;
    std::string js = Get(L"endoflife.date", (std::wstring(L"/api/") + product + L".json").c_str());
    for (size_t a = js.find('{'); a != std::string::npos; a = js.find('{', a + 1)) {
        size_t b = js.find('}', a);
        if (b == std::string::npos) break;
        std::string obj = js.substr(a, b - a + 1);
        Cycle c{Field(obj, "cycle"), Field(obj, "latest"), Field(obj, "lts"), Field(obj, "eol")};
        if (!c.cycle.empty() && !c.latest.empty()) out.push_back(c);
        a = b;
    }
    return out;
}

std::wstring Installed(const std::wstring& exe) {
    std::wstring out;
    if (exe.empty() || RunCapture(L"\"" + exe + L"\" --version", out, 15000) < 0) return L"";
    return FirstVersion(out);
}

std::wstring Today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[16];
    swprintf(b, 16, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

// Поезд релизов: ветки от своей до последней по возрастанию (циклы endoflife.date приходят новыми первыми).
std::vector<std::wstring> Train(const std::vector<Cycle>& cs, const std::wstring& mine, const std::wstring& last) {
    std::vector<std::wstring> t;
    for (auto it = cs.rbegin(); it != cs.rend(); ++it)
        if (Compare(it->cycle, mine) >= 0 && Compare(it->cycle, last) <= 0) t.push_back(it->cycle);
    return t;
}

// Ветки по номерам: 3.28 -> 3.31 даёт 3.28, 3.29, 3.30, 3.31 (только внутри одной старшей версии).
std::vector<std::wstring> MinorTrain(const std::wstring& from, const std::wstring& to) {
    std::vector<int> a = Parts(MajorMinor(from)), b = Parts(MajorMinor(to));
    std::vector<std::wstring> t;
    if (a.size() < 2 || b.size() < 2 || a[0] != b[0] || b[1] < a[1] || b[1] - a[1] > 12) return t;
    for (int m = a[1]; m <= b[1]; ++m) t.push_back(std::to_wstring(a[0]) + L"." + std::to_wstring(m));
    return t;
}

// «поддержка до 10.2026», если конец поддержки своей ветки ближе года (или прошёл)
std::wstring Eol(const std::vector<Cycle>& cs, const std::wstring& mine) {
    for (const Cycle& c : cs) {
        if (c.cycle != mine || c.eol.size() < 7) continue;
        std::wstring today = Today(), soon = std::to_wstring(_wtoi(today.substr(0, 4).c_str()) + 1) + today.substr(4);
        if (c.eol > soon) return L"";
        std::wstring mmyyyy = c.eol.substr(5, 2) + L"." + c.eol.substr(0, 4);
        return c.eol < today ? L"без поддержки с " + mmyyyy : L"поддержка до " + mmyyyy;
    }
    return L"";
}

}  // namespace

std::vector<ToolVersion> ToolVersions() {
    std::vector<ToolVersion> out;
    std::map<std::wstring, std::wstring> where;
    for (const Which& w : WhichAll()) where[w.cmd] = w.path;

    // Python: исправления своей ветки и вышедшая новая ветка
    if (where.count(L"python")) {
        ToolVersion t{L"Python", where[L"python"], Installed(where[L"python"])};
        std::vector<Cycle> cs = Cycles(L"python");
        if (!t.installed.empty() && !cs.empty()) {
            for (const Cycle& c : cs)
                if (c.cycle == MajorMinor(t.installed) && Compare(c.latest, t.installed) > 0)
                    t.latest = c.latest, t.note = L"исправления " + c.latest, t.newer = true;
            if (Compare(cs[0].cycle, MajorMinor(t.installed)) > 0) {
                t.note += (t.note.empty() ? L"" : L", ") + std::wstring(L"вышла ") + cs[0].latest;
                if (t.latest.empty()) t.latest = cs[0].latest;
                t.newer = true;
            }
            t.train = Train(cs, MajorMinor(t.installed), cs[0].cycle);
            t.eol = Eol(cs, MajorMinor(t.installed));
        }
        out.push_back(t);
    }
    // Node: исправления своей ветки и свежая LTS (нечётные ветки не LTS - их не предлагаем)
    if (where.count(L"node")) {
        ToolVersion t{L"Node", where[L"node"], Installed(where[L"node"])};
        std::vector<Cycle> cs = Cycles(L"nodejs");
        std::wstring today = Today(), major = Split(t.installed, L'.').empty() ? L"" : Split(t.installed, L'.')[0];
        for (const Cycle& c : cs)
            if (c.cycle == major && Compare(c.latest, t.installed) > 0)
                t.latest = c.latest, t.note = L"исправления " + c.latest, t.newer = true;
        for (const Cycle& c : cs)
            if (!c.lts.empty() && c.lts <= today) {
                if (Compare(c.cycle, major) > 0) {
                    t.note += (t.note.empty() ? L"" : L", ") + std::wstring(L"LTS ") + c.latest;
                    if (t.latest.empty()) t.latest = c.latest;
                    t.newer = true;
                    t.train = Train(cs, major, c.cycle);
                }
                break;
            }
        if (!major.empty()) t.eol = Eol(cs, major);
        out.push_back(t);
    }
    // CMake и Git: последний релиз на GitHub
    struct Gh {
        const wchar_t *name, *cmd, *repo;
    };
    for (const Gh& g : {Gh{L"CMake", L"cmake", L"/repos/Kitware/CMake/releases/latest"},
                        Gh{L"Git", L"git", L"/repos/git-for-windows/git/releases/latest"}}) {
        if (!where.count(g.cmd)) continue;
        ToolVersion t{g.name, where[g.cmd], Installed(where[g.cmd])};
        std::wstring tag = Field(Get(L"api.github.com", g.repo), "tag_name");
        std::wstring latest = FirstVersion(tag);
        // у Git для Windows версия «2.51.0.windows.1»: сравниваем три числа
        std::wstring inst = t.installed;
        if (!latest.empty() && !inst.empty()) {
            std::vector<std::wstring> a = Split(latest, L'.'), b = Split(inst, L'.');
            if (a.size() > 3) latest = a[0] + L"." + a[1] + L"." + a[2];
            if (b.size() > 3) inst = b[0] + L"." + b[1] + L"." + b[2];
            t.installed = inst;
            if (Compare(latest, inst) > 0) {
                t.latest = latest, t.note = L"вышла " + latest, t.newer = true;
                t.train = MinorTrain(inst, latest);
            }
        }
        out.push_back(t);
    }
    // MSYS2: checkupdates качает базы во временную папку, свои базы пакетов не трогает
    std::wstring bash = L"D:\\msys64\\usr\\bin\\bash.exe";
    if (GetFileAttributesW(bash.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(L"D:\\msys64\\usr\\bin\\checkupdates") != INVALID_FILE_ATTRIBUTES) {
        std::wstring res;
        RunCapture(L"\"" + bash + L"\" -lc checkupdates", res, 180000);
        int n = 0;
        std::wstring first;
        for (const std::wstring& l : Split(res, L'\n'))
            if (l.find(L" -> ") != std::wstring::npos) {
                if (n < 4) first += (first.empty() ? L"" : L", ") + l.substr(0, l.find(L' '));
                ++n;
            }
        ToolVersion t{L"MSYS2", bash};
        if (n) {
            t.newer = true;
            t.latest = std::to_wstring(n);
            t.note = Plural(n, L"пакет", L"пакета", L"пакетов") + L": " + first + (n > 4 ? L"..." : L"") +
                     L". Обновить: pacman -Syu в терминале MSYS2";
        }
        out.push_back(t);
    }
    return out;
}

}  // namespace tools
