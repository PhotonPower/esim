#include "circuit/devices/Behavioral.hpp"

#include <stdexcept>

#include "circuit/Circuit.hpp"
#include "circuit/Symbols.hpp"

namespace esim {

BehavioralSource::BehavioralSource(std::string name, int p, int n, Kind kind, std::string text,
                                   expr::AstPtr expression)
    : Device(std::move(name)), p_(p), n_(n), kind_(kind), text_(std::move(text)),
      ast_(std::move(expression)) {}

void BehavioralSource::setup(Circuit& ckt) {
    if (kind_ == Kind::Voltage || kind_ == Kind::Flux)
        br_ = ckt.addUnknown("i(" + name() + ")", UnknownKind::Current, false);
}

void BehavioralSource::bind(Circuit& ckt) {
    try {
        prog_ = expr::Program::compile(ast_, CircuitSymbols(ckt));
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("component '" + name() + "': \"" + text_ + "\": " + e.what());
    }
    if ((kind_ == Kind::Charge || kind_ == Kind::Flux) && prog_.dependsOnTime())
        throw std::runtime_error("component '" + name() +
                                 "': a charge or flux must not depend on time explicitly");
}

void BehavioralSource::stamp(StampContext& ctx) {
    const double value = prog_.evalGrad(ctx.x, ctx.t, grad_, ctx.frozenModes());
    const auto& vars = prog_.vars();
    switch (kind_) {
        case Kind::Current:
            ctx.stampCurrent(p_, n_, value);
            for (size_t k = 0; k < vars.size(); ++k) {
                ctx.addG(p_, vars[k], grad_[k]);
                ctx.addG(n_, vars[k], -grad_[k]);
            }
            break;
        case Kind::Charge:
            ctx.stampCharge(p_, n_, value);
            for (size_t k = 0; k < vars.size(); ++k) {
                ctx.addC(p_, vars[k], grad_[k]);
                ctx.addC(n_, vars[k], -grad_[k]);
            }
            break;
        case Kind::Voltage:
            ctx.stampBranch(p_, n_, br_);
            ctx.addF(br_, -value);
            for (size_t k = 0; k < vars.size(); ++k) ctx.addG(br_, vars[k], -grad_[k]);
            break;
        case Kind::Flux:  // v(p) - v(n) - d/dt flux = 0, like an inductor with flux = L i
            ctx.stampBranch(p_, n_, br_);
            ctx.addQ(br_, -value);
            for (size_t k = 0; k < vars.size(); ++k) ctx.addC(br_, vars[k], -grad_[k]);
            break;
    }
}

}  // namespace esim
