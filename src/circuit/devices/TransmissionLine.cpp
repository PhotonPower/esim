#include "circuit/devices/TransmissionLine.hpp"

#include <stdexcept>

#include "circuit/Circuit.hpp"

namespace esim {

TransmissionLine::TransmissionLine(std::string name, int p1, int n1, int p2, int n2, double z0, double td)
    : Device(std::move(name)), p1_(p1), n1_(n1), p2_(p2), n2_(n2), z0_(z0), td_(td) {
    if (!(z0_ > 0.0)) throw std::runtime_error(this->name() + ": z0 must be > 0");
    if (!(td_ > 0.0)) throw std::runtime_error(this->name() + ": td must be > 0");
}

void TransmissionLine::setup(Circuit& ckt) {
    br1_ = ckt.addUnknown("i(" + name() + ")", UnknownKind::Current, false);
    br2_ = ckt.addUnknown("i(" + name() + ".2)", UnknownKind::Current, true);
}

void TransmissionLine::reset() {
    w1_.clear();
    w2_.clear();
    w1_.setDelay(td_);
    w2_.setDelay(td_);
}

void TransmissionLine::acceptStep(double t, const Vector& x, bool start) {
    if (start) reset();
    const auto v = [&](int node) { return node < 0 ? 0.0 : x[node]; };
    w1_.add(t, v(p1_) - v(n1_) + z0_ * x[br1_]);
    w2_.add(t, v(p2_) - v(n2_) + z0_ * x[br2_]);
    if (start) noteDiscontinuity(t);
}

void TransmissionLine::stamp(StampContext& ctx) {
    ctx.stampBranch(p1_, n1_, br1_);  // port currents, and v1, v2 in the two wave equations
    ctx.stampBranch(p2_, n2_, br2_);
    ctx.addF(br1_, -z0_ * ctx.x[br1_]);
    ctx.addG(br1_, br1_, -z0_);
    ctx.addF(br2_, -z0_ * ctx.x[br2_]);
    ctx.addG(br2_, br2_, -z0_);

    // The wave arriving from the other port: from the past during a time
    // integration, otherwise the present one (steady state). The matrix entries
    // are stamped in both cases, so that the pattern does not change.
    const bool past = ctx.transient && !w1_.empty();
    const double now = past ? 0.0 : 1.0;
    const double v1 = ctx.v(p1_) - ctx.v(n1_), v2 = ctx.v(p2_) - ctx.v(n2_);
    ctx.addF(br1_, past ? -w2_.delayed(ctx.t) : -(v2 + z0_ * ctx.x[br2_]));
    ctx.addG(br1_, p2_, -now);
    ctx.addG(br1_, n2_, now);
    ctx.addG(br1_, br2_, -now * z0_);
    ctx.addF(br2_, past ? -w1_.delayed(ctx.t) : -(v1 + z0_ * ctx.x[br1_]));
    ctx.addG(br2_, p1_, -now);
    ctx.addG(br2_, n1_, now);
    ctx.addG(br2_, br1_, -now * z0_);
}

// The operating point contains "- (v + z0 i)" of the other port; the AC
// equations need "- exp(-j w td) (v + z0 i)". This adds the difference.
void TransmissionLine::stampAcMatrix(double omega,
                                     std::vector<Eigen::Triplet<std::complex<double>>>& out) const {
    const std::complex<double> d = 1.0 - std::exp(std::complex<double>(0.0, -omega * td_));
    const auto add = [&](int row, int col, std::complex<double> value) {
        if (row >= 0 && col >= 0) out.emplace_back(row, col, value);
    };
    add(br1_, p2_, d);
    add(br1_, n2_, -d);
    add(br1_, br2_, d * z0_);
    add(br2_, p1_, d);
    add(br2_, n1_, -d);
    add(br2_, br1_, d * z0_);
}

}  // namespace esim
