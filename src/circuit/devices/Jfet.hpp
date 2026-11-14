#pragma once

#include "circuit/Device.hpp"
#include "circuit/Junction.hpp"

namespace esim {

struct JfetParams {
    double vto = -2.0;    // pinch-off (threshold) voltage [V]; negative for both polarities
    double beta = 1e-4;   // transconductance parameter [A/V^2]
    double lambda = 0.0;  // channel length modulation [1/V]
    double is = 1e-14;    // gate junction saturation current [A]
    double cgs = 0.0;     // zero-bias gate-source capacitance [F]
    double cgd = 0.0;     // zero-bias gate-drain capacitance [F]
    double pb = 1.0;      // gate junction potential [V]
    double fc = 0.5;
    double kf = 0.0;      // flicker noise coefficient
    double af = 1.0;      // flicker noise exponent
};

// Junction field-effect transistor, Shichman-Hodges model (SPICE JFET):
//   id = 0                                          vgs <= vto
//   id = beta (vgs - vto)^2 (1 + lambda vds)        saturation, vds >= vgs - vto
//   id = beta vds (2 (vgs - vto) - vds) (1 + lambda vds)   otherwise
// with pn junctions from the gate to source and drain (current and depletion
// charge). Drain and source swap their roles for vds < 0.
class Jfet final : public Device {
public:
    // polarity: +1 n-channel, -1 p-channel
    Jfet(std::string name, int d, int g, int s, int polarity, const JfetParams& p);
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void noiseSources(std::vector<NoiseSource>& out) const override;

private:
    enum { kD = 0, kG = 1, kS = 2 };
    int d_, g_, s_;
    double pol_;
    JfetParams p_;
    PnJunction jgs_, jgd_;
    bool hasCharge_;
    double kT_ = 0.0;
    double opId_ = 0.0, opGm_ = 0.0, opGds_ = 0.0, opIg_ = 0.0;
};

}  // namespace esim
