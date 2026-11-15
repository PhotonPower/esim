#include "circuit/devices/Bsim3.hpp"

#include <cmath>
#include <set>
#include <stdexcept>

#include "circuit/Circuit.hpp"

namespace esim {

namespace {

// Constants of the model, with the values the model itself uses.
constexpr double kEpsOx = 3.453133e-11;
constexpr double kEpsSi = 1.03594e-10;
constexpr double kKboQ = 8.617087e-5;
constexpr double kCharge = 1.60219e-19;
constexpr double kMaxExp = 5.834617425e14;
constexpr double kMinExp = 1.713908431e-15;
constexpr double kExpThreshold = 34.0;
constexpr double kPiB = 3.141592654;

// Parameters of BSIM3 that are accepted without effect here: options, the
// capacitance and noise models that are not implemented, limits of model bins.
const std::set<std::string>& harmless() {
    static const std::set<std::string> names = {
        "capmod", "noimod", "version", "paramchk", "lmin", "lmax", "wmin", "wmax", "tcj",
        "tcjsw", "tcjswg", "tpb", "tpbsw", "tpbswg", "elm", "cgsl", "cgdl", "ckappa", "clc",
        "cle", "noff", "voffcv", "acde", "moin", "llc", "lwc", "lwlc", "wlc", "wwc", "wwlc",
        "rsh", "hdif", "nqsmod", "acnqsmod", "xpart", "noia", "noib", "noic", "em", "ef",
        "lref", "wref", "acm", "vfbcv", "ijth", "wmlt", "lmlt", "nrd", "nrs", "geo", "level",
        "lcgsl", "wcgsl", "pcgsl", "lcgdl", "wcgdl", "pcgdl", "lckappa", "wckappa", "pckappa",
        "lcf", "wcf", "pcf", "lclc", "wclc", "pclc", "lcle", "wcle", "pcle", "lvfbcv", "wvfbcv",
        "pvfbcv", "lacde", "wacde", "pacde", "lmoin", "wmoin", "pmoin", "lnoff", "wnoff",
        "pnoff", "lvoffcv", "wvoffcv", "pvoffcv", "lelm", "welm", "pelm", "tox1", "dtoxcv"};
    return names;
}

// Other names for the same parameter.
std::string canonical(std::string key) {
    static const std::map<std::string, std::string> alias = {
        {"vtho", "vth0"}, {"nch", "npeak"}, {"pdiblc1", "pdibl1"}, {"pdiblc2", "pdibl2"},
        {"pdiblcb", "pdiblb"}, {"cjo", "cj"}};
    for (const char* prefix : {"", "l", "w", "p"}) {
        const std::string p = prefix;
        if (key.compare(0, p.size(), p) != 0) continue;
        auto it = alias.find(key.substr(p.size()));
        if (it != alias.end()) return p + it->second;
    }
    return key;
}

}  // namespace

Bsim3::Bsim3(std::string name, int d, int g, int s, int b, int polarity,
             std::map<std::string, double> params)
    : Device(std::move(name)), d_(d), g_(g), s_(s), b_(b), pol_(polarity >= 0 ? 1.0 : -1.0) {
    for (const auto& [key, v] : params) raw_[canonical(key)] = v;
}

double Bsim3::value(const std::string& key, double fallback) const {
    used_[key] = true;
    auto it = raw_.find(key);
    return it == raw_.end() ? fallback : it->second;
}

std::vector<std::string> Bsim3::unknownParameters() const {
    std::vector<std::string> out;
    for (const auto& [key, v] : raw_)
        if (!used_.count(key) && !harmless().count(key)) out.push_back(key);
    return out;
}

void Bsim3::setup(Circuit& ckt) {
    prepare(ckt.options.temp + 273.15, value("tnom", ckt.options.tnom) + 273.15);
}

// Everything that depends on the size of the transistor and the temperature
// but not on its voltages.
void Bsim3::prepare(double Temp, double Tnom) {
    Prepared& p = p_;
    const std::string who = name() + ": ";
    const double TRatio = Temp / Tnom, dT = TRatio - 1.0;
    p.tempRatio = dT;
    p.tox = value("tox", 150.0e-10);
    if (!(p.tox > 0.0)) throw std::runtime_error(who + "tox must be > 0");
    const double toxm = value("toxm", p.tox);
    p.cox = kEpsOx / p.tox;
    p.factor1 = std::sqrt(kEpsSi / kEpsOx * p.tox);
    p.mobMod = static_cast<int>(value("mobmod", 1));
    mult_ = value("m", 1.0);

    const double Vtm0 = kKboQ * Tnom;
    const double Eg0 = 1.16 - 7.02e-4 * Tnom * Tnom / (Tnom + 1108.0);
    const double ni = 1.45e10 * (Tnom / 300.15) * std::sqrt(Tnom / 300.15) * std::exp(21.5565981 - Eg0 / (2.0 * Vtm0));
    p.vtm = kKboQ * Temp;
    const double Eg = 1.16 - 7.02e-4 * Temp * Temp / (Temp + 1108.0);

    // Effective channel length and width.
    const double L = value("l", 5.0e-6), W = value("w", 5.0e-6);
    const double t0 = std::pow(L, value("lln", 1.0)), t1 = std::pow(W, value("lwn", 1.0));
    const double dl = value("lint", 0.0) + value("ll", 0.0) / t0 + value("lw", 0.0) / t1 + value("lwl", 0.0) / (t0 * t1);
    const double t2 = std::pow(L, value("wln", 1.0)), t3 = std::pow(W, value("wwn", 1.0));
    const double dw = value("wint", 0.0) + value("wl", 0.0) / t2 + value("ww", 0.0) / t3 + value("wwl", 0.0) / (t2 * t3);
    p.leff = L + value("xl", 0.0) - 2.0 * dl;
    p.weff = W + value("xw", 0.0) - 2.0 * dw;
    if (!(p.leff > 0.0)) throw std::runtime_error(who + "effective channel length <= 0");
    if (!(p.weff > 0.0)) throw std::runtime_error(who + "effective channel width <= 0");

    const bool micron = static_cast<int>(value("binunit", 1)) == 1;
    const double invL = (micron ? 1.0e-6 : 1.0) / p.leff, invW = (micron ? 1.0e-6 : 1.0) / p.weff;
    const double invLW = (micron ? 1.0e-12 : 1.0) / (p.leff * p.weff);
    const auto bin = [&](const std::string& key, double fallback) {
        return value(key, fallback) + value("l" + key, 0.0) * invL + value("w" + key, 0.0) * invW +
               value("p" + key, 0.0) * invLW;
    };
    const auto binGiven = [&](const std::string& key) {
        return given(key) || given("l" + key) || given("w" + key) || given("p" + key);
    };

    p.cdsc = bin("cdsc", 2.4e-4);
    p.cdscb = bin("cdscb", 0.0);
    p.cdscd = bin("cdscd", 0.0);
    p.cit = bin("cit", 0.0);
    p.nfactor = bin("nfactor", 1.0);
    p.xj = bin("xj", 0.15e-6);
    const double vsat = bin("vsat", 8.0e4), at = bin("at", 3.3e4);
    p.a0 = bin("a0", 1.0);
    p.ags = bin("ags", 0.0);
    p.a1 = bin("a1", 0.0);
    p.a2 = bin("a2", 1.0);
    p.keta = bin("keta", -0.047);
    double nsub = bin("nsub", 6.0e16), npeak = bin("npeak", 1.7e17);
    p.ngate = bin("ngate", 0.0);
    double gamma1 = bin("gamma1", 0.0), gamma2 = bin("gamma2", 0.0);
    double vbx = bin("vbx", 0.0), vbm = bin("vbm", -3.0);
    const double xt = bin("xt", 1.55e-7);
    p.kt1 = bin("kt1", -0.11);
    p.kt1l = bin("kt1l", 0.0);
    p.kt2 = bin("kt2", 0.022);
    p.k3 = bin("k3", 80.0);
    p.k3b = bin("k3b", 0.0);
    p.w0 = bin("w0", 2.5e-6);
    p.nlx = bin("nlx", 1.74e-7);
    p.dvt0 = bin("dvt0", 2.2);
    p.dvt1 = bin("dvt1", 0.53);
    p.dvt2 = bin("dvt2", -0.032);
    p.dvt0w = bin("dvt0w", 0.0);
    p.dvt1w = bin("dvt1w", 5.3e6);
    p.dvt2w = bin("dvt2w", -0.032);
    const double drout = bin("drout", 0.56);
    const double dsub = binGiven("dsub") ? bin("dsub", 0.0) : drout;
    p.ua = bin("ua", 2.25e-9) + bin("ua1", 4.31e-9) * dT;
    p.ub = bin("ub", 5.87e-19) + bin("ub1", -7.61e-18) * dT;
    p.uc = bin("uc", p.mobMod == 3 ? -0.0465 : -0.0465e-9) + bin("uc1", p.mobMod == 3 ? -0.056 : -0.056e-9) * dT;
    double u0 = bin("u0", pol_ > 0.0 ? 0.067 : 0.025);
    if (u0 > 1.0) u0 /= 1.0e4;  // given in cm^2/Vs
    const double ute = bin("ute", -1.5);
    p.voff = bin("voff", -0.08);
    p.delta = bin("delta", 0.01);
    const double rdsw = bin("rdsw", 0.0), prt = bin("prt", 0.0), wr = bin("wr", 1.0);
    p.prwg = bin("prwg", 0.0);
    p.prwb = bin("prwb", 0.0);
    p.eta0 = bin("eta0", 0.08);
    p.etab = bin("etab", -0.07);
    p.pclm = bin("pclm", 1.3);
    const double pdibl1 = bin("pdibl1", 0.39), pdibl2 = bin("pdibl2", 0.0086);
    p.pdiblb = bin("pdiblb", 0.0);
    p.pscbe1 = bin("pscbe1", 4.24e8);
    p.pscbe2 = bin("pscbe2", 1.0e-5);
    p.pvag = bin("pvag", 0.0);
    p.dwg = bin("dwg", 0.0);
    p.dwb = bin("dwb", 0.0);
    p.b0 = bin("b0", 0.0);
    p.b1 = bin("b1", 0.0);
    p.alpha0 = bin("alpha0", 0.0);
    p.alpha1 = bin("alpha1", 0.0);
    p.beta0 = bin("beta0", 30.0);

    // Doping given per m^3 instead of per cm^3.
    if (npeak > 1.0e20) npeak *= 1.0e-6;
    if (nsub > 1.0e20) nsub *= 1.0e-6;
    if (p.ngate > 1.0e23) p.ngate *= 1.0e-6;
    if (!(npeak > 0.0) || !(nsub > 0.0)) throw std::runtime_error(who + "nch and nsub must be > 0");

    p.u0temp = u0 * std::pow(TRatio, ute);
    p.vsattemp = vsat - at * dT;
    p.rds0 = (rdsw + prt * dT) / std::pow(p.weff * 1.0e6, wr);
    if (!(p.vsattemp > 0.0)) throw std::runtime_error(who + "vsat must be > 0 at this temperature");

    if (!binGiven("npeak") && binGiven("gamma1")) {
        const double t = gamma1 * p.cox;
        npeak = 3.021e22 * t * t;
    }
    p.phi = 2.0 * Vtm0 * std::log(npeak / ni);
    p.sqrtPhi = std::sqrt(p.phi);
    p.phis3 = p.sqrtPhi * p.phi;
    p.xdep0 = std::sqrt(2.0 * kEpsSi / (kCharge * npeak * 1.0e6)) * p.sqrtPhi;
    p.litl = std::sqrt(3.0 * p.xj * p.tox);
    p.vbi = Vtm0 * std::log(1.0e20 * npeak / (ni * ni));
    p.cdep0 = std::sqrt(kCharge * kEpsSi * npeak * 1.0e6 / 2.0 / p.phi);

    // Body effect coefficients: given, or from the doping profile.
    if (binGiven("k1") || binGiven("k2")) {
        p.k1 = binGiven("k1") ? bin("k1", 0.0) : 0.53;
        p.k2 = binGiven("k2") ? bin("k2", 0.0) : -0.0186;
    } else {
        if (!binGiven("vbx")) vbx = p.phi - 7.7348e-4 * npeak * xt * xt;
        if (vbx > 0.0) vbx = -vbx;
        if (vbm > 0.0) vbm = -vbm;
        if (!binGiven("gamma1")) gamma1 = 5.753e-12 * std::sqrt(npeak) / p.cox;
        if (!binGiven("gamma2")) gamma2 = 5.753e-12 * std::sqrt(nsub) / p.cox;
        const double a = gamma1 - gamma2;
        const double b = std::sqrt(p.phi - vbx) - p.sqrtPhi;
        const double c = std::sqrt(p.phi * (p.phi - vbm)) - p.phi;
        p.k2 = a * b / (2.0 * c + vbm);
        p.k1 = gamma2 - 2.0 * p.k2 * std::sqrt(p.phi - vbm);
    }
    if (p.k2 < 0.0) {
        const double t = 0.5 * p.k1 / p.k2;
        p.vbsc = std::min(-3.0, std::max(-30.0, 0.9 * (p.phi - t * t)));
    } else {
        p.vbsc = -30.0;
    }
    if (p.vbsc > vbm) p.vbsc = vbm;

    // Flat-band and threshold voltage, in the n-channel frame.
    const bool vthGiven = binGiven("vth0");
    const double vthModel = bin("vth0", pol_ > 0.0 ? 0.7 : -0.7);
    p.vfb = binGiven("vfb") ? bin("vfb", 0.0)
                            : (vthGiven ? pol_ * vthModel - p.phi - p.k1 * p.sqrtPhi : -1.0);
    p.vth0 = vthGiven ? pol_ * vthModel : p.vfb + p.phi + p.k1 * p.sqrtPhi;
    p.k1ox = p.k1 * p.tox / toxm;
    p.k2ox = p.k2 * p.tox / toxm;

    const double lt0 = std::sqrt(kEpsSi / kEpsOx * p.tox * p.xdep0);
    double e = std::exp(-0.5 * dsub * p.leff / lt0);
    p.theta0vb0 = e + 2.0 * e * e;
    e = std::exp(-0.5 * drout * p.leff / lt0);
    p.thetaRout = pdibl1 * (e + 2.0 * e * e) + pdibl2;

    // Capacitances: channel, overlaps (with the defaults of the model), junctions.
    coxWL_ = p.cox * p.weff * p.leff * mult_;
    const double dlc = value("dlc", value("lint", 0.0)), dwc = value("dwc", value("wint", 0.0));
    const double cf = value("cf", 2.0 * kEpsOx / kPiB * std::log(1.0 + 0.4e-6 / p.tox));
    const double overlap = (given("dlc") && dlc > 0.0) ? dlc * p.cox : 0.6 * p.xj * p.cox;
    cgso_ = (value("cgso", overlap) + cf) * p.weff * mult_;
    cgdo_ = (value("cgdo", overlap) + cf) * p.weff * mult_;
    cgbo_ = value("cgbo", 2.0 * dwc * p.cox) * p.leff * mult_;

    const double cj = value("cj", 5.0e-4), cjsw = value("cjsw", 5.0e-10), cjswg = value("cjswg", cjsw);
    pbJ_[0] = value("pb", 1.0);
    pbJ_[1] = value("pbsw", 1.0);
    pbJ_[2] = value("pbswg", pbJ_[1]);
    mjJ_[0] = value("mj", 0.5);
    mjJ_[1] = value("mjsw", 0.33);
    mjJ_[2] = value("mjswg", mjJ_[1]);
    const auto junctionCaps = [&](double area, double perimeter, double cz[3]) {
        cz[0] = cj * area * mult_;
        if (perimeter < p.weff) {  // the whole perimeter lies along the gate
            cz[1] = 0.0;
            cz[2] = cjswg * perimeter * mult_;
        } else {
            cz[1] = cjsw * (perimeter - p.weff) * mult_;
            cz[2] = cjswg * p.weff * mult_;
        }
    };
    const double as = value("as", 0.0), ad = value("ad", 0.0), ps = value("ps", 0.0), pd = value("pd", 0.0);
    junctionCaps(as, ps, czs_);
    junctionCaps(ad, pd, czd_);

    // Junction diodes: saturation current from area and perimeter, scaled to the temperature.
    const double nj = value("nj", 1.0), xti = value("xti", 3.0);
    const double scale = Temp != Tnom ? std::exp((Eg0 / Vtm0 - Eg / p.vtm + xti * std::log(Temp / Tnom)) / nj) : 1.0;
    const double js = value("js", 1.0e-4) * scale, jsw = value("jsw", 0.0) * scale;
    const auto saturation = [&](double area, double perimeter) {
        return (area <= 0.0 && perimeter <= 0.0) ? 1.0e-14 : std::max(area * js + perimeter * jsw, 1.0e-30);
    };
    jbs_.configure(saturation(as, ps) * mult_, nj * p.vtm);
    jbd_.configure(saturation(ad, pd) * mult_, nj * p.vtm);

    kT_ = kBoltzmann * Temp;
    kf_ = value("kf", 0.0);
    af_ = value("af", 1.0);
}

// Drain current of BSIM3v3.3 for vds >= 0 (n-channel frame). The steps and
// their names follow the model equations: effective body bias, threshold
// voltage, subthreshold slope, effective gate drive, bulk charge factor,
// mobility, saturation voltage, effective drain voltage, output resistance.
Dual Bsim3::drainCurrent(const Dual& Vgs, const Dual& Vds, const Dual& Vbs, Dual& Isub, Dual& VgstOut) const {
    const Prepared& p = p_;
    const double Leff = p.leff, Vtm = p.vtm, cox = p.cox;

    // Effective body bias: bounded below by vbsc.
    Dual T0 = Vbs - p.vbsc - 0.001;
    Dual T1 = sqrt(T0 * T0 - 0.004 * p.vbsc);
    Dual Vbseff = p.vbsc + 0.5 * (T0 + T1);
    if (Vbseff.v < Vbs.v) Vbseff = Vbs;

    Dual Phis, sqrtPhis;
    if (Vbseff.v > 0.0) {
        Phis = p.phi * p.phi / (p.phi + Vbseff);
        sqrtPhis = p.phis3 / (p.phi + 0.5 * Vbseff);
    } else {
        Phis = p.phi - Vbseff;
        sqrtPhis = sqrt(Phis);
    }
    const Dual Xdep = p.xdep0 * sqrtPhis / p.sqrtPhi;

    // Threshold voltage.
    Dual T3 = sqrt(Xdep);
    const double V0 = p.vbi - p.phi;
    const auto bounded = [](const Dual& t) {  // 1 + t, kept positive
        return t.v >= -0.5 ? 1.0 + t : (1.0 + 3.0 * t) / (3.0 + 8.0 * t);
    };
    const Dual lt1 = p.factor1 * T3 * bounded(p.dvt2 * Vbseff);
    const Dual ltw = p.factor1 * T3 * bounded(p.dvt2w * Vbseff);
    const auto shortChannel = [](const Dual& t) {  // exp(t) (1 + 2 exp(t))
        const Dual e = t.v > -kExpThreshold ? exp(t) : Dual(kMinExp);
        return e * (1.0 + 2.0 * e);
    };
    const Dual Theta0 = shortChannel(-0.5 * p.dvt1 * Leff / lt1);
    const Dual Delt_vth = p.dvt0 * Theta0 * V0;
    const Dual narrow = p.dvt0w * shortChannel(-0.5 * p.dvt1w * p.weff * Leff / ltw) * V0;

    const double lateral = std::sqrt(1.0 + p.nlx / Leff);
    T1 = p.k1ox * (lateral - 1.0) * p.sqrtPhi + (p.kt1 + p.kt1l / Leff + p.kt2 * Vbseff) * p.tempRatio;
    const double tmp2w = p.tox * p.phi / (p.weff + p.w0);
    T3 = p.eta0 + p.etab * Vbseff;
    if (T3.v < 1.0e-4) T3 = (2.0e-4 - T3) / (3.0 - 2.0e4 * T3);
    const Dual DIBL_Sft = T3 * p.theta0vb0 * Vds;
    const Dual Vth = p.vth0 - p.k1 * p.sqrtPhi + p.k1ox * sqrtPhis - p.k2ox * Vbseff - Delt_vth - narrow +
                     (p.k3 + p.k3b * Vbseff) * tmp2w + T1 - DIBL_Sft;

    // Subthreshold swing factor n.
    const Dual tmp4 = (p.nfactor * kEpsSi / Xdep + (p.cdsc + p.cdscb * Vbseff + p.cdscd * Vds) * Theta0 + p.cit) / cox;
    const Dual n = tmp4.v >= -0.5 ? 1.0 + tmp4 : (1.0 + 3.0 * tmp4) / (3.0 + 8.0 * tmp4);

    // Poly-silicon gate depletion.
    Dual Vgs_eff = Vgs;
    const double vfbPhi = p.vfb + p.phi;
    if (p.ngate > 1.0e18 && p.ngate < 1.0e25 && Vgs.v > vfbPhi) {
        const double t1 = 1.0e6 * kCharge * kEpsSi * p.ngate / (cox * cox);
        const Dual T4 = sqrt(1.0 + 2.0 * (Vgs - vfbPhi) / t1);
        const Dual T2 = t1 * (T4 - 1.0);
        const Dual Vpoly = 0.5 * T2 * T2 / t1;
        const Dual T7 = 1.12 - Vpoly - 0.05;
        const Dual T6 = sqrt(T7 * T7 + 0.224);
        Vgs_eff = Vgs - (1.12 - 0.5 * (T7 + T6));
    }
    const Dual Vgst = Vgs_eff - Vth;
    VgstOut = Vgst;

    // Effective gate drive: Vgst above threshold, exponential below.
    const Dual T10 = 2.0 * n * Vtm;
    const Dual VgstNVt = Vgst / T10;
    const Dual ExpArg = (2.0 * p.voff - Vgst) / T10;
    Dual Vgsteff;
    if (VgstNVt.v > kExpThreshold) {
        Vgsteff = Vgst;
    } else if (ExpArg.v > kExpThreshold) {
        Vgsteff = Vtm * p.cdep0 / cox * exp((Vgst - p.voff) / (n * Vtm));
    } else {
        const Dual num = T10 * log(1.0 + exp(VgstNVt));
        const Dual den = 1.0 + T10 * cox / (Vtm * p.cdep0) * exp(ExpArg);
        Vgsteff = num / den;
    }

    // Effective width and source/drain resistance.
    const Dual dPhis = sqrtPhis - p.sqrtPhi;
    Dual Weff = p.weff - 2.0 * (p.dwg * Vgsteff + p.dwb * dPhis);
    if (Weff.v < 2.0e-8) Weff = 2.0e-8 * (4.0e-8 - Weff) / (6.0e-8 - 2.0 * Weff);
    T0 = p.prwg * Vgsteff + p.prwb * dPhis;
    const Dual Rds = T0.v >= -0.9 ? p.rds0 * (1.0 + T0) : p.rds0 * (0.8 + T0) / (17.0 + 20.0 * T0);

    // Bulk charge factor Abulk.
    T1 = 0.5 * p.k1ox / sqrtPhis;
    const Dual T5 = Leff / (Leff + 2.0 * sqrt(p.xj * Xdep));
    Dual Abulk0 = 1.0 + T1 * (p.a0 * T5 + p.b0 / (p.weff + p.b1));
    Dual Abulk = Abulk0 - T1 * p.ags * p.a0 * T5 * T5 * T5 * Vgsteff;
    if (Abulk0.v < 0.1) Abulk0 = (0.2 - Abulk0) / (3.0 - 20.0 * Abulk0);
    if (Abulk.v < 0.1) Abulk = (0.2 - Abulk) / (3.0 - 20.0 * Abulk);
    Dual T2 = p.keta * Vbseff;
    T0 = T2.v >= -0.9 ? 1.0 / (1.0 + T2) : (17.0 + 20.0 * T2) / (0.8 + T2);
    Abulk = Abulk * T0;

    // Mobility.
    if (p.mobMod == 1) {
        T3 = (Vgsteff + Vth + Vth) / p.tox;
        T0 = T3 * (p.ua + p.uc * Vbseff + p.ub * T3);
    } else if (p.mobMod == 2) {
        T0 = Vgsteff / p.tox * (p.ua + p.uc * Vbseff + p.ub * Vgsteff / p.tox);
    } else {
        T3 = (Vgsteff + Vth + Vth) / p.tox;
        T0 = T3 * (p.ua + p.ub * T3) * (1.0 + p.uc * Vbseff);
    }
    const Dual Denomi = T0.v >= -0.8 ? 1.0 + T0 : (0.6 + T0) / (7.0 + 10.0 * T0);
    const Dual ueff = p.u0temp / Denomi;

    // Saturation voltage.
    const Dual WVCoxRds = Weff * p.vsattemp * cox * Rds;
    const Dual EsatL = 2.0 * p.vsattemp / ueff * Leff;
    Dual Lambda = p.a2;
    if (p.a1 > 0.0) {
        const double t0 = 1.0 - p.a2;
        T1 = t0 - p.a1 * Vgsteff - 0.0001;
        Lambda = p.a2 + t0 - 0.5 * (T1 + sqrt(T1 * T1 + 0.0004 * t0));
    } else if (p.a1 < 0.0) {
        T1 = p.a2 + p.a1 * Vgsteff - 0.0001;
        Lambda = 0.5 * (T1 + sqrt(T1 * T1 + 0.0004 * p.a2));
    }
    const Dual Vgst2Vtm = Vgsteff + 2.0 * Vtm;
    Dual Vdsat;
    if (Rds.v == 0.0 && Lambda.v == 1.0) {
        Vdsat = EsatL * Vgst2Vtm / (Abulk * EsatL + Vgst2Vtm);
    } else {
        const Dual T9 = Abulk * WVCoxRds;
        const Dual T7 = Vgst2Vtm * T9;
        const Dual T6 = Vgst2Vtm * WVCoxRds;
        T0 = 2.0 * Abulk * (T9 - 1.0 + 1.0 / Lambda);
        T1 = Vgst2Vtm * (2.0 / Lambda - 1.0) + Abulk * EsatL + 3.0 * T7;
        T2 = Vgst2Vtm * (EsatL + 2.0 * T6);
        Vdsat = (T1 - sqrt(T1 * T1 - 2.0 * T0 * T2)) / T0;
    }

    // Effective drain voltage: vds below saturation, vdsat above, smooth in between.
    T1 = Vdsat - Vds - p.delta;
    T2 = sqrt(T1 * T1 + 4.0 * p.delta * Vdsat);
    Dual Vdseff = Vdsat - 0.5 * (T1 + T2);
    if (Vds.v == 0.0) {  // exactly zero there, with the slope along vds kept
        Vdseff.v = 0.0;
        Vdseff.d[0] = Vdseff.d[2] = 0.0;
    }

    // Early voltage from velocity saturation.
    const Dual tmp4s = 1.0 - 0.5 * Abulk * Vdsat / Vgst2Vtm;
    const Dual Vasat = (EsatL + Vdsat + 2.0 * WVCoxRds * Vgsteff * tmp4s) / (2.0 / Lambda - 1.0 + WVCoxRds * Abulk);
    if (Vdseff.v > Vds.v) Vdseff = Vds;
    const Dual diffVds = Vds - Vdseff;

    // Channel length modulation.
    Dual VACLM = kMaxExp;
    if (p.pclm > 0.0 && diffVds.v > 1.0e-10)
        VACLM = Leff * (Abulk + Vgsteff / EsatL) / (p.pclm * Abulk * p.litl) * diffVds;

    // Drain-induced barrier lowering.
    Dual VADIBL = kMaxExp;
    if (p.thetaRout > 0.0) {
        const Dual T8 = Abulk * Vdsat;
        VADIBL = (Vgst2Vtm - Vgst2Vtm * T8 / (Vgst2Vtm + T8)) / p.thetaRout;
        const Dual T7 = p.pdiblb * Vbseff;
        VADIBL = VADIBL * (T7.v >= -0.9 ? 1.0 / (1.0 + T7) : (17.0 + 20.0 * T7) / (0.8 + T7));
    }

    Dual T9 = p.pvag / EsatL * Vgsteff;
    T0 = T9.v > -0.9 ? 1.0 + T9 : (0.8 + T9) / (17.0 + 20.0 * T9);
    const Dual Va = Vasat + T0 * VACLM * VADIBL / (VACLM + VADIBL);

    // Substrate-current-induced body effect.
    Dual VASCBE = kMaxExp;
    if (p.pscbe2 > 0.0) {
        if (diffVds.v > p.pscbe1 * p.litl / kExpThreshold)
            VASCBE = Leff * exp(p.pscbe1 * p.litl / diffVds) / p.pscbe2;
        else
            VASCBE = kMaxExp * Leff / p.pscbe2;
    }

    // Drain current.
    const Dual beta = ueff * cox * Weff / Leff;
    const Dual fgche1 = Vgsteff * (1.0 - 0.5 * Abulk * Vdseff / Vgst2Vtm);
    const Dual fgche2 = 1.0 + Vdseff / EsatL;
    const Dual gche = beta * fgche1 / fgche2;
    const Dual Idl = gche * Vdseff / (1.0 + gche * Rds);
    const Dual Idsa = Idl * (1.0 + diffVds / Va);
    const Dual Ids = Idsa * (1.0 + diffVds / VASCBE);

    // Substrate current from impact ionization.
    const double tmp = p.alpha0 + p.alpha1 * Leff;
    if (tmp <= 0.0 || p.beta0 <= 0.0) {
        Isub = 0.0;
    } else if (diffVds.v > p.beta0 / kExpThreshold) {
        Isub = tmp / Leff * diffVds * exp(-p.beta0 / diffVds) * Idsa;
    } else {
        Isub = tmp / Leff * kMinExp * diffVds * Idsa;
    }
    return Ids;
}

void Bsim3::noiseSources(std::vector<NoiseSource>& out) const {
    out.push_back({name(), d_, s_, 4.0 * kT_ * std::max(2.0 / 3.0 * opGm_, opGds_),
                   kf_ * std::pow(std::abs(opId_), af_)});
}

// Bulk junction to drain or source: diode current, and the depletion charges
// of the bottom, the sidewall and the sidewall along the gate.
void Bsim3::addJunction(StampContext& ctx, Local& m, PnJunction& j, int to, const double cz[3]) {
    const int node = to == kD ? d_ : s_;
    if (b_ == node) return;
    const double v = pol_ * (ctx.v(b_) - ctx.v(node));
    const double vl = j.limit(v, ctx.limiting, ctx.out.limited);
    double i, g;
    j.current(vl, i, g);
    i += ctx.gmin * vl;
    g += ctx.gmin;
    double q = 0.0, c = 0.0;
    for (int k = 0; k < 3; ++k) {
        if (cz[k] <= 0.0) continue;
        double qk, ck;
        depletionCharge(vl, cz[k], pbJ_[k], mjJ_[k], 0.5, qk, ck);
        q += qk;
        c += ck;
    }
    i += g * (v - vl);
    q += c * (v - vl);
    m.f[kB] += pol_ * i;
    m.f[to] -= pol_ * i;
    m.g[kB][kB] += g;
    m.g[kB][to] -= g;
    m.g[to][kB] -= g;
    m.g[to][to] += g;
    m.q[kB] += pol_ * q;
    m.q[to] -= pol_ * q;
    m.c[kB][kB] += c;
    m.c[kB][to] -= c;
    m.c[to][kB] -= c;
    m.c[to][to] += c;
}

void Bsim3::stamp(StampContext& ctx) {
    Local m;  // terminal order d, g, s, b; handed over in a fixed sequence, see Mosfet::stamp

    // The model is written for vds >= 0: otherwise drain and source swap roles.
    int D = kD, S = kS;
    int nodeS = s_, nodeD = d_;
    if (pol_ * (ctx.v(d_) - ctx.v(s_)) < 0.0) {
        std::swap(D, S);
        std::swap(nodeS, nodeD);
    }
    // Derivatives are taken with respect to (vgs, vds, vbs).
    const Dual vgs = Dual::variable(pol_ * (ctx.v(g_) - ctx.v(nodeS)), 0);
    const Dual vds = Dual::variable(pol_ * (ctx.v(nodeD) - ctx.v(nodeS)), 1);
    const Dual vbs = Dual::variable(pol_ * (ctx.v(b_) - ctx.v(nodeS)), 2);

    Dual isub, vgst;
    Dual ids = drainCurrent(vgs, vds, vbs, isub, vgst) * mult_;
    isub = isub * mult_;
    opId_ = ids.v;
    opGm_ = ids.d[0];
    opGds_ = ids.d[1];
    ids = ids + ctx.gmin * vds;

    // A quantity flowing into terminal `row` (sign +1) or out of it (-1). Its
    // derivative with respect to vgs belongs to the columns of gate and
    // source, the one for vds to drain and source, the one for vbs to bulk
    // and source.
    const int col[3] = {kG, D, kB};
    const auto add = [&](int row, const Dual& value, double f[4], double g[4][4], double sign) {
        f[row] += sign * pol_ * value.v;
        for (int k = 0; k < 3; ++k) {
            g[row][col[k]] += sign * value.d[k];
            g[row][S] -= sign * value.d[k];
        }
    };
    add(D, ids, m.f, m.g, 1.0);
    add(S, ids, m.f, m.g, -1.0);
    add(D, isub, m.f, m.g, 1.0);
    add(kB, isub, m.f, m.g, -1.0);

    // Intrinsic charges: long-channel model with a = vgs - vth of BSIM3 and
    // b = max(a - vds, 0); below threshold the gate charges the bulk.
    if (coxWL_ > 0.0) {
        const double K = coxWL_;
        const Dual& a = vgst;
        Dual qg, qd, qs, qb;
        if (a.v <= 0.0) {
            qg = K * a;
            qb = -K * a;
        } else {
            const Dual b = vds.v < a.v ? a - vds : Dual(0.0);
            const Dual sum = a + b;
            const Dual qi = -(2.0 / 3.0) * K * (a * a + a * b + b * b) / sum;
            qd = -(2.0 / 15.0) * K * (2.0 * a * a * a + 4.0 * a * a * b + 6.0 * a * b * b + 3.0 * b * b * b) /
                 (sum * sum);
            qs = qi - qd;
            qg = -qi;
        }
        add(kG, qg, m.q, m.c, 1.0);
        add(D, qd, m.q, m.c, 1.0);
        add(S, qs, m.q, m.c, 1.0);
        add(kB, qb, m.q, m.c, 1.0);
    }

    addJunction(ctx, m, jbd_, kD, czd_);
    addJunction(ctx, m, jbs_, kS, czs_);

    const int node[4] = {d_, g_, s_, b_};
    const auto linearCap = [&](int a, int b, double c) {
        const double q = c * (ctx.v(node[a]) - ctx.v(node[b]));
        m.q[a] += q;
        m.q[b] -= q;
        m.c[a][a] += c;
        m.c[a][b] -= c;
        m.c[b][a] -= c;
        m.c[b][b] += c;
    };
    linearCap(kG, kS, cgso_);
    linearCap(kG, kD, cgdo_);
    linearCap(kG, kB, cgbo_);

    for (int i = 0; i < 4; ++i) {
        ctx.addF(node[i], m.f[i]);
        for (int j = 0; j < 4; ++j) ctx.addG(node[i], node[j], m.g[i][j]);
    }
    for (int i = 0; i < 4; ++i) {
        ctx.addQ(node[i], m.q[i]);
        for (int j = 0; j < 4; ++j) ctx.addC(node[i], node[j], m.c[i][j]);
    }
}

}  // namespace esim
