#pragma once

#include <cmath>

#include "core/DaeSystem.hpp"

namespace esim {

// exp(x) that continues linearly above x = 80, so that a bad Newton iterate
// cannot overflow. d receives the derivative.
inline double limexp(double x, double& d) {
    constexpr double xmax = 80.0;
    if (x < xmax) {
        d = std::exp(x);
        return d;
    }
    static const double emax = std::exp(xmax);
    d = emax;
    return emax * (1.0 + (x - xmax));
}

// SPICE junction voltage limiting: restricts the change of a junction voltage
// between two Newton iterations so that the exponential stays tame.
inline double pnjlim(double vnew, double vold, double vt, double vcrit, bool& limited) {
    if (vnew > vcrit && std::abs(vnew - vold) > 2.0 * vt) {
        if (vold > 0.0) {
            const double arg = 1.0 + (vnew - vold) / vt;
            vnew = arg > 0.0 ? vold + vt * std::log(arg) : vcrit;
        } else {
            vnew = vt * std::log(vnew / vt);
        }
        limited = true;
    }
    return vnew;
}

inline double junctionVcrit(double is, double nvt) {
    return nvt * std::log(nvt / (1.4142135623730951 * is));
}

// Depletion charge and capacitance of a pn junction (SPICE formulation with
// linear continuation of the capacitance above fc * vj).
inline void depletionCharge(double v, double cj0, double vj, double m, double fc, double& q,
                            double& c) {
    if (cj0 <= 0.0) {
        q = 0.0;
        c = 0.0;
        return;
    }
    if (v < fc * vj) {
        const double arg = 1.0 - v / vj;
        const double sarg = std::pow(arg, -m);
        q = cj0 * vj * (1.0 - arg * sarg) / (1.0 - m);
        c = cj0 * sarg;
    } else {
        const double f1 = vj * (1.0 - std::pow(1.0 - fc, 1.0 - m)) / (1.0 - m);
        const double f2 = std::pow(1.0 - fc, 1.0 + m);
        const double f3 = 1.0 - fc * (1.0 + m);
        q = cj0 * (f1 + (f3 * (v - fc * vj) + (m / (2.0 * vj)) * (v * v - fc * fc * vj * vj)) / f2);
        c = cj0 * (f3 + m * v / vj) / f2;
    }
}

// Ideal exponential junction i = is * (exp(v / nvt) - 1) with limiter memory.
//
// Usage inside Device::stamp():
//     vl = j.limit(v, ctx.limiting, ctx.out.limited);
//     j.current(vl, i, g);
//     i += g * (v - vl);        // linear extrapolation back to the real voltage
class PnJunction {
public:
    void configure(double is, double nvt) {
        is_ = is;
        nvt_ = nvt;
        vcrit_ = junctionVcrit(is, nvt);
    }

    double limit(double v, LimitMode mode, bool& limited) {
        double vl = v;
        if (mode == LimitMode::On) vl = pnjlim(v, vold_, nvt_, vcrit_, limited);
        if (mode != LimitMode::Off) vold_ = vl;
        return vl;
    }

    void current(double v, double& i, double& g) const {
        double d;
        const double e = limexp(v / nvt_, d);
        i = is_ * (e - 1.0);
        g = is_ * d / nvt_;
    }

    double nvt() const { return nvt_; }

private:
    double is_ = 1e-14, nvt_ = 0.025865, vcrit_ = 0.6, vold_ = 0.0;
};

}  // namespace esim
