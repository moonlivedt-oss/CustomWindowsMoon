#pragma once
#include <map>
#include <string>
#include <vector>

namespace net {
struct SettingStamp { std::wstring name, digest, summary; };
struct SettingChange { std::wstring name, before, after; };
std::vector<SettingStamp> ReadSettingStamps();
std::vector<SettingStamp> LoadSettingBaseline();
bool SaveSettingBaseline(const std::vector<SettingStamp>& stamps);

class SettingsLatch {
public:
    void Seed(const std::vector<SettingStamp>& stamps) {
        for (const auto& s : stamps) stable_[s.name] = s;
    }
    std::vector<SettingChange> Observe(const std::vector<SettingStamp>& stamps) {
        std::vector<SettingChange> result;
        for (const auto& s : stamps) {
            if (s.digest.empty()) continue;
            auto it = stable_.find(s.name);
            if (it == stable_.end()) { stable_[s.name] = s; continue; }
            if (it->second.digest == s.digest) { pending_.erase(s.name); continue; }
            auto p = pending_.find(s.name);
            if (p != pending_.end() && p->second == s.digest) {
                result.push_back({s.name, it->second.summary, s.summary});
                it->second = s;
                pending_.erase(s.name);
            } else pending_[s.name] = s.digest;
        }
        // Ошибка чтения не должна подтверждать последовательность изменений.
        for (auto it = pending_.begin(); it != pending_.end();) {
            bool present = false;
            for (const auto& s : stamps) if (s.name == it->first) present = true;
            if (!present) it = pending_.erase(it); else ++it;
        }
        return result;
    }
    std::vector<SettingStamp> Stable() const {
        std::vector<SettingStamp> out;
        for (const auto& p : stable_) out.push_back(p.second);
        return out;
    }
private:
    std::map<std::wstring, SettingStamp> stable_;
    std::map<std::wstring, std::wstring> pending_;
};

// Регулятор только наших больших передач: задержка растёт при ухудшении RTT и плавно спадает.
class TransferPacer {
public:
    unsigned Sample(int rtt) {
        if (rtt < 0) return delay_;
        if (base_ < 0 || rtt < base_) base_ = rtt;
        if (rtt > base_ + 50 && rtt > base_ * 2) delay_ = delay_ < 200 ? delay_ + 25 : 200;
        else delay_ = delay_ > 10 ? delay_ - 10 : 0;
        return delay_;
    }
    unsigned Delay() const { return delay_; }
private:
    int base_ = -1;
    unsigned delay_ = 0;
};

inline bool ExpectedTunnel(const std::wstring& expected, const std::wstring& route, bool tunnel) {
    return !expected.empty() && tunnel && expected == route;
}
}  // namespace net
