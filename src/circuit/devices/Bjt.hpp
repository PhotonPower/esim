#pragma once

#include <limits>

#include "circuit/Device.hpp"
#include "circuit/Junction.hpp"

namespace esim {

struct BjtParams {
    double is = 1e-16;  // transport saturation current [A]
    double bf = 100.0;  // forward current gain
    double br = 1.0;    // reverse current gain
    double nf = 1.0;    // forward emission coefficient
    double nr = 1.0;    // reverse emission coefficient
    double vaf = std::numeric_limits<double>::infinity();  // forward Early voltage [V]
    double var = std::numeric_limits<double>::infinity();  // reverse Early voltage [V]
    double ikf = std::numeric_limits<double>::infinity();  // forward knee current (high injection) [A]
    double ikr = std::numeric_limits<double>::infinity();  // reverse knee current [A]
    double ise = 0.0, ne = 1.5;  // base-emitter leakage: saturation current, emission coefficient
    double isc = 0.0, nc = 2.0;  // base-collector leakage
    double rb = 0.0, rc = 0.0, re = 0.0;        // series resistances [Ohm]
    double cje = 0.0, vje = 0.75, mje = 0.33;  // base-emitter depletion capacitance
    double cjc = 0.0, vjc = 0.75, mjc = 0.33;  // base-collector depletion capacitance
    double tf = 0.0;    // forward transit time [s]
    double tr = 0.0;    // reverse transit time [s]
    double fc = 0.5;
    double eg = 1.11;   // energy gap [eV]            (temperature dependence of is)
    double xti = 3.0;   // temperature exponent of is
    double xtb = 0.0;   // temperature exponent of bf and br
};

// Bipolar transistor, Gummel-Poon model:
//   transport current with Early effect and high-injection roll-off
//       ict = (if - ir) / qb,   qb = q1 (1 + sqrt(1 + 4 q2)) / 2,
//       q1 = 1 / (1 - vbc/vaf - vbe/var),   q2 = if/ikf + ir/ikr
//   base current  if/bf + ir/br  plus the leakage (recombination) currents
//   series resistances rb, rc, re (internal nodes), junction and diffusion charges,
//   temperature dependence of is, ise, isc, bf, br.
// Not modelled: substrate junction, current-dependent base resistance, the
// bias dependence of the transit time.
class Bjt final : public Device {
public:
    // polarity: +1 npn, -1 pnp
    Bjt(std::string name, int c, int b, int e, int polarity, const BjtParams& p);
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;

private:
    int c_, b_, e_;     // terminals
    int ci_, bi_, ei_;  // nodes of the intrinsic transistor (behind the series resistances)
    double pol_;
    BjtParams p_;
    double bf_ = 100.0, br_ = 1.0, ise_ = 0.0, isc_ = 0.0;  // at the circuit temperature
    double nevt_ = 0.0, ncvt_ = 0.0;
    PnJunction jbe_, jbc_;
};

}  // namespace esim
