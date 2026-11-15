#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <utility>

namespace esim {

// Past values of a quantity, recorded at the accepted points of a time
// integration and read back a fixed delay later (delays, transmission lines).
//
// Kinks and jumps need care. Where a delayed kink arrives, nothing warns the
// integrator: it would step across, and the curve recorded there (which is
// delayed again in the next round) would be smeared over that step. So the
// integrator reports the times at which it restarted (breakpoints of sources,
// events); if the recorded curve really has a kink there, the time one delay
// later becomes a breakpoint in turn. Reflections that have died away no
// longer produce breakpoints.
class History {
public:
    void setDelay(double td) { td_ = td; }
    double delay() const { return td_; }
    void clear() {
        pts_.clear();
        breaks_.clear();
        marked_ = false;
        peak_ = 0.0;
    }
    bool empty() const { return pts_.empty(); }

    // Record the value at time t (t must not decrease).
    void add(double t, double value) {
        peak_ = std::max(peak_, std::abs(value));
        if (marked_ && t > mark_ && !pts_.empty() && pts_.back().first == mark_) {
            marked_ = false;
            // Values at the marked time before and after a possible jump, and
            // the slopes on both sides.
            size_t first = pts_.size() - 1;
            while (first > 0 && pts_[first - 1].first == mark_) --first;
            const double before = first > 0 ? (pts_[first].second - pts_[first - 1].second) /
                                                  (mark_ - pts_[first - 1].first)
                                            : 0.0;
            const double after = (value - pts_.back().second) / (t - mark_);
            const double tol = 1e-4 * peak_ + 1e-30;
            if (std::abs(pts_.back().second - pts_[first].second) > tol ||
                std::abs(after - before) * td_ > tol)
                breaks_.push_back(mark_ + td_);
        }
        pts_.emplace_back(t, value);
        while (pts_.size() > 2 && pts_[1].first < t - 2.0 * td_) pts_.pop_front();
        while (!breaks_.empty() && breaks_.front() <= t) breaks_.pop_front();
    }

    // The integration restarted at time t, the time of the last recorded point.
    void mark(double t) {
        mark_ = t;
        marked_ = true;
    }

    // Next time after t at which a kink of the recorded curve arrives.
    double nextBreakpoint(double t) const {
        for (double b : breaks_)
            if (b > t) return b;
        return std::numeric_limits<double>::infinity();
    }

    // The value one delay before t.
    double delayed(double t) const { return at(t - td_); }

    // Value at time t, interpolated between the recorded points. Before the
    // first point its value is returned (the quantity was at rest), after the
    // last one the last value. At a jump (two points with the same time) the
    // value after the jump counts.
    //
    // Where the curve is smooth, the cubic through the four surrounding points
    // is used: integration steps are often long compared with the curvature,
    // and a straight line would then be the largest error of the whole
    // simulation. At kinks and jumps a cubic would overshoot; there the second
    // differences on both sides disagree, and the straight line is taken.
    double at(double t) const {
        if (pts_.empty()) return 0.0;
        auto hi = std::upper_bound(pts_.begin(), pts_.end(), t,
                                   [](double time, const std::pair<double, double>& p) { return time < p.first; });
        if (hi == pts_.begin()) return pts_.front().second;
        if (hi == pts_.end()) return pts_.back().second;
        const auto lo = hi - 1;
        const double t1 = lo->first, t2 = hi->first, y1 = lo->second, y2 = hi->second;
        if (!(t2 > t1)) return y2;
        const double linear = y1 + (y2 - y1) * (t - t1) / (t2 - t1);
        if (lo == pts_.begin() || hi + 1 == pts_.end()) return linear;
        const double t0 = (lo - 1)->first, y0 = (lo - 1)->second, t3 = (hi + 1)->first, y3 = (hi + 1)->second;
        if (!(t1 > t0) || !(t3 > t2)) return linear;
        const double s0 = (y1 - y0) / (t1 - t0), s1 = (y2 - y1) / (t2 - t1), s2 = (y3 - y2) / (t3 - t2);
        const double ca = (s1 - s0) / (t2 - t0), cb = (s2 - s1) / (t3 - t1);  // second differences
        if (!(ca * cb > 0.0) || std::abs(ca) > 4.0 * std::abs(cb) || std::abs(cb) > 4.0 * std::abs(ca))
            return linear;
        // Newton form of the cubic through the four points.
        const double third = (cb - ca) / (t3 - t0);
        return y0 + (t - t0) * (s0 + (t - t1) * (ca + (t - t2) * third));
    }

private:
    std::deque<std::pair<double, double>> pts_;
    std::deque<double> breaks_;  // arrival times of kinks, ascending
    double td_ = 0.0;
    double mark_ = 0.0;
    bool marked_ = false;
    double peak_ = 0.0;  // largest magnitude recorded: the scale of what counts as a kink
};

}  // namespace esim
