#include "circuit/Waveform.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/Types.hpp"

namespace esim {

namespace {
const double kInf = std::numeric_limits<double>::infinity();
}

// --- SinWave ---------------------------------------------------------------

SinWave::SinWave(double offset, double amplitude, double freq, double delay, double damping,
                 double phaseDeg)
    : offset_(offset),
      amplitude_(amplitude),
      freq_(freq),
      delay_(delay),
      damping_(damping),
      phase_(phaseDeg * kPi / 180.0) {}

double SinWave::value(double t) const {
    if (t <= delay_) return offset_ + amplitude_ * std::sin(phase_);
    const double tt = t - delay_;
    return offset_ + amplitude_ * std::exp(-damping_ * tt) * std::sin(2.0 * kPi * freq_ * tt + phase_);
}

double SinWave::nextBreakpoint(double t) const { return t < delay_ ? delay_ : kInf; }

// --- PulseWave -------------------------------------------------------------

PulseWave::PulseWave(double v1, double v2, double delay, double rise, double fall, double width,
                     double period)
    : v1_(v1), v2_(v2), delay_(delay), rise_(rise), fall_(fall), width_(width), period_(period) {
    if (!(rise_ > 0.0) || !(fall_ > 0.0))
        throw std::runtime_error("pulse: rise and fall time must be > 0");
    if (std::isfinite(period_) && period_ < rise_ + width_ + fall_)
        throw std::runtime_error("pulse: period is shorter than rise + width + fall");
}

double PulseWave::value(double t) const {
    if (t <= delay_) return v1_;
    double tt = t - delay_;
    if (std::isfinite(period_)) tt = std::fmod(tt, period_);
    if (tt < rise_) return v1_ + (v2_ - v1_) * tt / rise_;
    if (tt < rise_ + width_) return v2_;
    if (tt < rise_ + width_ + fall_) return v2_ + (v1_ - v2_) * (tt - rise_ - width_) / fall_;
    return v1_;
}

double PulseWave::nextBreakpoint(double t) const {
    if (t < delay_) return delay_;
    const bool periodic = std::isfinite(period_);
    const double base = periodic ? delay_ + std::floor((t - delay_) / period_) * period_ : delay_;
    const double offsets[] = {0.0, rise_, rise_ + width_, rise_ + width_ + fall_};
    double best = kInf;
    for (int k = 0; k < (periodic ? 2 : 1); ++k) {
        for (double off : offsets) {
            const double c = base + (k ? period_ : 0.0) + off;
            if (std::isfinite(c) && c > t) best = std::min(best, c);
        }
    }
    return best;
}

// --- PwlWave ---------------------------------------------------------------

PwlWave::PwlWave(std::vector<std::pair<double, double>> points) : pts_(std::move(points)) {
    if (pts_.empty()) throw std::runtime_error("pwl: at least one point is required");
    for (size_t i = 1; i < pts_.size(); ++i)
        if (!(pts_[i].first > pts_[i - 1].first))
            throw std::runtime_error("pwl: times must be strictly increasing");
}

double PwlWave::value(double t) const {
    if (t <= pts_.front().first) return pts_.front().second;
    if (t >= pts_.back().first) return pts_.back().second;
    auto hi = std::upper_bound(pts_.begin(), pts_.end(), t,
                               [](double v, const auto& p) { return v < p.first; });
    auto lo = hi - 1;
    return lo->second + (hi->second - lo->second) * (t - lo->first) / (hi->first - lo->first);
}

double PwlWave::nextBreakpoint(double t) const {
    auto it = std::upper_bound(pts_.begin(), pts_.end(), t,
                               [](double v, const auto& p) { return v < p.first; });
    return it == pts_.end() ? kInf : it->first;
}

}  // namespace esim
