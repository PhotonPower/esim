#include "circuit/devices/Mosfet.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "circuit/Circuit.hpp"

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
    beta_ = p_.kp * p_.w / p_.l;
    vto_ = pol_ * p_.vto;
    if (p_.tox > 0.0) cox_ = 3.9 * 8.8541878128e-12 / p_.tox * p_.w * p_.l;
    hasCharge_ = cox_ > 0.0 || p_.cbd > 0.0 || p_.cbs > 0.0 || p_.cgso > 0.0 || p_.cgdo > 0.0 ||
                 p_.cgbo > 0.0;
}

void Mosfet::setup(Circuit& ckt) {
    const CircuitOptions& o = ckt.options;
    const double vt = o.vt();
    kT_ = kBoltzmann * (o.temp + 273.15);
    const double is = o.saturationCurrent(p_.is, 1.0, 1.11, 3.0);
    jbd_.configure(is, vt);
    jbs_.configure(is, vt);
    // Mobility ~ T^-1.5; the threshold moves by tcv per kelvin (towards zero for tcv > 0).
    beta_ = p_.kp * std::pow(o.tempRatio(), -1.5) * p_.w / p_.l;
    vto_ = pol_ * p_.vto - p_.tcv * (o.temp - o.tnom);
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

void Mosfet::stamp(StampContext& ctx) {
    // All contributions are collected in a local 4 x 4 block (terminal order
    // d, g, s, b) and handed over in one fixed sequence at the end. The
    // sequence of matrix entries therefore never changes, in particular not
    // when drain and source swap their roles, so the solver can keep its map
    // from entries to matrix positions.
    Local m;

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

}  // namespace esim
