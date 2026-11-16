#pragma once

#include "circuit/Device.hpp"
#include "circuit/Junction.hpp"

namespace esim {

struct CircuitOptions;

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
    bool ekv = false;     // charge-based model valid from weak to strong inversion
    double theta = 0.0;   // ekv: mobility reduction by the gate field [1/V]
    double ucrit = 0.0;   // ekv: critical field of velocity saturation [V/m]; 0: none
    // level 3 (SPICE MOS3, semi-empirical short-channel model); theta acts as well
    int level = 1;        // 1 or 3
    double eta = 0.0;     // static feedback: drain-induced lowering of the threshold
    double vmax = 0.0;    // maximum drift velocity of the carriers [m/s]; 0: none
    double nfs = 0.0;     // fast surface state density [1/cm^2]; > 0: weak inversion current
    double kappa = 0.2;   // saturation field factor (channel length modulation, with nsub)
    double xj = 0.0;      // junction depth [m] (short channel effect, with nsub)
    double ld = 0.0;      // lateral diffusion [m]: effective length l - 2 ld
    double delta = 0.0;   // width effect on the threshold
    double u0 = 600.0;    // surface mobility [cm^2/Vs]
    double nsub = 0.0;    // substrate doping [1/cm^3]; 0: not given
    double nss = 0.0;     // surface state density [1/cm^2] (threshold from nsub)
    double tpg = 1.0;     // gate material (threshold from nsub)
    bool kpGiven = true, vtoGiven = true, gammaGiven = true, phiGiven = true;
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
//
// With "level": "ekv" the channel follows the charge-based EKV model instead.
// All voltages refer to the bulk, the device is symmetric in drain and source,
// and one smooth expression covers weak, moderate and strong inversion:
//     vp  = vg' - phi - gamma (sqrt(vg' + (gamma/2)^2) - gamma/2)    pinch-off voltage
//     n   = 1 + gamma / (2 sqrt(vp + phi + 4 vt))                    slope factor
//     if  = ln^2(1 + exp((vp - vs) / (2 vt))),  ir likewise with vd  forward, reverse current
//     id  = 2 n beta vt^2 (if - ir)
// with vg' = vg - vto + phi + gamma sqrt(phi). Below threshold the current
// falls exponentially (n vt ln 10 per decade), far above it follows the square
// law beta/(2 n) (vg - vto - n vs)^2. On top of this:
//     theta   beta / (1 + theta * mean overdrive of the channel)     mobility reduction
//     ucrit   id / (1 + vds_eff / (ucrit L))                         velocity saturation
//     lambda  id (1 + lambda |vds|)                                  channel length modulation
// The charges are those of the same model (they add up to zero), so the
// capacitances are continuous through all regions as well.
//
// With "level": 3 the drain current follows SPICE MOS3 as in ngspice
// (mos3load.c): threshold with body, short-channel (xj), narrow-width (delta)
// and static feedback (eta) effects, mobility reduction (theta), velocity
// saturation (vmax), channel length modulation (kappa, with nsub) and weak
// inversion (nfs). Parameters given as in SPICE: u0 in cm^2/Vs, nsub in
// 1/cm^3, nfs in 1/cm^2; kp follows from u0 and tox if missing, and tox is
// 1e-7 m if missing. Temperature as ngspice (kp, u0, phi, built-in voltage).
// The gate charge is the charge-conserving one of level 1 with the threshold
// of level 3 (ngspice uses Meyer's capacitances instead).
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
    void stampLevel1(StampContext& ctx, Local& m);
    void stampEkv(StampContext& ctx, Local& m);
    void stampLevel3(StampContext& ctx, Local& m);
    void setupLevel3(const CircuitOptions& o);

    int d_, g_, s_, b_;
    double pol_;
    MosfetParams p_;
    double beta_, vto_;  // vto_ normalized to the n-channel equations
    double cox_ = 0.0;   // gate oxide capacitance cox * W * L [F]
    double vt_ = 0.0;    // thermal voltage
    bool hasCharge_ = false;  // any capacitance at all
    // level 3, after setup: effective size, oxide capacitance per area,
    // and the parameters at the temperature of the circuit
    double leff_ = 0.0, weff_ = 0.0, coxArea_ = 0.0;
    double gamma3_ = 0.0, phi3_ = 0.0, vbi3_ = 0.0, u0t_ = 0.0;
    double eta3_ = 0.0, alpha3_ = 0.0, depletion3_ = 0.0, narrow3_ = 0.0, csonco3_ = 0.0;
    PnJunction jbd_, jbs_;
};

}  // namespace esim
