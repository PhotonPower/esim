#pragma once

#include <map>
#include <string>
#include <vector>

#include "circuit/Device.hpp"
#include "circuit/Dual.hpp"
#include "circuit/Junction.hpp"

namespace esim {

// MOSFET with the drain current of BSIM3v3.3 (the Berkeley short-channel
// model, SPICE levels 8 / 49 / 53), written from the model equations as
// published by the BSIM group of the University of California, Berkeley.
//
// Covered, with the parameter names of the model:
//   threshold    vth0 k1 k2 k3 k3b w0 nlx dvt0 dvt1 dvt2 dvt0w dvt1w dvt2w
//                eta0 etab dsub (short and narrow channel, DIBL), nch nsub
//                gamma1 gamma2 vbx vbm xt, vfb
//   subthreshold voff nfactor cdsc cdscb cdscd cit
//   mobility     u0 ua ub uc, mobmod 1 to 3
//   saturation   vsat a0 ags a1 a2 b0 b1 keta delta
//   resistance   rdsw prwg prwb wr
//   output       pclm pdiblc1 pdiblc2 pdiblcb drout pscbe1 pscbe2 pvag
//   geometry     lint wint ll lln lw lwn lwl wl wln ww wwn wwl xl xw dwg dwb,
//                and the l / w / p variants of all of the above (binning)
//   gate         tox toxm ngate (poly depletion)
//   substrate    alpha0 alpha1 beta0 (impact ionization current)
//   temperature  tnom ute kt1 kt1l kt2 ua1 ub1 uc1 at prt
//   junctions    js jsw nj xti cj cjsw cjswg mj mjsw mjswg pb pbsw pbswg
//   charges      capmod 0 to 3 (default 3), xpart (0: 40/60, 0.5: 50/50,
//                1: 0/100 partition, < 0: no intrinsic charges), vfbcv clc cle
//                noff voffcv acde moin, dlc dwc llc lwc lwlc wlc wwc wwlc
//   overlap      cgso cgdo cgbo cf, and bias dependent with capmod 1 to 3:
//                cgsl cgdl ckappa (defaults from xj, dlc, dwc as in the model)
//   instance     w l ad as pd ps m
//
// The charges are those of BSIM3v3.2 and later: capMod 1 and 2 take the
// flat-band voltage vfbzb that follows from vth0, whatever `version` says.
// Absent: non-quasi-static effects (nqsmod, elm), the BSIM3 noise models (the
// level 1 formulas are used), the acm geometry options, and the selection
// among several model cards by lmin / lmax / wmin / wmax.
class Bsim3 final : public Device {
public:
    // polarity: +1 nmos, -1 pmos. params: model and instance parameters by
    // their lower-case names.
    Bsim3(std::string name, int d, int g, int s, int b, int polarity,
          std::map<std::string, double> params);

    // Names in `params` that are neither used nor known as harmless.
    std::vector<std::string> unknownParameters() const;

    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void noiseSources(std::vector<NoiseSource>& out) const override;

    // Quantities of the drain current calculation that the charges use.
    struct Bias {
        Dual vbseff, phis, sqrtPhis, vth, vgsEff, vgst, n, abulk0;
    };
    // Drain current and substrate current for vds >= 0 in the n-channel frame.
    Dual drainCurrent(const Dual& vgs, const Dual& vds, const Dual& vbs, Dual& isub, Bias& bias) const;

    // Intrinsic charges of the channel for vds >= 0 in the n-channel frame,
    // for one transistor (without the multiplier m), from the drain current's
    // intermediate quantities at the same bias.
    struct Charges {
        Dual g, d, s, b;
    };
    Charges intrinsicCharges(const Dual& vds, const Dual& vbs, const Bias& bias) const;

    // Size- and temperature-dependent quantities, see prepare().
    struct Prepared {
        double leff, weff, cox, vtm, tempRatio;
        double phi, sqrtPhi, phis3, xdep0, litl, vbi, cdep0;
        double k1, k2, k1ox, k2ox, vbsc, vfb, vth0;
        double theta0vb0, thetaRout, factor1;
        double u0temp, vsattemp, rds0, ua, ub, uc;
        // binned model parameters
        double cdsc, cdscb, cdscd, cit, nfactor, xj, a0, ags, a1, a2, keta, ngate;
        double kt1, kt1l, kt2, k3, k3b, w0, nlx, dvt0, dvt1, dvt2, dvt0w, dvt1w, dvt2w;
        double voff, delta, prwg, prwb, eta0, etab, pclm, pdiblb, pscbe1, pscbe2, pvag;
        double dwg, dwb, b0, b1, alpha0, alpha1, beta0;
        int mobMod;
        double tox;
        // charges
        int capMod;
        double xpart, leffCV, weffCV, coxWL, abulkCVfactor, vfbzb, vfbcv;
        double noff, voffcv, ldeb, acde, moin, cgsl, cgdl, ckappa;
    };
    const Prepared& prepared() const { return p_; }

private:
    enum { kD = 0, kG = 1, kS = 2, kB = 3 };
    struct Local {
        double f[4] = {}, q[4] = {};
        double g[4][4] = {}, c[4][4] = {};
    };
    bool given(const std::string& key) const { return raw_.count(key) != 0; }
    double value(const std::string& key, double fallback) const;
    void prepare(double temp, double tnom);
    void addJunction(StampContext& ctx, Local& m, PnJunction& j, int to, const double cz[3]);
    Dual overlapCharge(const Dual& v, double cgo, double cgl) const;

    int d_, g_, s_, b_;
    double pol_;
    std::map<std::string, double> raw_;
    mutable std::map<std::string, bool> used_;
    Prepared p_{};
    double mult_ = 1.0;           // instance multiplier m
    double cgso_ = 0.0, cgdo_ = 0.0, cgbo_ = 0.0;  // overlap capacitances of one transistor [F]
    double czs_[3] = {}, czd_[3] = {};             // junction capacitances: bottom, sidewall, gate side
    double pbJ_[3] = {1.0, 1.0, 1.0}, mjJ_[3] = {0.5, 0.33, 0.33};
    PnJunction jbs_, jbd_;
    double kT_ = 0.0, kf_ = 0.0, af_ = 1.0;
    double opId_ = 0.0, opGm_ = 0.0, opGds_ = 0.0;
};

}  // namespace esim
