#include "circuit/devices/Switching.hpp"

#include <stdexcept>

#include "circuit/Circuit.hpp"
#include "circuit/Symbols.hpp"

namespace esim {

// --- Switch ----------------------------------------------------------------

Switch::Switch(std::string name, int p, int n, expr::AstPtr control, const SwitchParams& params)
    : Device(std::move(name)), a_(p), b_(n), ast_(std::move(control)), p_(params),
      on_(params.initiallyOn) {
    if (!(p_.ron > 0.0) || !(p_.roff > 0.0))
        throw std::runtime_error(this->name() + ": ron and roff must be > 0");
    if (p_.hysteresis < 0.0) throw std::runtime_error(this->name() + ": hysteresis must be >= 0");
}

void Switch::bind(Circuit& ckt) {
    try {
        control_ = expr::Program::compile(ast_, CircuitSymbols(ckt));
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("component '" + name() + "', control: " + e.what());
    }
}

bool Switch::update(double control) {
    const bool before = on_;
    if (on_ && control < p_.threshold - p_.hysteresis) on_ = false;
    else if (!on_ && control > p_.threshold + p_.hysteresis) on_ = true;
    return on_ != before;
}

void Switch::stamp(StampContext& ctx) {
    // A change of position during a Newton iteration requires another one.
    if (!ctx.frozenModes() && update(control_.eval(ctx.x, ctx.t))) ctx.out.limited = true;
    const double g = 1.0 / (on_ ? p_.ron : p_.roff);
    ctx.stampCurrent(a_, b_, g * (ctx.v(a_) - ctx.v(b_)));
    ctx.stampConductance(a_, b_, g);
}

// Distance of the control value to the threshold that changes the position.
void Switch::eventFunctions(double t, const Vector& x, double* g) const {
    g[0] = control_.eval(x, t) - (on_ ? p_.threshold - p_.hysteresis
                                      : p_.threshold + p_.hysteresis);
}

bool Switch::eventUpdate(double t, Vector& x, bool /*initial*/) {
    update(control_.eval(x, t));
    return false;
}

// --- IdealDiode ------------------------------------------------------------

IdealDiode::IdealDiode(std::string name, int anode, int cathode, const IdealDiodeParams& params)
    : Device(std::move(name)), a_(anode), c_(cathode), p_(params) {
    if (!(p_.ron > 0.0) || !(p_.roff > p_.ron))
        throw std::runtime_error(this->name() + ": need 0 < ron < roff");
}

void IdealDiode::stamp(StampContext& ctx) {
    const double v = ctx.v(a_) - ctx.v(c_);
    if (!ctx.frozenModes()) {
        const bool now = v > p_.vf;
        if (now != on_) {
            on_ = now;
            ctx.out.limited = true;
        }
    }
    const double goff = 1.0 / p_.roff, gon = 1.0 / p_.ron;
    const double i = goff * v + (on_ ? (gon - goff) * (v - p_.vf) : 0.0);
    ctx.stampCurrent(a_, c_, i);
    ctx.stampConductance(a_, c_, on_ ? gon : goff);
}

void IdealDiode::eventFunctions(double /*t*/, const Vector& x, double* g) const {
    g[0] = voltage(x) - p_.vf;
}

bool IdealDiode::eventUpdate(double /*t*/, Vector& x, bool /*initial*/) {
    on_ = voltage(x) > p_.vf;
    return false;
}

}  // namespace esim
