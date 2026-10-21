#include "tools/guard.h"

#include <shlwapi.h>

#include <algorithm>

#include "core/text.h"

namespace guard {

std::vector<ProcRule> ParseRules(const std::wstring& text, std::wstring* bad) {
    std::vector<ProcRule> out;
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find(L'|', a);
        std::wstring part = text.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        a = b == std::wstring::npos ? text.size() + 1 : b + 1;
        size_t eq = part.find(L'=');
        auto trim = [](std::wstring s) {
            size_t x = s.find_first_not_of(L" \t"), y = s.find_last_not_of(L" \t");
            return x == std::wstring::npos ? std::wstring() : s.substr(x, y - x + 1);
        };
        if (trim(part).empty()) continue;
        ProcRule r;
        r.mask = trim(part.substr(0, eq));
        bool ok = eq != std::wstring::npos && !r.mask.empty();
        std::wstring what = eq == std::wstring::npos ? L"" : part.substr(eq + 1);
        size_t c = 0;
        while (ok && c <= what.size()) {
            size_t d = what.find(L',', c);
            std::wstring w = trim(what.substr(c, d == std::wstring::npos ? std::wstring::npos : d - c));
            c = d == std::wstring::npos ? what.size() + 1 : d + 1;
            for (wchar_t& ch : w) ch = (wchar_t)towlower(ch);
            if (w.empty()) continue;
            if (w == L"idle" || w == L"low")
                r.prio = IDLE_PRIORITY_CLASS;
            else if (w == L"below")
                r.prio = BELOW_NORMAL_PRIORITY_CLASS;
            else if (w == L"normal")
                r.prio = NORMAL_PRIORITY_CLASS;
            else if (w == L"above")
                r.prio = ABOVE_NORMAL_PRIORITY_CLASS;
            else if (w == L"high")
                r.prio = HIGH_PRIORITY_CLASS;
            else if (w == L"eco")
                r.eco = 1;
            else if (w == L"noeco")
                r.eco = 0;
            else if (w.rfind(L"cores:", 0) == 0) {
                // 0-3,6 внутри одного слова через «;» или «+», запятая уже занята: cores:0-3+6
                std::wstring list = w.substr(6);
                size_t e = 0;
                while (ok && e <= list.size()) {
                    size_t f = list.find_first_of(L"+;", e);
                    std::wstring span = list.substr(e, f == std::wstring::npos ? std::wstring::npos : f - e);
                    e = f == std::wstring::npos ? list.size() + 1 : f + 1;
                    if (span.empty()) continue;
                    size_t dash = span.find(L'-');
                    int lo = _wtoi(span.c_str()), hi = dash == std::wstring::npos ? lo : _wtoi(span.c_str() + dash + 1);
                    if (!iswdigit(span[0]) || lo < 0 || hi < lo || hi > 63) {
                        ok = false;
                        break;
                    }
                    for (int k = lo; k <= hi; ++k) r.cores |= 1ull << k;
                }
                ok = ok && r.cores;
            } else {
                ok = false;
            }
        }
        if (ok && (r.prio || r.eco >= 0 || r.cores))
            out.push_back(r);
        else if (bad)
            *bad += (bad->empty() ? L"" : L"; ") + trim(part);
    }
    return out;
}

std::wstring DescribeRule(const ProcRule& r) {
    std::wstring s;
    auto add = [&](const std::wstring& w) { s += (s.empty() ? L"" : L", ") + w; };
    if (r.prio == IDLE_PRIORITY_CLASS) add(L"низкий");
    if (r.prio == BELOW_NORMAL_PRIORITY_CLASS) add(L"ниже обычного");
    if (r.prio == NORMAL_PRIORITY_CLASS) add(L"обычный");
    if (r.prio == ABOVE_NORMAL_PRIORITY_CLASS) add(L"выше обычного");
    if (r.prio == HIGH_PRIORITY_CLASS) add(L"высокий");
    if (r.eco == 1) add(L"экономия");
    if (r.eco == 0) add(L"без экономии");
    if (r.cores) {
        int n = 0;
        for (int k = 0; k < 64; ++k) n += (r.cores >> k) & 1;
        add(L"ядер: " + std::to_wstring(n));
    }
    return r.mask + L": " + s;
}

bool Match(const std::wstring& name, const std::wstring& masks) {
    for (const std::wstring& m : Split(masks, L'|'))
        if (!m.empty() && PathMatchSpecW(name.c_str(), m.c_str())) return true;
    return false;
}

void Balancer::Feed(ULONGLONG now, const std::vector<Proc>& procs, int cores, bool active,
                    const std::set<DWORD>& protect, std::vector<Act>& lower, std::vector<Act>& restore) {
    std::set<DWORD> alive;
    for (const Proc& p : procs) {
        alive.insert(p.pid);
        auto it = st_.find(p.pid);
        if (it == st_.end() || it->second.created != p.created) {  // новый процесс (или номер достался другому)
            St s;
            s.name = p.name, s.created = p.created, s.cpu = p.cpu, s.at = now;
            st_[p.pid] = s;
            continue;
        }
        St& s = it->second;
        ULONGLONG dt = now > s.at ? now - s.at : 0;
        if (dt < 1000) continue;
        // процессорное время в 100 нс за dt мс на всех ядрах -> доля всего процессора
        double used = p.cpu > s.cpu ? (double)(p.cpu - s.cpu) / 10000.0 : 0;
        s.share = used / ((double)dt * std::max(1, cores));
        s.cpu = p.cpu, s.at = now;
        bool hot = s.share >= kHog, calm = s.share < kCalm;
        if (hot) {
            if (!s.hogSince) s.hogSince = now;
            s.calmSince = 0;
        } else {
            s.hogSince = 0;
            if (calm && !s.calmSince) s.calmSince = now;
            if (!calm) s.calmSince = 0;
        }
        bool prot = protect.count(p.pid) > 0;
        if (!s.lowered && active && !prot && s.hogSince && now - s.hogSince >= kHogMs) {
            s.lowered = true;
            lower.push_back({p.pid, s.name, s.share});
        } else if (s.lowered && (!active || prot || (s.calmSince && now - s.calmSince >= kCalmMs))) {
            s.lowered = false;
            s.hogSince = 0;
            restore.push_back({p.pid, s.name, s.share});
        }
    }
    for (auto it = st_.begin(); it != st_.end();)
        it = alive.count(it->first) ? std::next(it) : st_.erase(it);  // закрылся - возвращать нечего
}

std::vector<Balancer::Act> Balancer::Lowered() const {
    std::vector<Act> out;
    for (const auto& [pid, s] : st_)
        if (s.lowered) out.push_back({pid, s.name, s.share});
    return out;
}

std::wstring KeepAwake(const std::vector<Proc>& procs, const std::wstring& masks) {
    for (const Proc& p : procs)
        if (Match(p.name, masks)) return p.name;
    return L"";
}

std::vector<LongWatch::Done> LongWatch::Feed(ULONGLONG now, const std::vector<Proc>& procs, const std::wstring& masks,
                                             ULONGLONG minMs) {
    std::map<DWORD, const Proc*> byPid;
    for (const Proc& p : procs) byPid[p.pid] = &p;
    for (const Proc& p : procs) {
        if (!Match(p.name, masks)) continue;
        auto it = runs_.find(p.pid);
        if (it != runs_.end() && it->second.created == p.created) {
            it->second.seen = now;
            continue;
        }
        Run r;
        r.name = p.name, r.created = p.created, r.seen = now;
        // цепочка родителей (с проверкой, что родитель старше: номер мог достаться новому процессу)
        DWORD cur = p.parent;
        ULONGLONG childCreated = p.created;
        for (int i = 0; i < 8 && cur; ++i) {
            auto pp = byPid.find(cur);
            if (pp == byPid.end() || pp->second->created > childCreated) break;
            r.ancestors.push_back(cur);
            auto tr = runs_.find(cur);
            if (tr != runs_.end() && tr->second.created == pp->second->created) r.nested = true;
            childCreated = pp->second->created;
            cur = pp->second->parent;
        }
        runs_[p.pid] = r;
    }
    std::vector<Done> out;
    for (auto it = runs_.begin(); it != runs_.end();) {
        if (it->second.seen == now) {
            ++it;
            continue;
        }
        // закончился: шёл от создания до последнего раза, когда его видели
        const Run& r = it->second;
        ULONGLONG ms = r.seen > r.created ? (r.seen - r.created) / 10000 : 0;
        if (!r.nested && ms >= minMs) out.push_back({it->first, r.name, ms, r.ancestors});
        it = runs_.erase(it);
    }
    return out;
}

}  // namespace guard
