#pragma once

#include "circuit/Device.hpp"
#include "circuit/Junction.hpp"

namespace esim {

struct MosfetParams {
    double vto = 0.0;     // threshold voltage [V] (negative for enhancement pmos)
    double kp = 2e-5;     // transconductance parameter [A/V^2]
    double gamma = 0.0;   // body effect [sqrt(V)]
    double phi = 0.6;     // surface potential [V]
    double lambda = 0.0;  // channel length modulation [1/V]
    double w = 1e-4;      // channel width [m]
    double l = 1e-4;      // channel length [m]
    double cgso = 0.0;    // gate-source overlap capacitance per width [F/m]
    double cgdo = 0.0;    // gate-drain overlap capacitance per width [F/m]
    double cgbo = 0.0;    // gate-bulk overlap capacitance per length [F/m]
    double cbd = 0.0;     // zero-bias bulk-drain junction capacitance [F]
    double cbs = 0.0;     // zero-bias bulk-source junction capacitance [F]
    double pb = 0.8;      // bulk junction potential [V]
    double mj = 0.5;      // bulk junction grading coefficient
    double fc = 0.5;
    double is = 1e-14;    // bulk junction saturation current [A]
    double tox = 0.0;     // gate oxide thickness [m]; 0: no intrinsic gate charge
    double tcv = 0.0;     // temperature coefficient of the threshold voltage [V/K]
    double kf = 0.0;      // flicker noise coefficient
    double af = 1.0;      // flicker noise exponent
};

// MOSFET, SPICE level 1 (Shichman-Hodges) with body effect, channel length
// modulation, bulk diodes, overlap and bulk junction capacitances.
//
// With tox given, the charge of the channel is included. It is the charge of
// the long-channel model, split between drain and source 40 : 60 in saturation
// (Ward-Dutton), written with a = vgs - vth and b = max(a - vds, 0):
//     qi = -(2/3) K (a^2 + a b + b^2) / (a + b),          K = cox W L
//     qd = -(2/15) K (2 a^3 + 4 a^2 b + 6 a b^2 + 3 b^3) / (a + b)^2
//     qs = qi - qd,   qg = -qi
// Below threshold the gate charges the bulk, qg = K a = -qb. The charges add up
// to zero in every region (charge conservation); the charge of the depletion
// layer under the channel is neglected.
// The mobility falls with temperature: kp ~ T^-1.5.
class Mosfet final : public Device {
public:
    // polarity: +1 nmos, -1 pmos
    Mosfet(std::string name, int d, int g, int s, int b, int polarity, const MosfetParams& p);
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void noiseSources(std::vector<NoiseSource>& out) const override;

private:
    double kT_ = 0.0;
    double opId_ = 0.0, opGm_ = 0.0, opGds_ = 0.0;  // channel at the last evaluation
    // Positions of the terminals in the local block of the device.
    enum { kD = 0, kG = 1, kS = 2, kB = 3 };
    struct Local {
        double f[4] = {}, q[4] = {};
        double g[4][4] = {}, c[4][4] = {};
    };
    void addBulkJunction(StampContext& ctx, Local& m, PnJunction& j, int to, double cj0);
    void addLinearCap(StampContext& ctx, Local& m, int a, int b, double c) const;
    void addChannelCharge(Local& m, int D, int S, double a, double vds, double daDvbs) const;

    int d_, g_, s_, b_;
    double pol_;
    MosfetParams p_;
    double beta_, vto_;  // vto_ normalized to the n-channel equations
    double cox_ = 0.0;   // gate oxide capacitance cox * W * L [F]
    bool hasCharge_ = false;  // any capacitance at all
    PnJunction jbd_, jbs_;
};

}  // namespace esim
