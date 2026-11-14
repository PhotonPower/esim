#include "circuit/devices/Bjt.hpp"

#include <cmath>
#include <stdexcept>

#include "circuit/Circuit.hpp"

namespace esim {

Bjt::Bjt(std::string name, int c, int b, int e, int polarity, const BjtParams& p)
    : Device(std::move(name)), c_(c), b_(b), e_(e), ci_(c), bi_(b), ei_(e),
      pol_(polarity >= 0 ? 1.0 : -1.0), p_(p) {
    if (!(p_.is > 0.0)) throw std::runtime_error(this->name() + ": is must be > 0");
    if (!(p_.bf > 0.0) || !(p_.br > 0.0))
        throw std::runtime_error(this->name() + ": bf and br must be > 0");
    if (p_.rb < 0.0 || p_.rc < 0.0 || p_.re < 0.0)
        throw std::runtime_error(this->name() + ": rb, rc and re must be >= 0");
    if (!(p_.ikf > 0.0) || !(p_.ikr > 0.0))
        throw std::runtime_error(this->name() + ": ikf and ikr must be > 0");
    if (p_.ise < 0.0 || p_.isc < 0.0 || !(p_.ne > 0.0) || !(p_.nc > 0.0))
        throw std::runtime_error(this->name() + ": ise, isc must be >= 0 and ne, nc > 0");
}

void Bjt::setup(Circuit& ckt) {
    const CircuitOptions& o = ckt.options;
    const double vt = o.vt();
    const double ratio = o.tempRatio();
    const double is = o.saturationCurrent(p_.is, 1.0, p_.eg, p_.xti);
    jbe_.configure(is, p_.nf * vt);
    jbc_.configure(is, p_.nr * vt);
    const double gainFactor = std::pow(ratio, p_.xtb);
    bf_ = p_.bf * gainFactor;
    br_ = p_.br * gainFactor;
    // Leakage currents scale like is^(1/n) and inversely to the current gain.
    ise_ = o.saturationCurrent(p_.ise, p_.ne, p_.eg, p_.xti) / gainFactor;
    isc_ = o.saturationCurrent(p_.isc, p_.nc, p_.eg, p_.xti) / gainFactor;
    nevt_ = p_.ne * vt;
    ncvt_ = p_.nc * vt;

    if (p_.rc > 0.0) ci_ = ckt.addUnknown("v(" + name() + ":c)", UnknownKind::Voltage, true);
    if (p_.rb > 0.0) bi_ = ckt.addUnknown("v(" + name() + ":b)", UnknownKind::Voltage, true);
    if (p_.re > 0.0) ei_ = ckt.addUnknown("v(" + name() + ":e)", UnknownKind::Voltage, true);
}

void Bjt::stamp(StampContext& ctx) {
    // All equations are written for an npn transistor; pol_ mirrors voltages
    // and currents for a pnp.
    const double vbe = pol_ * (ctx.v(bi_) - ctx.v(ei_));
    const double vbc = pol_ * (ctx.v(bi_) - ctx.v(ci_));
    const double vbeL = jbe_.limit(vbe, ctx.limiting, ctx.out.limited);
    const double vbcL = jbc_.limit(vbc, ctx.limiting, ctx.out.limited);

    double ifw, gif, irv, gir;
    jbe_.current(vbeL, ifw, gif);
    jbc_.current(vbcL, irv, gir);

    // Base charge qb: Early effect (q1) and high injection (q2).
    double q1 = 1.0 - vbcL / p_.vaf - vbeL / p_.var;
    double dq1Be, dq1Bc;
    if (q1 < 0.1) {  // far outside the validity of the model
        q1 = 10.0;
        dq1Be = dq1Bc = 0.0;
    } else {
        q1 = 1.0 / q1;
        dq1Be = q1 * q1 / p_.var;
        dq1Bc = q1 * q1 / p_.vaf;
    }
    const double q2 = ifw / p_.ikf + irv / p_.ikr;
    const double root = std::sqrt(std::max(1.0 + 4.0 * q2, 1e-12));
    const double qb = 0.5 * q1 * (1.0 + root);
    const double dqbBe = 0.5 * dq1Be * (1.0 + root) + q1 * (gif / p_.ikf) / root;
    const double dqbBc = 0.5 * dq1Bc * (1.0 + root) + q1 * (gir / p_.ikr) / root;

    // Transport current collector -> emitter.
    const double ict = (ifw - irv) / qb;
    const double gctBe = (gif - ict * dqbBe) / qb;
    const double gctBc = (-gir - ict * dqbBc) / qb;

    // Base current: ideal part, leakage (recombination) and gmin, per junction.
    double ibe = ifw / bf_ + ctx.gmin * vbeL, gbe = gif / bf_ + ctx.gmin;
    double ibc = irv / br_ + ctx.gmin * vbcL, gbc = gir / br_ + ctx.gmin;
    if (ise_ > 0.0) {
        double d;
        const double e = limexp(vbeL / nevt_, d);
        ibe += ise_ * (e - 1.0);
        gbe += ise_ * d / nevt_;
    }
    if (isc_ > 0.0) {
        double d;
        const double e = limexp(vbcL / ncvt_, d);
        ibc += isc_ * (e - 1.0);
        gbc += isc_ * d / ncvt_;
    }

    // Terminal currents into collector and base, with derivatives with
    // respect to vbe and vbc, extrapolated to the actual voltages.
    const double dbe = vbe - vbeL, dbc = vbc - vbcL;
    const double gcBe = gctBe, gcBc = gctBc - gbc;
    const double gbBe = gbe, gbBc = gbc;
    const double ic = ict - ibc + gcBe * dbe + gcBc * dbc;
    const double ib = ibe + ibc + gbBe * dbe + gbBc * dbc;

    ctx.addF(ci_, pol_ * ic);
    ctx.addF(bi_, pol_ * ib);
    ctx.addF(ei_, -pol_ * (ic + ib));

    // d/dv(b) = g_be + g_bc, d/dv(e) = -g_be, d/dv(c) = -g_bc (pol_^2 = 1)
    auto stampRow = [&](int row, double gBe, double gBc) {
        ctx.addG(row, bi_, gBe + gBc);
        ctx.addG(row, ei_, -gBe);
        ctx.addG(row, ci_, -gBc);
    };
    stampRow(ci_, gcBe, gcBc);
    stampRow(bi_, gbBe, gbBc);
    stampRow(ei_, -(gcBe + gbBe), -(gcBc + gbBc));

    // Charges: diffusion (transit time) plus depletion, per junction.
    double qbe, cbe, qbc, cbc;
    depletionCharge(vbeL, p_.cje, p_.vje, p_.mje, p_.fc, qbe, cbe);
    depletionCharge(vbcL, p_.cjc, p_.vjc, p_.mjc, p_.fc, qbc, cbc);
    qbe += p_.tf * ifw;
    cbe += p_.tf * gif;
    qbc += p_.tr * irv;
    cbc += p_.tr * gir;
    qbe += cbe * dbe;
    qbc += cbc * dbc;

    ctx.stampCharge(bi_, ei_, pol_ * qbe);
    ctx.stampCapacitance(bi_, ei_, cbe);
    ctx.stampCharge(bi_, ci_, pol_ * qbc);
    ctx.stampCapacitance(bi_, ci_, cbc);

    // Series resistances.
    auto resistor = [&](int a, int b, double r) {
        if (a == b) return;
        ctx.stampCurrent(a, b, (ctx.v(a) - ctx.v(b)) / r);
        ctx.stampConductance(a, b, 1.0 / r);
    };
    resistor(c_, ci_, p_.rc);
    resistor(b_, bi_, p_.rb);
    resistor(e_, ei_, p_.re);
}

}  // namespace esim
