#pragma once

#include <limits>
#include <utility>
#include <vector>

namespace esim {

// Time dependence of an independent source.
class Waveform {
public:
    virtual ~Waveform() = default;
    virtual double value(double t) const = 0;
    // Smallest time > t at which the waveform has a kink.
    virtual double nextBreakpoint(double /*t*/) const {
        return std::numeric_limits<double>::infinity();
    }
    // Step size that still resolves the waveform (piecewise linear ones need none).
    virtual double maxStep() const { return std::numeric_limits<double>::infinity(); }
};

class DcWave final : public Waveform {
public:
    explicit DcWave(double v) : v_(v) {}
    double value(double) const override { return v_; }

private:
    double v_;
};

// offset + amplitude * exp(-damping (t - delay)) * sin(2 pi freq (t - delay) + phase)
class SinWave final : public Waveform {
public:
    SinWave(double offset, double amplitude, double freq, double delay, double damping,
            double phaseDeg);
    double value(double t) const override;
    double nextBreakpoint(double t) const override;
    // At least 20 points per period.
    double maxStep() const override {
        return freq_ > 0.0 ? 1.0 / (20.0 * freq_) : std::numeric_limits<double>::infinity();
    }

private:
    double offset_, amplitude_, freq_, delay_, damping_, phase_;
};

// Trapezoidal pulse between v1 and v2. period and width may be infinite.
class PulseWave final : public Waveform {
public:
    PulseWave(double v1, double v2, double delay, double rise, double fall, double width,
              double period);
    double value(double t) const override;
    double nextBreakpoint(double t) const override;

private:
    double v1_, v2_, delay_, rise_, fall_, width_, period_;
};

// Piecewise linear through (t, v) points, constant outside.
class PwlWave final : public Waveform {
public:
    explicit PwlWave(std::vector<std::pair<double, double>> points);
    double value(double t) const override;
    double nextBreakpoint(double t) const override;

private:
    std::vector<std::pair<double, double>> pts_;
};

}  // namespace esim
