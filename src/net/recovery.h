#pragma once

#include <algorithm>
#include <cstdint>

namespace net {

// После сна DHCP и маршрут могут появиться позже первой проверки. Только короткие пробы:
// настройки адаптера, DNS, прокси и VPN этот режим не меняет.
inline unsigned RecoveryWait(std::uint64_t now, std::uint64_t until, unsigned normalMs) {
    return now < until ? static_cast<unsigned>(std::min<std::uint64_t>(2000, until - now)) : normalMs;
}

// Address churn may extend grace, but cannot postpone checks forever.
// Call under the watch lock; events within two seconds share one refresh.
class AddressBurst {
public:
    bool Observe(std::uint64_t now) {
        if (!active_ || now - last_ >= 25000) { start_ = now; active_ = true; accepted_ = false; }
        last_ = now;
        if (accepted_ && now - acceptedAt_ < 2000) return false;
        accepted_ = true;
        acceptedAt_ = now;
        until_ = std::min(now + 25000, start_ + 90000);
        return true;
    }
    std::uint64_t Until() const { return until_; }
private:
    bool active_ = false, accepted_ = false;
    std::uint64_t start_ = 0, last_ = 0, acceptedAt_ = 0, until_ = 0;
};

inline unsigned BackgroundProbeInterval(unsigned normalMs, bool constrained) {
    return constrained ? std::max(normalMs, 60000u) : normalMs;
}

}  // namespace net
