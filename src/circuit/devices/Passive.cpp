#include "circuit/devices/Passive.hpp"

#include <cmath>
#include <stdexcept>

#include "circuit/Circuit.hpp"

namespace esim {

// --- Resistor --------------------------------------------------------------

Resistor::Resistor(std::string name, int a, int b, double r, bool noisy)
    : Device(std::move(name)), a_(a), b_(b), g_(1.0 / r), noisy_(noisy) {
    if (!(r != 0.0)) throw std::runtime_error(this->name() + ": resistance must not be zero");
}

void Resistor::setup(Circuit& ckt) { kT_ = kBoltzmann * (ckt.options.temp + 273.15); }

void Resistor::noiseSources(std::vector<NoiseSource>& out) const {
    if (!noisy_) return;
    out.push_back({name(), a_, b_, 4.0 * kT_ * std::abs(g_), 0.0});  // thermal
}

void Resistor::stamp(StampContext& ctx) {
    ctx.stampCurrent(a_, b_, g_ * (ctx.v(a_) - ctx.v(b_)));
    ctx.stampConductance(a_, b_, g_);
}

// --- Capacitor -------------------------------------------------------------

Capacitor::Capacitor(std::string name, int a, int b, double c, std::optional<double> ic)
    : Device(std::move(name)), a_(a), b_(b), c_(c), ic_(ic) {}

void Capacitor::stamp(StampContext& ctx) {
    const double v = (ctx.useIc && ic_) ? *ic_ : ctx.v(a_) - ctx.v(b_);
    ctx.stampCharge(a_, b_, c_ * v);
    ctx.stampCapacitance(a_, b_, c_);
}

void Capacitor::applyIc(Vector& x0) const {
    if (!ic_) return;
    // Only unambiguous if one terminal is grounded; otherwise the charge
    // override in stamp() still enforces the initial condition.
    if (a_ >= 0 && b_ < 0) x0[a_] = *ic_;
    if (b_ >= 0 && a_ < 0) x0[b_] = -*ic_;
}

// --- Inductor --------------------------------------------------------------

Inductor::Inductor(std::string name, int a, int b, double l, std::optional<double> ic)
    : Device(std::move(name)), a_(a), b_(b), l_(l), ic_(ic) {}

void Inductor::setup(Circuit& ckt) {
    br_ = ckt.addUnknown("i(" + name() + ")", UnknownKind::Current, false);
}

void Inductor::stamp(StampContext& ctx) {
    // Branch equation: v(a) - v(b) - L di/dt = 0, i.e. f = v(a) - v(b), q = -L i.
    ctx.stampBranch(a_, b_, br_);
    const double i = (ctx.useIc && ic_) ? *ic_ : ctx.x[br_];
    ctx.addQ(br_, -l_ * i);
    ctx.addC(br_, br_, -l_);
}

void Inductor::applyIc(Vector& x0) const {
    if (ic_) x0[br_] = *ic_;
}

// --- MutualInductance ------------------------------------------------------

MutualInductance::MutualInductance(std::string name, std::string l1, std::string l2, double k)
    : Device(std::move(name)), name1_(std::move(l1)), name2_(std::move(l2)), k_(k) {
    if (!(std::abs(k_) < 1.0))
        throw std::runtime_error(this->name() + ": the coupling factor k must be between -1 and 1 "
                                                "(exclusive)");
}

void MutualInductance::bind(Circuit& ckt) {
    auto find = [&](const std::string& n) {
        const auto* l = dynamic_cast<const Inductor*>(ckt.findDevice(n));
        if (!l) throw std::runtime_error(name() + ": '" + n + "' is not an inductor");
        return l;
    };
    const Inductor* l1 = find(name1_);
    const Inductor* l2 = find(name2_);
    if (l1 == l2) throw std::runtime_error(name() + ": needs two different inductors");
    br1_ = l1->branch();
    br2_ = l2->branch();
    m_ = k_ * std::sqrt(l1->inductance() * l2->inductance());
}

void MutualInductance::stamp(StampContext& ctx) {
    // Adds -M di2/dt to the branch equation of L1 and -M di1/dt to that of L2.
    ctx.addQ(br1_, -m_ * ctx.x[br2_]);
    ctx.addC(br1_, br2_, -m_);
    ctx.addQ(br2_, -m_ * ctx.x[br1_]);
    ctx.addC(br2_, br1_, -m_);
}

}  // namespace esim
