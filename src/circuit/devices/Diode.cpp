#include "circuit/devices/Diode.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "circuit/Circuit.hpp"
#include "circuit/Junction.hpp"

namespace esim {

Diode::Diode(std::string name, int anode, int cathode, const DiodeParams& p)
    : Device(std::move(name)), a_(anode), c_(cathode), ai_(anode), p_(p),
      hasBv_(std::isfinite(p.bv)) {
    if (!(p_.is > 0.0)) throw std::runtime_error(this->name() + ": is must be > 0");
    if (!(p_.n > 0.0)) throw std::runtime_error(this->name() + ": n must be > 0");
    if (p_.rs < 0.0) throw std::runtime_error(this->name() + ": rs must be >= 0");
}

void Diode::setup(Circuit& ckt) {
    nvt_ = p_.n * ckt.options.vt();
    is_ = ckt.options.saturationCurrent(p_.is, p_.n, p_.eg, p_.xti);
    vcrit_ = junctionVcrit(is_, nvt_);
    vcritBv_ = junctionVcrit(p_.ibv, nvt_);
    if (p_.rs > 0.0) ai_ = ckt.addUnknown("v(" + name() + ":a)", UnknownKind::Voltage, true);
}

void Diode::stamp(StampContext& ctx) {
    const double vd = ctx.v(ai_) - ctx.v(c_);

    // Junction voltage limiting, in forward direction and around breakdown.
    double vl = vd;
    if (ctx.limiting == LimitMode::On) {
        if (hasBv_ && vd < std::min(0.0, -p_.bv + 10.0 * nvt_)) {
            const double vr = pnjlim(-(vd + p_.bv), -(vold_ + p_.bv), nvt_, vcritBv_,
                                     ctx.out.limited);
            vl = -(vr + p_.bv);
        } else {
            vl = pnjlim(vd, vold_, nvt_, vcrit_, ctx.out.limited);
        }
    }
    if (ctx.limiting != LimitMode::Off) vold_ = vl;

    double de;
    const double e = limexp(vl / nvt_, de);
    const double ifwd = is_ * (e - 1.0);
    const double gfwd = is_ * de / nvt_;

    double i = ifwd + ctx.gmin * vl;
    double g = gfwd + ctx.gmin;
    if (hasBv_) {
        double deb;
        const double eb = limexp(-(vl + p_.bv) / nvt_, deb);
        i -= p_.ibv * eb;
        g += p_.ibv * deb / nvt_;
    }

    double q, c;
    depletionCharge(vl, p_.cj0, p_.vj, p_.m, p_.fc, q, c);
    q += p_.tt * ifwd;
    c += p_.tt * gfwd;

    // Linear extrapolation from the limited to the actual junction voltage.
    i += g * (vd - vl);
    q += c * (vd - vl);

    ctx.stampCurrent(ai_, c_, i);
    ctx.stampConductance(ai_, c_, g);
    ctx.stampCharge(ai_, c_, q);
    ctx.stampCapacitance(ai_, c_, c);

    if (ai_ != a_) {
        const double gs = 1.0 / p_.rs;
        ctx.stampCurrent(a_, ai_, gs * (ctx.v(a_) - ctx.v(ai_)));
        ctx.stampConductance(a_, ai_, gs);
    }
}

}  // namespace esim
