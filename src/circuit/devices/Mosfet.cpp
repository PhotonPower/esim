#include "circuit/devices/Mosfet.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "circuit/Circuit.hpp"
#include "circuit/Dual.hpp"

namespace esim {

Mosfet::Mosfet(std::string name, int d, int g, int s, int b, int polarity, const MosfetParams& p)
    : Device(std::move(name)), d_(d), g_(g), s_(s), b_(b), pol_(polarity >= 0 ? 1.0 : -1.0),
      p_(p) {
    if (!(p_.w > 0.0) || !(p_.l > 0.0))
        throw std::runtime_error(this->name() + ": w and l must be > 0");
    if (!(p_.kp > 0.0)) throw std::runtime_error(this->name() + ": kp must be > 0");
    if (!(p_.phi > 0.0)) throw std::runtime_error(this->name() + ": phi must be > 0");
    if (!(p_.is > 0.0)) throw std::runtime_error(this->name() + ": is must be > 0");
    if (p_.tox < 0.0) throw std::runtime_error(this->name() + ": tox must be >= 0");
    if (p_.theta < 0.0 || p_.ucrit < 0.0 || p_.gamma < 0.0)
        throw std::runtime_error(this->name() + ": gamma, theta and ucrit must be >= 0");
    if (p_.level != 1 && p_.level != 3) throw std::runtime_error(this->name() + ": level must be 1 or 3");
    if (p_.level == 3) {
        if (!(p_.tox > 0.0)) throw std::runtime_error(this->name() + ": tox must be > 0 for level 3");
        leff_ = p_.l - 2.0 * p_.ld;
        weff_ = p_.w;
        if (!(leff_ > 0.0)) throw std::runtime_error(this->name() + ": effective length l - 2 ld must be > 0");
        coxArea_ = 3.9 * 8.854214871e-12 / p_.tox;
        if (!p_.kpGiven) p_.kp = p_.u0 * coxArea_ * 1e-4;
        if (p_.nsub < 0.0 || p_.vmax < 0.0 || p_.nfs < 0.0) throw std::runtime_error(this->name() + ": nsub, vmax and nfs must be >= 0");
    }
    beta_ = p_.kp * p_.w / p_.l;
    vto_ = pol_ * p_.vto;
    if (p_.level == 3) cox_ = coxArea_ * leff_ * weff_;
    else if (p_.tox > 0.0) cox_ = 3.9 * 8.8541878128e-12 / p_.tox * p_.w * p_.l;
    hasCharge_ = cox_ > 0.0 || p_.cbd > 0.0 || p_.cbs > 0.0 || p_.cgso > 0.0 || p_.cgdo > 0.0 ||
                 p_.cgbo > 0.0;
}

void Mosfet::setup(Circuit& ckt) {
    const CircuitOptions& o = ckt.options;
    const double vt = o.vt();
    vt_ = vt;
    kT_ = kBoltzmann * (o.temp + 273.15);
    const double is = o.saturationCurrent(p_.is, 1.0, 1.11, 3.0);
    jbd_.configure(is, vt);
    jbs_.configure(is, vt);
    // Mobility ~ T^-1.5; the threshold moves by tcv per kelvin (towards zero for tcv > 0).
    beta_ = p_.kp * std::pow(o.tempRatio(), -1.5) * p_.w / p_.l;
    vto_ = pol_ * p_.vto - p_.tcv * (o.temp - o.tnom);
    if (p_.level == 3) setupLevel3(o);
}

// Level 3: parameters from nsub, and their temperature dependence, as in
// ngspice mos3temp.c.
void Mosfet::setupLevel3(const CircuitOptions& o) {
    constexpr double q = 1.6021766208e-19, k = 1.38064852e-23, refTemp = 300.15;
    constexpr double epsSi = 11.7 * 8.854214871e-12;
    const double tnom = o.tnom + 273.15, temp = o.temp + 273.15;
    const double vtnom = k * tnom / q, vt = k * temp / q;
    const auto egap = [](double t) { return 1.16 - 7.02e-4 * t * t / (t + 1108.0); };
    const double eg1 = egap(tnom), eg = egap(temp);
    double gamma = p_.gamma, phi = p_.phi, vto = p_.vto;
    if (p_.nsub > 0.0) {
        const double nifact = (tnom / 300.0) * std::sqrt(tnom / 300.0) * std::exp(0.5 * eg1 * (1.0 / 300.0 - 1.0 / tnom) / (k / q));
        const double ni = 1.45e16 * nifact;
        if (p_.nsub * 1e6 <= ni) throw std::runtime_error(name() + ": nsub below the intrinsic density");
        if (!p_.phiGiven) phi = std::max(0.1, 2.0 * vtnom * std::log(p_.nsub * 1e6 / ni));
        if (!p_.gammaGiven) gamma = std::sqrt(2.0 * epsSi * q * p_.nsub * 1e6) / coxArea_;
        if (!p_.vtoGiven) {
            const double fermis = pol_ * 0.5 * phi;
            double wkfng = 3.2;
            if (p_.tpg != 0.0) wkfng = 3.25 + 0.5 * eg1 - pol_ * p_.tpg * 0.5 * eg1;
            const double vfb = wkfng - (3.25 + 0.5 * eg1 + fermis) - p_.nss * 1e4 * q / coxArea_;
            vto = vfb + pol_ * (gamma * std::sqrt(phi) + phi);
        }
        alpha3_ = 2.0 * epsSi / (q * p_.nsub * 1e6);
        depletion3_ = std::sqrt(alpha3_);
    }
    narrow3_ = p_.delta * 0.5 * M_PI * epsSi / coxArea_;
    const double fact1 = tnom / refTemp, fact2 = temp / refTemp;
    const double arg1 = -eg1 / (2.0 * k * tnom) + 1.1150877 / (k * 2.0 * refTemp);
    const double pbfact1 = -2.0 * vtnom * (1.5 * std::log(fact1) + q * arg1);
    const double arg = -eg / (2.0 * k * temp) + 1.1150877 / (k * 2.0 * refTemp);
    const double pbfact = -2.0 * vt * (1.5 * std::log(fact2) + q * arg);
    const double ratio = temp / tnom, ratio4 = ratio * std::sqrt(ratio);
    phi3_ = fact2 * (phi - pbfact1) / fact1 + pbfact;
    gamma3_ = gamma;
    // built-in voltage (threshold without body charge), n-channel frame
    vbi3_ = pol_ * (vto - pol_ * gamma * std::sqrt(phi) + 0.5 * (eg1 - eg) + pol_ * 0.5 * (phi3_ - phi));
    u0t_ = p_.u0 / ratio4;
    beta_ = p_.kp / ratio4 * weff_ / leff_;
    eta3_ = p_.eta * 8.15e-22 / (coxArea_ * leff_ * leff_ * leff_);
    csonco3_ = q * p_.nfs * 1e4 / coxArea_;
    vt_ = vt;
    // saturation current of the bulk junctions, as ngspice scales it for MOSFETs
    const double isT = p_.is * std::exp(-eg / vt + eg1 / vtnom);
    jbd_.configure(isT, vt);
    jbs_.configure(isT, vt);
}

// Drain current of level 3 (ngspice mos3load.c, "moseq3"), with the
// derivatives by forward differentiation.
void Mosfet::stampLevel3(StampContext& ctx, Local& m) {
    int D = kD, S = kS;
    int nodeD = d_, nodeS = s_;
    if (pol_ * (ctx.v(d_) - ctx.v(s_)) < 0.0) {
        std::swap(D, S);
        std::swap(nodeD, nodeS);
    }
    const Dual vgs = Dual::variable(pol_ * (ctx.v(g_) - ctx.v(nodeS)), 0);
    const Dual vds = Dual::variable(pol_ * (ctx.v(nodeD) - ctx.v(nodeS)), 1);
    const Dual vbs = Dual::variable(pol_ * (ctx.v(b_) - ctx.v(nodeS)), 2);
    const double phi = phi3_, vt = vt_;

    // square root term
    Dual phibs, sqphbs;
    if (vbs.v <= 0.0) {
        phibs = phi - vbs;
        sqphbs = sqrt(phibs);
    } else {
        const double sqphis = std::sqrt(phi);
        sqphbs = sqphis / (1.0 + vbs / (phi + phi));
        phibs = sqphbs * sqphbs;
    }
    // short channel effect
    Dual fshort = 1.0;
    if (p_.xj != 0.0 && depletion3_ != 0.0) {
        const Dual wponxj = depletion3_ * sqphbs / p_.xj;
        const Dual wconxj = 0.0631353 + 0.8013292 * wponxj - 0.01110777 * wponxj * wponxj;
        const Dual arga = wconxj + p_.ld / p_.xj;
        const Dual argc = wponxj / (1.0 + wponxj);
        const Dual argb = sqrt(1.0 - argc * argc);
        fshort = 1.0 - p_.xj / leff_ * (arga * argb - p_.ld / p_.xj);
    }
    // body effect, static feedback, threshold
    const Dual gammas = gamma3_ * fshort;
    const Dual fbodys = 0.5 * gammas / (sqphbs + sqphbs);
    const Dual fbody = fbodys + narrow3_ / weff_;
    const Dual qbonco = gammas * sqphbs + narrow3_ * phibs / weff_;
    const Dual vth = vbi3_ - eta3_ * vds + qbonco;
    // weak and strong inversion
    Dual von = vth, xn = 1.0;
    Dual id = 0.0;
    const bool weak = p_.nfs != 0.0;
    if (weak) {
        xn = 1.0 + csonco3_ + qbonco / (phibs + phibs);
        von = vth + vt * xn;
    }
    if (weak || vgs.v > von.v) {
        const Dual vgsx = vgs.v > von.v ? vgs : von;
        const Dual fgate = 1.0 / (1.0 + p_.theta * (vgsx - vth));
        const Dual us = u0t_ * 1e-4 * fgate;
        const Dual onfbdy = 1.0 / (1.0 + fbody);
        Dual vdsat = (vgsx - vth) * onfbdy, vdsc;
        if (p_.vmax > 0.0) {
            vdsc = leff_ * p_.vmax / us;
            const Dual arga = (vgsx - vth) * onfbdy;
            vdsat = arga + vdsc - sqrt(arga * arga + vdsc * vdsc);
        }
        const Dual vdsx = vds.v < vdsat.v ? vds : vdsat;
        const Dual cdo = vgsx - vth - 0.5 * (1.0 + fbody) * vdsx;
        id = beta_ * fgate * cdo * vdsx;
        Dual fdrain = 1.0;
        if (p_.vmax > 0.0) {
            fdrain = 1.0 / (1.0 + vdsx / vdsc);
            id = id * fdrain;
        }
        // channel length modulation
        bool clm = false;
        Dual delxl;
        if (vds.v <= vdsat.v) {
            if (p_.vmax <= 0.0 && alpha3_ != 0.0) {
                const Dual a = vds / vdsat;
                delxl = sqrt(p_.kappa * alpha3_ * vdsat / 8.0) * (a * a) * (a * a);
                clm = true;
            }
        } else if (p_.vmax <= 0.0) {
            delxl = sqrt(p_.kappa * alpha3_ * (vds - vdsat + vdsat / 8.0));
            clm = alpha3_ != 0.0;
        } else if (alpha3_ != 0.0) {
            Dual gdsat = id * (1.0 - fdrain) / vdsc;
            if (gdsat.v < 1e-12) gdsat = 1e-12;
            const Dual emax = p_.kappa * id / (leff_ * gdsat);
            const Dual arga = 0.5 * emax * alpha3_;
            const Dual argb = sqrt(arga * arga + p_.kappa * alpha3_ * (vds - vdsat));
            delxl = argb - arga;
            clm = true;
        }
        if (clm) {
            if (delxl.v > 0.5 * leff_) delxl = leff_ - leff_ * leff_ / (4.0 * delxl);  // punch through
            id = id / (1.0 - delxl / leff_);
        }
        if (weak && vgs.v < von.v) id = id * exp((vgs - von) / (vt * xn));  // weak inversion
    }

    opId_ = id.v;
    opGm_ = id.d[0];
    opGds_ = id.d[1];
    const double gm = id.d[0], gds = id.d[1] + ctx.gmin, gmbs = id.d[2];
    const double i = id.v + ctx.gmin * vds.v;
    m.f[D] += pol_ * i;
    m.f[S] -= pol_ * i;
    const double gss = -(gm + gds + gmbs);
    m.g[D][kG] += gm;
    m.g[D][D] += gds;
    m.g[D][kB] += gmbs;
    m.g[D][S] += gss;
    m.g[S][kG] -= gm;
    m.g[S][D] -= gds;
    m.g[S][kB] -= gmbs;
    m.g[S][S] -= gss;

    if (cox_ > 0.0) addChannelCharge(m, D, S, (vgs - vth).v, vds.v, -vth.d[2]);
}

// Channel charge in the n-channel frame (D, S: terminal positions ordered so
// that vds >= 0). a = vgs - vth, daDvbs = d a / d vbs (body effect).
void Mosfet::addChannelCharge(Local& m, int D, int S, double a, double vds, double daDvbs) const {
    const double K = cox_;
    double qg, qd, qs, qb;
    // Partial derivatives with respect to a and vds.
    double qgA, qgV, qdA, qdV, qsA, qsV, qbA;
    if (a <= 0.0) {
        qg = K * a;
        qb = -K * a;
        qd = qs = 0.0;
        qgA = K;
        qbA = -K;
        qgV = qdA = qdV = qsA = qsV = 0.0;
    } else {
        const bool triode = vds < a;
        const double b = triode ? a - vds : 0.0;
        const double sum = a + b, sum2 = sum * sum, sum3 = sum2 * sum;
        const double qi = -(2.0 / 3.0) * K * (a * a + a * b + b * b) / sum;
        const double qiA = -(2.0 / 3.0) * K * (a * a + 2.0 * a * b) / sum2;  // d qi / d a at fixed b
        const double qiB = -(2.0 / 3.0) * K * (b * b + 2.0 * a * b) / sum2;
        const double num = 2.0 * a * a * a + 4.0 * a * a * b + 6.0 * a * b * b + 3.0 * b * b * b;
        const double numA = 6.0 * a * a + 8.0 * a * b + 6.0 * b * b;
        const double numB = 4.0 * a * a + 12.0 * a * b + 9.0 * b * b;
        const double qdn = -(2.0 / 15.0) * K * num / sum2;
        const double qdnA = -(2.0 / 15.0) * K * (numA * sum - 2.0 * num) / sum3;
        const double qdnB = -(2.0 / 15.0) * K * (numB * sum - 2.0 * num) / sum3;
        // b = a - vds in the triode region, 0 in saturation.
        const double bA = triode ? 1.0 : 0.0, bV = triode ? -1.0 : 0.0;
        const double qiTotA = qiA + qiB * bA, qiTotV = qiB * bV;
        qd = qdn;
        qdA = qdnA + qdnB * bA;
        qdV = qdnB * bV;
        qs = qi - qd;
        qsA = qiTotA - qdA;
        qsV = qiTotV - qdV;
        qg = -qi;
        qgA = -qiTotA;
        qgV = -qiTotV;
        qb = 0.0;
        qbA = 0.0;
    }

    // a depends on vgs (1) and vbs (daDvbs); with vgs = pol (vg - vS) etc. the
    // derivatives with respect to the terminal voltages follow (pol^2 = 1).
    const auto add = [&](int row, double q, double qA, double qV) {
        m.q[row] += pol_ * q;
        const double dG = qA, dD = qV, dB = qA * daDvbs;
        m.c[row][kG] += dG;
        m.c[row][D] += dD;
        m.c[row][kB] += dB;
        m.c[row][S] -= dG + dD + dB;
    };
    add(kG, qg, qgA, qgV);
    add(D, qd, qdA, qdV);
    add(S, qs, qsA, qsV);
    add(kB, qb, qbA, 0.0);
}

// Thermal noise of the channel: (8/3) kT gm in saturation, 4 kT gds as the
// channel becomes a resistor (the larger of the two is used). Flicker noise as
// in SPICE: kf id^af / (f cox' L^2), or kf id^af / f without tox.
void Mosfet::noiseSources(std::vector<NoiseSource>& out) const {
    const double thermal = 4.0 * kT_ * std::max(2.0 / 3.0 * opGm_, opGds_);
    double flicker = p_.kf * std::pow(std::abs(opId_), p_.af);
    if (cox_ > 0.0) flicker /= cox_ / (p_.w * p_.l) * p_.l * p_.l;
    out.push_back({name(), d_, s_, thermal, flicker});
}

// Bulk diode from the bulk to the terminal at position `to` (drain or source).
void Mosfet::addBulkJunction(StampContext& ctx, Local& m, PnJunction& j, int to, double cj0) {
    if (b_ == (to == kD ? d_ : s_)) return;  // bulk tied to this terminal: no voltage, no current
    const double v = pol_ * (ctx.v(b_) - ctx.v(to == kD ? d_ : s_));
    const double vl = j.limit(v, ctx.limiting, ctx.out.limited);
    double i, g, q, c;
    j.current(vl, i, g);
    i += ctx.gmin * vl;
    g += ctx.gmin;
    depletionCharge(vl, cj0, p_.pb, p_.mj, p_.fc, q, c);
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

void Mosfet::addLinearCap(StampContext& ctx, Local& m, int a, int b, double c) const {
    if (c <= 0.0) return;
    const int node[4] = {d_, g_, s_, b_};
    const double q = c * (ctx.v(node[a]) - ctx.v(node[b]));
    m.q[a] += q;
    m.q[b] -= q;
    m.c[a][a] += c;
    m.c[a][b] -= c;
    m.c[b][a] -= c;
    m.c[b][b] += c;
}

// Channel current and charges of the EKV model.
void Mosfet::stampEkv(StampContext& ctx, Local& m) {
    // Terminal voltages against the bulk in the n-channel frame; the
    // derivatives are taken with respect to (gate, drain, source).
    const int pos[3] = {kG, kD, kS};
    const Dual vg = Dual::variable(pol_ * (ctx.v(g_) - ctx.v(b_)), 0);
    const Dual vd = Dual::variable(pol_ * (ctx.v(d_) - ctx.v(b_)), 1);
    const Dual vs = Dual::variable(pol_ * (ctx.v(s_) - ctx.v(b_)), 2);
    const double vt = vt_, gamma = p_.gamma;

    const Dual vgp = vg - vto_ + p_.phi + gamma * std::sqrt(p_.phi);
    // Depletion and inversion; 0 in accumulation. The corner at flat band is
    // rounded narrowly, so that it does not reach into the subthreshold region.
    const Dual vgpPos = smoothPositive(vgp, 0.25 * vt);
    const Dual vp = vgpPos - p_.phi - gamma * (sqrt(vgpPos + 0.25 * gamma * gamma) - 0.5 * gamma);
    const Dual n = 1.0 + gamma / (2.0 * sqrt(vp + p_.phi + 4.0 * vt));
    const Dual sf = softplus((vp - vs) / (2.0 * vt));  // sqrt of the forward current
    const Dual sr = softplus((vp - vd) / (2.0 * vt));  // sqrt of the reverse current

    Dual id = 2.0 * n * beta_ * vt * vt * (sf * sf - sr * sr);
    if (p_.theta > 0.0) id = id / (1.0 + p_.theta * vt * (sf + sr));
    if (p_.ucrit > 0.0) id = id / (1.0 + 2.0 * vt * smoothAbs(sf - sr, 1e-3) / (p_.ucrit * p_.l));
    if (p_.lambda > 0.0) id = id * (1.0 + p_.lambda * smoothAbs(vd - vs, 1e-3));

    opId_ = id.v;
    opGm_ = id.d[0];
    opGds_ = id.d[1];
    id = id + ctx.gmin * (vd - vs);

    // A derivative with respect to a terminal voltage against the bulk is the
    // matrix entry of that terminal; the bulk gets minus their sum.
    const auto add = [&](int row, const Dual& value, double f[4], double g[4][4], double sign) {
        f[row] += sign * pol_ * value.v;
        for (int k = 0; k < 3; ++k) {
            g[row][pos[k]] += sign * value.d[k];
            g[row][kB] -= sign * value.d[k];
        }
    };
    add(kD, id, m.f, m.g, 1.0);
    add(kS, id, m.f, m.g, -1.0);

    if (cox_ > 0.0) {
        const Dual xf = sqrt(sf * sf + 0.25), xr = sqrt(sr * sr + 0.25);
        const Dual sum = xf + xr, nq = n * vt * cox_;
        const Dual qi = -nq * (4.0 / 3.0 * (xf * xf + xf * xr + xr * xr) / sum - 1.0);
        const Dual qd = -nq * (4.0 / 15.0 * (3.0 * xr * xr * xr + 6.0 * xr * xr * xf + 4.0 * xr * xf * xf +
                                             2.0 * xf * xf * xf) / (sum * sum) - 0.5);
        const Dual qs = qi - qd;
        // Bulk: depletion charge under the channel, and the accumulation layer.
        const Dual qb = -cox_ * gamma * sqrt(vp + p_.phi + 1e-6) - (n - 1.0) / n * qi - cox_ * (vgp - vgpPos);
        const Dual qg = -qi - qb;
        add(kG, qg, m.q, m.c, 1.0);
        add(kD, qd, m.q, m.c, 1.0);
        add(kS, qs, m.q, m.c, 1.0);
        add(kB, qb, m.q, m.c, 1.0);
    }
}

void Mosfet::stamp(StampContext& ctx) {
    // All contributions are collected in a local 4 x 4 block (terminal order
    // d, g, s, b) and handed over in one fixed sequence at the end. The
    // sequence of matrix entries therefore never changes, in particular not
    // when drain and source swap their roles, so the solver can keep its map
    // from entries to matrix positions.
    Local m;
    if (p_.ekv) stampEkv(ctx, m);
    else if (p_.level == 3) stampLevel3(ctx, m);
    else stampLevel1(ctx, m);

    addBulkJunction(ctx, m, jbd_, kD, p_.cbd);
    addBulkJunction(ctx, m, jbs_, kS, p_.cbs);

    addLinearCap(ctx, m, kG, kS, p_.cgso * p_.w);
    addLinearCap(ctx, m, kG, kD, p_.cgdo * p_.w);
    addLinearCap(ctx, m, kG, kB, p_.cgbo * p_.l);

    const int node[4] = {d_, g_, s_, b_};
    for (int i = 0; i < 4; ++i) {
        ctx.addF(node[i], m.f[i]);
        for (int j = 0; j < 4; ++j) ctx.addG(node[i], node[j], m.g[i][j]);
    }
    if (hasCharge_) {
        for (int i = 0; i < 4; ++i) {
            ctx.addQ(node[i], m.q[i]);
            for (int j = 0; j < 4; ++j) ctx.addC(node[i], node[j], m.c[i][j]);
        }
    }
}

// Channel current and charges of the level 1 model.
void Mosfet::stampLevel1(StampContext& ctx, Local& m) {
    // Channel current in n-channel form. The device is symmetric: for vds < 0
    // drain and source swap their roles.
    int D = kD, S = kS;
    int nodeD = d_, nodeS = s_;
    if (pol_ * (ctx.v(d_) - ctx.v(s_)) < 0.0) {
        std::swap(D, S);
        std::swap(nodeD, nodeS);
    }
    const double vgs = pol_ * (ctx.v(g_) - ctx.v(nodeS));
    const double vds = pol_ * (ctx.v(nodeD) - ctx.v(nodeS));
    const double vbs = pol_ * (ctx.v(b_) - ctx.v(nodeS));

    // Threshold voltage with body effect.
    const double sqrtPhi = std::sqrt(p_.phi);
    double sarg, dsarg;  // sqrt(phi - vbs) and its derivative with respect to vbs
    if (vbs <= 0.0) {
        sarg = std::sqrt(p_.phi - vbs);
        dsarg = -0.5 / sarg;
    } else {
        sarg = sqrtPhi - vbs / (2.0 * sqrtPhi);
        dsarg = -0.5 / sqrtPhi;
        if (sarg < 0.0) {
            sarg = 0.0;
            dsarg = 0.0;
        }
    }
    const double vth = vto_ + p_.gamma * (sarg - sqrtPhi);
    const double vov = vgs - vth;

    double id = 0.0, gm = 0.0, gds = 0.0;
    if (vov > 0.0) {
        const double clm = 1.0 + p_.lambda * vds;
        if (vds >= vov) {  // saturation
            id = 0.5 * beta_ * vov * vov * clm;
            gm = beta_ * vov * clm;
            gds = 0.5 * beta_ * vov * vov * p_.lambda;
        } else {  // triode
            id = beta_ * vds * (vov - 0.5 * vds) * clm;
            gm = beta_ * vds * clm;
            gds = beta_ * (vov - vds) * clm + beta_ * vds * (vov - 0.5 * vds) * p_.lambda;
        }
    }
    const double gmbs = -gm * p_.gamma * dsarg;  // gm * (-dvth/dvbs)

    opId_ = id;
    opGm_ = gm;
    opGds_ = gds;
    id += ctx.gmin * vds;
    gds += ctx.gmin;

    m.f[D] += pol_ * id;
    m.f[S] -= pol_ * id;
    const double gss = -(gm + gds + gmbs);
    m.g[D][kG] += gm;
    m.g[D][D] += gds;
    m.g[D][kB] += gmbs;
    m.g[D][S] += gss;
    m.g[S][kG] -= gm;
    m.g[S][D] -= gds;
    m.g[S][kB] -= gmbs;
    m.g[S][S] -= gss;

    if (cox_ > 0.0) addChannelCharge(m, D, S, vov, vds, -p_.gamma * dsarg);
}

}  // namespace esim
