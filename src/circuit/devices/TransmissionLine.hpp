#pragma once

#include "circuit/Device.hpp"
#include "core/History.hpp"

namespace esim {

// Ideal (lossless) transmission line with characteristic impedance z0 and
// delay td between port 1 (p1, n1) and port 2 (p2, n2). With the currents i1,
// i2 flowing into the line at the positive terminals, the two waves give
//     v1(t) - z0 i1(t) = v2(t - td) + z0 i2(t - td)
//     v2(t) - z0 i2(t) = v1(t - td) + z0 i1(t - td)
// The right-hand sides are known from the past, which is why the step size
// never exceeds td. At DC the line is a pair of wires (v1 = v2, i1 = -i2); in
// the AC analysis the delay becomes the factor exp(-j w td).
class TransmissionLine final : public Device {
public:
    TransmissionLine(std::string name, int p1, int n1, int p2, int n2, double z0, double td);
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void stampAcMatrix(double omega, std::vector<Eigen::Triplet<std::complex<double>>>& out) const override;
    int branch() const override { return br1_; }
    double maxStep() const override { return td_; }
    void acceptStep(double t, const Vector& x, bool start) override;
    bool usesHistory() const override { return true; }
    void noteDiscontinuity(double t) override {
        w1_.mark(t);
        w2_.mark(t);
    }
    double nextBreakpoint(double t) const override {
        return std::min(w1_.nextBreakpoint(t), w2_.nextBreakpoint(t));
    }
    void reset() override;

private:
    int p1_, n1_, p2_, n2_;
    double z0_, td_;
    int br1_ = -1, br2_ = -1;
    History w1_, w2_;  // v + z0 i at port 1 and port 2
};

}  // namespace esim
