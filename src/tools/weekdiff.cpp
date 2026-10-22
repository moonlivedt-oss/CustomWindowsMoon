// «Что изменилось за неделю»: строки снимка и сравнение - без системы, под тестами. Сбор снимка - weeksnap.cpp.
#include "tools/weekdiff.h"

#include <algorithm>
#include <map>

namespace tools {

std::wstring WeekRows(const std::vector<WeekEntry>& s) {
    std::wstring t;
    for (const WeekEntry& e : s) t += e.kind + L"\t" + e.key + L"\t" + e.label + L"\n";
    return t;
}

std::vector<WeekEntry> ParseWeekRows(const std::wstring& text) {
    std::vector<WeekEntry> out;
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find(L'\n', a);
        std::wstring line = text.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
        size_t t1 = line.find(L'\t'), t2 = t1 == std::wstring::npos ? t1 : line.find(L'\t', t1 + 1);
        if (t2 != std::wstring::npos)
            out.push_back({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
        if (e == std::wstring::npos) break;
        a = e + 1;
    }
    return out;
}

std::vector<WeekChange> WeekDiff(const std::vector<WeekEntry>& old, const std::vector<WeekEntry>& now) {
    auto index = [](const std::vector<WeekEntry>& v) {
        std::map<std::wstring, const WeekEntry*> m;
        for (const WeekEntry& e : v) m[e.kind + L"\t" + e.key] = &e;
        return m;
    };
    std::map<std::wstring, const WeekEntry*> was = index(old), is = index(now);
    std::vector<WeekChange> out;
    for (const auto& [k, e] : is) {
        auto it = was.find(k);
        if (it == was.end())
            out.push_back({WeekChange::ADDED, *e, L""});
        else if (it->second->label != e->label)
            out.push_back({WeekChange::UPDATED, *e, it->second->label});
    }
    for (const auto& [k, e] : was)
        if (!is.count(k)) out.push_back({WeekChange::REMOVED, *e, L""});
    // сначала новое (о нём и спрашивают), потом удалённое, обновления в конце
    std::stable_sort(out.begin(), out.end(), [](const WeekChange& a, const WeekChange& b) {
        auto rank = [](WeekChange::What w) { return w == WeekChange::ADDED ? 0 : w == WeekChange::REMOVED ? 1 : 2; };
        return rank(a.what) < rank(b.what);
    });
    return out;
}

const wchar_t* WeekKindName(const std::wstring& kind) {
    if (kind == L"app") return L"программа";
    if (kind == L"start") return L"автозапуск";
    if (kind == L"svc") return L"служба";
    if (kind == L"drv") return L"драйвер";
    if (kind == L"task") return L"задача";
    return L"запись";
}

}  // namespace tools
