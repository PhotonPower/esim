#include "circuit/devices/Sources.hpp"

#include <cmath>
#include <stdexcept>

#include "circuit/Circuit.hpp"

namespace esim {

namespace {

int controlBranch(const Circuit& ckt, const std::string& self, const std::string& control) {
    const Device* d = ckt.findDevice(control);
    if (!d) throw std::runtime_error(self + ": controlling component '" + control + "' not found");
    if (d->branch() < 0)
        throw std::runtime_error(self + ": controlling component '" + control +
                                 "' has no branch current (use a voltage source or inductor)");
    return d->branch();
}

}  // namespace

// --- IndependentSource -----------------------------------------------------

void IndependentSource::setAc(double magnitude, double phaseDeg) {
    ac_ = std::polar(magnitude, phaseDeg * kPi / 180.0);
}

double IndependentSource::nextBreakpoint(double t) const {
    return wave_ ? wave_->nextBreakpoint(t) : Device::nextBreakpoint(t);
}

double IndependentSource::value(const StampContext& ctx) const {
    double v;
    if (ctx.transient && wave_) {
        v = wave_->value(ctx.t);
    } else if (dc_.has) {
        v = dc_.value;
    } else {
        v = wave_ ? wave_->value(0.0) : 0.0;
    }
    return v * ctx.srcScale;
}

double IndependentSource::startValue() const {
    return wave_ ? wave_->value(0.0) : (dc_.has ? dc_.value : 0.0);
}

// --- VoltageSource ---------------------------------------------------------


void VoltageSource::setup(Circuit& ckt) {
    br_ = ckt.addUnknown("i(" + name() + ")", UnknownKind::Current, false);
}

void VoltageSource::stamp(StampContext& ctx) {
    ctx.stampBranch(p_, n_, br_);
    ctx.addF(br_, -value(ctx));
}

void VoltageSource::stampAc(Eigen::VectorXcd& rhs) const { rhs[br_] += ac(); }

// --- CurrentSource ---------------------------------------------------------

void CurrentSource::stamp(StampContext& ctx) { ctx.stampCurrent(p_, n_, value(ctx)); }

void CurrentSource::stampAc(Eigen::VectorXcd& rhs) const {
    if (p_ >= 0) rhs[p_] -= ac();
    if (n_ >= 0) rhs[n_] += ac();
}

// --- Vcvs ------------------------------------------------------------------

Vcvs::Vcvs(std::string name, int p, int n, int cp, int cn, double gain)
    : Device(std::move(name)), p_(p), n_(n), cp_(cp), cn_(cn), gain_(gain) {}

void Vcvs::setup(Circuit& ckt) {
    br_ = ckt.addUnknown("i(" + name() + ")", UnknownKind::Current, false);
}

void Vcvs::stamp(StampContext& ctx) {
    ctx.stampBranch(p_, n_, br_);
    ctx.addF(br_, -gain_ * (ctx.v(cp_) - ctx.v(cn_)));
    ctx.addG(br_, cp_, -gain_);
    ctx.addG(br_, cn_, gain_);
}

// --- Vccs ------------------------------------------------------------------

Vccs::Vccs(std::string name, int p, int n, int cp, int cn, double gm)
    : Device(std::move(name)), p_(p), n_(n), cp_(cp), cn_(cn), gm_(gm) {}

void Vccs::stamp(StampContext& ctx) {
    ctx.stampCurrent(p_, n_, gm_ * (ctx.v(cp_) - ctx.v(cn_)));
    ctx.addG(p_, cp_, gm_);
    ctx.addG(p_, cn_, -gm_);
    ctx.addG(n_, cp_, -gm_);
    ctx.addG(n_, cn_, gm_);
}

// --- Cccs ------------------------------------------------------------------

Cccs::Cccs(std::string name, int p, int n, std::string control, double gain)
    : Device(std::move(name)), p_(p), n_(n), control_(std::move(control)), gain_(gain) {}

void Cccs::bind(Circuit& ckt) { cbr_ = controlBranch(ckt, name(), control_); }

void Cccs::stamp(StampContext& ctx) {
    ctx.stampCurrent(p_, n_, gain_ * ctx.x[cbr_]);
    ctx.addG(p_, cbr_, gain_);
    ctx.addG(n_, cbr_, -gain_);
}

// --- Ccvs ------------------------------------------------------------------

Ccvs::Ccvs(std::string name, int p, int n, std::string control, double r)
    : Device(std::move(name)), p_(p), n_(n), control_(std::move(control)), r_(r) {}

void Ccvs::setup(Circuit& ckt) {
    br_ = ckt.addUnknown("i(" + name() + ")", UnknownKind::Current, false);
}

void Ccvs::bind(Circuit& ckt) { cbr_ = controlBranch(ckt, name(), control_); }

void Ccvs::stamp(StampContext& ctx) {
    ctx.stampBranch(p_, n_, br_);
    ctx.addF(br_, -r_ * ctx.x[cbr_]);
    ctx.addG(br_, cbr_, -r_);
}

}  // namespace esim
