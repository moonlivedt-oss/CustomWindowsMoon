// Кто слушает микрофон или смотрит в камеру: ключ знакомой пары и выбор впервые включённых (под тестами).
#include "core/text.h"
#include "system/auto/capwatch.h"

namespace capwatch {

std::wstring SeenKey(const Use& u) { return u.cap + L"|" + Lower(u.path.empty() ? u.key : u.path); }

std::vector<Use> Fresh(const std::vector<Use>& now, const std::set<std::wstring>& seen) {
    std::vector<Use> out;
    std::set<std::wstring> taken;
    for (const Use& u : now) {
        std::wstring k = SeenKey(u);
        if (u.inUse && !seen.count(k) && taken.insert(k).second) out.push_back(u);
    }
    return out;
}

}  // namespace capwatch
