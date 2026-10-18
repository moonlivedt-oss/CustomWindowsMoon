#include "tools/guard.h"

#include <shlwapi.h>

#include <algorithm>

#include "core/text.h"

namespace guard {

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
