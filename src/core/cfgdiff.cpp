// Сравнение двух версий radial.cfg по смыслу (cfgdiff.h).
#include "core/cfgdiff.h"

#include <map>

namespace cfgdiff {

namespace {

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r"), b = s.find_last_not_of(L" \t\r");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::wstring Short(const std::wstring& s) { return s.size() > 48 ? s.substr(0, 47) + L"…" : s; }

struct Sector {
    std::map<std::wstring, std::wstring> props;                // icon, path, expand...
    std::vector<std::pair<std::wstring, std::wstring>> items;  // подпись -> вся строка item
};
struct Parsed {
    std::map<std::wstring, std::wstring> settings;
    std::map<std::wstring, Sector> sectors[2];  // левый, правый край
    std::vector<std::wstring> order[2];
};

Parsed Parse(const std::wstring& text) {
    Parsed p;
    enum { NONE, SETTINGS, SECTOR } mode = NONE;
    int edge = 0;
    Sector* cur = nullptr;
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find(L'\n', a);
        std::wstring line = Trim(text.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a));
        a = e == std::wstring::npos ? text.size() : e + 1;
        if (line.empty() || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            std::wstring name = line.substr(1, line.size() - 2);
            if (name == L"settings") {
                mode = SETTINGS;
            } else if (name == L"@left" || name == L"@right") {
                edge = name == L"@right";
                mode = NONE;
            } else {
                mode = SECTOR;
                if (!p.sectors[edge].count(name)) p.order[edge].push_back(name);
                cur = &p.sectors[edge][name];
            }
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq)), val = Trim(line.substr(eq + 1));
        if (mode == SETTINGS) {
            p.settings[key] = val;
        } else if (mode == SECTOR && cur) {
            if (key == L"item")
                cur->items.push_back({Trim(val.substr(0, val.find(L'|'))), val});
            else
                cur->props[key] = val;
        }
    }
    return p;
}

void DiffMaps(const std::map<std::wstring, std::wstring>& o, const std::map<std::wstring, std::wstring>& n,
              const std::wstring& where, std::vector<Change>& out) {
    for (const auto& [k, v] : n) {
        auto it = o.find(k);
        if (it == o.end())
            out.push_back({Change::ADDED, where, k + L": " + Short(v)});
        else if (it->second != v)
            out.push_back({Change::CHANGED, where, k + L": " + Short(it->second) + L" -> " + Short(v)});
    }
    for (const auto& [k, v] : o)
        if (!n.count(k)) out.push_back({Change::REMOVED, where, k + L": " + Short(v)});
}

}  // namespace

std::vector<Change> Diff(const std::wstring& older, const std::wstring& newer) {
    Parsed o = Parse(older), n = Parse(newer);
    std::vector<Change> out;
    DiffMaps(o.settings, n.settings, L"Настройки", out);
    const wchar_t* edgeName[2] = {L"Левый край", L"Правый край"};
    for (int e = 0; e < 2; ++e) {
        for (const std::wstring& name : n.order[e])
            if (!o.sectors[e].count(name)) out.push_back({Change::ADDED, edgeName[e], L"сектор «" + name + L"»"});
        for (const std::wstring& name : o.order[e])
            if (!n.sectors[e].count(name)) out.push_back({Change::REMOVED, edgeName[e], L"сектор «" + name + L"»"});
        // общие секторы: порядок, свойства, пункты по подписи
        std::vector<std::wstring> co, cn;
        for (const std::wstring& s : o.order[e])
            if (n.sectors[e].count(s)) co.push_back(s);
        for (const std::wstring& s : n.order[e])
            if (o.sectors[e].count(s)) cn.push_back(s);
        if (co != cn) out.push_back({Change::CHANGED, edgeName[e], L"порядок секторов"});
        for (const std::wstring& name : cn) {
            const Sector &so = o.sectors[e].at(name), &sn = n.sectors[e].at(name);
            std::wstring where = std::wstring(edgeName[e]) + L" / " + name;
            DiffMaps(so.props, sn.props, where, out);
            auto find = [](const Sector& s, const std::wstring& label) -> const std::wstring* {
                for (const auto& [l, line] : s.items)
                    if (l == label) return &line;
                return nullptr;
            };
            for (const auto& [label, line] : sn.items) {
                const std::wstring* was = find(so, label);
                if (!was)
                    out.push_back({Change::ADDED, where, L"пункт «" + label + L"»"});
                else if (*was != line)
                    out.push_back(
                        {Change::CHANGED, where, L"пункт «" + label + L"»: " + Short(*was) + L" -> " + Short(line)});
            }
            for (const auto& [label, line] : so.items)
                if (!find(sn, label)) out.push_back({Change::REMOVED, where, L"пункт «" + label + L"»"});
        }
    }
    return out;
}

std::wstring Summary(const std::vector<Change>& changes) {
    int add = 0, rem = 0, chg = 0;
    for (const Change& c : changes) (c.kind == Change::ADDED ? add : c.kind == Change::REMOVED ? rem : chg)++;
    std::wstring s;
    if (add) s += L"+" + std::to_wstring(add);
    if (rem) s += (s.empty() ? L"-" : L", -") + std::to_wstring(rem);
    if (chg) s += (s.empty() ? L"изменено " : L", изменено ") + std::to_wstring(chg);
    return s;
}

}  // namespace cfgdiff
