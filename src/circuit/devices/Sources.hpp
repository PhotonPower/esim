#pragma once

#include <complex>
#include <memory>

#include "circuit/Device.hpp"
#include "circuit/Waveform.hpp"

namespace esim {

// Common part of independent voltage and current sources: a DC value, an
// optional waveform for transient analysis and an AC small-signal phasor.
class IndependentSource : public Device {
public:
    struct DcState {
        bool has;
        double value;
    };

    IndependentSource(std::string name, int p, int n) : Device(std::move(name)), p_(p), n_(n) {}

    void setDc(double v) { dc_ = {true, v}; }
    DcState dcState() const { return dc_; }
    void setDcState(DcState s) { dc_ = s; }
    void setWave(std::unique_ptr<Waveform> w) { wave_ = std::move(w); }
    void setAc(double magnitude, double phaseDeg);

    double nextBreakpoint(double t) const override;
    double maxStep() const override { return wave_ ? wave_->maxStep() : Device::maxStep(); }

protected:
    // Transient analysis uses the waveform. DC analyses use the DC value and
    // fall back to the waveform at t = 0.
    double value(const StampContext& ctx) const;
    std::complex<double> ac() const { return ac_; }

    int p_, n_;

private:
    DcState dc_{false, 0.0};
    std::unique_ptr<Waveform> wave_;
    std::complex<double> ac_{0.0, 0.0};
};

// Voltage source: v(p) - v(n) = V. The branch current flows from p through
// the source to n (it is negative while the source delivers power).
class VoltageSource final : public IndependentSource {
public:
    using IndependentSource::IndependentSource;
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void stampAc(Eigen::VectorXcd& rhs) const override;
    int branch() const override { return br_; }

private:
    int br_ = -1;
};

// Current source: I flows from p through the source to n.
class CurrentSource final : public IndependentSource {
public:
    using IndependentSource::IndependentSource;
    void stamp(StampContext& ctx) override;
    void stampAc(Eigen::VectorXcd& rhs) const override;
};

// E: v(p) - v(n) = gain * (v(cp) - v(cn))
class Vcvs final : public Device {
public:
    Vcvs(std::string name, int p, int n, int cp, int cn, double gain);
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    int branch() const override { return br_; }

private:
    int p_, n_, cp_, cn_, br_ = -1;
    double gain_;
};

// G: current gm * (v(cp) - v(cn)) from p through the source to n
class Vccs final : public Device {
public:
    Vccs(std::string name, int p, int n, int cp, int cn, double gm);
    void stamp(StampContext& ctx) override;

private:
    int p_, n_, cp_, cn_;
    double gm_;
};

// F: current gain * i(control) from p through the source to n
class Cccs final : public Device {
public:
    Cccs(std::string name, int p, int n, std::string control, double gain);
    void bind(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;

private:
    int p_, n_, cbr_ = -1;
    std::string control_;
    double gain_;
};

// H: v(p) - v(n) = r * i(control)
class Ccvs final : public Device {
public:
    Ccvs(std::string name, int p, int n, std::string control, double r);
    void setup(Circuit& ckt) override;
    void bind(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    int branch() const override { return br_; }

private:
    int p_, n_, br_ = -1, cbr_ = -1;
    std::string control_;
    double r_;
};

}  // namespace esim
