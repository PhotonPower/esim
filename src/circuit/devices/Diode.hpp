#pragma once

#include <limits>

#include "circuit/Device.hpp"

namespace esim {

struct DiodeParams {
    double is = 1e-14;   // saturation current [A]
    double n = 1.0;      // emission coefficient
    double rs = 0.0;     // series resistance [Ohm]
    double cj0 = 0.0;    // zero-bias junction capacitance [F]
    double vj = 1.0;     // junction potential [V]
    double m = 0.5;      // grading coefficient
    double fc = 0.5;     // forward-bias depletion capacitance coefficient
    double tt = 0.0;     // transit time [s]
    double bv = std::numeric_limits<double>::infinity();  // reverse breakdown voltage [V]
    double ibv = 1e-3;   // current at breakdown voltage [A]
    double eg = 1.11;    // energy gap [eV]           (temperature dependence of is)
    double xti = 3.0;    // temperature exponent of is
    double kf = 0.0;     // flicker noise coefficient
    double af = 1.0;     // flicker noise exponent
};

// pn diode:
//   i = is (exp(v / (n vt)) - 1) - ibv exp(-(v + bv) / (n vt)) + gmin v
//   q = tt * i_forward + depletion charge
class Diode final : public Device {
public:
    Diode(std::string name, int anode, int cathode, const DiodeParams& p);
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void noiseSources(std::vector<NoiseSource>& out) const override;

private:
    int a_, c_, ai_;  // ai_: internal anode behind the series resistance
    double kT_ = 0.0;
    double opCurrent_ = 0.0;  // junction current of the last evaluation
    DiodeParams p_;
    bool hasBv_;
    double is_ = 0.0;  // saturation current at the circuit temperature
    double nvt_ = 0.0, vcrit_ = 0.0, vcritBv_ = 0.0;
    double vold_ = 0.0;  // limiter memory
};

}  // namespace esim
