#include "core/stroke.h"

#include <cmath>

namespace stroke {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void Tracker::Start(unsigned long long t, double x, double y) {
    pts_.clear();
    pts_.push_back({t, x, y});
}

void Tracker::Feed(unsigned long long t, double x, double y) {
    if (pts_.empty()) return Start(t, x, y);
    const P& last = pts_.back();
    if (last.x == x && last.y == y) return;
    if (pts_.size() < 400) pts_.push_back({t, x, y});
}

bool Tracker::Flick(double minPx) const {
    if (pts_.size() < 2) return false;
    const P& a = pts_.front();
    for (const P& p : pts_) {
        if (p.t - a.t > kWindowMs) break;
        double d = std::hypot(p.x - a.x, p.y - a.y);
        if (d >= minPx) return d / (double)(p.t - a.t ? p.t - a.t : 1) >= kMinSpeed;
    }
    return false;
}

int Tracker::Turns() const {
    // точки ломаной: каждая следующая - не ближе kSegPx к прошлой, чтобы дрожь руки не давала поворотов
    std::vector<P> v;
    for (const P& p : pts_)
        if (v.empty() || std::hypot(p.x - v.back().x, p.y - v.back().y) >= kSegPx) v.push_back(p);
    int turns = 0;
    for (size_t i = 2; i < v.size(); ++i) {
        double a1 = std::atan2(v[i - 1].y - v[i - 2].y, v[i - 1].x - v[i - 2].x);
        double a2 = std::atan2(v[i].y - v[i - 1].y, v[i].x - v[i - 1].x);
        double d = std::fabs(a2 - a1) * 180 / kPi;
        if (d > 180) d = 360 - d;
        if (d > kTurnDeg) ++turns;
    }
    return turns;
}

double Tracker::Angle() const {
    if (pts_.size() < 2) return 0;
    const P& a = pts_.front();
    for (const P& p : pts_)
        if (std::hypot(p.x - a.x, p.y - a.y) >= kSegPx) return std::atan2(p.y - a.y, p.x - a.x) * 180 / kPi;
    const P& b = pts_.back();
    return std::atan2(b.y - a.y, b.x - a.x) * 180 / kPi;
}

}  // namespace stroke
