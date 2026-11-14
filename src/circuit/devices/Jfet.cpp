#include "circuit/devices/Jfet.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "circuit/Circuit.hpp"

namespace esim {

Jfet::Jfet(std::string name, int d, int g, int s, int polarity, const JfetParams& p)
    : Device(std::move(name)), d_(d), g_(g), s_(s), pol_(polarity >= 0 ? 1.0 : -1.0), p_(p),
      hasCharge_(p.cgs > 0.0 || p.cgd > 0.0) {
    if (!(p_.beta > 0.0)) throw std::runtime_error(this->name() + ": beta must be > 0");
    if (!(p_.is > 0.0)) throw std::runtime_error(this->name() + ": is must be > 0");
}

void Jfet::setup(Circuit& ckt) {
    const CircuitOptions& o = ckt.options;
    kT_ = kBoltzmann * (o.temp + 273.15);
    const double is = o.saturationCurrent(p_.is, 1.0, 1.11, 3.0);
    jgs_.configure(is, o.vt());
    jgd_.configure(is, o.vt());
}

void Jfet::noiseSources(std::vector<NoiseSource>& out) const {
    out.push_back({name(), d_, s_, 4.0 * kT_ * std::max(2.0 / 3.0 * opGm_, opGds_),
                   p_.kf * std::pow(std::abs(opId_), p_.af)});
    out.push_back({name() + ".ig", g_, s_, 2.0 * kElementaryCharge * std::abs(opIg_), 0.0});
}

void Jfet::stamp(StampContext& ctx) {
    // Everything is collected in a local 3 x 3 block (terminal order d, g, s)
    // and handed over in a fixed sequence, see Mosfet::stamp.
    double f[3] = {}, q[3] = {}, g[3][3] = {}, c[3][3] = {};

    // Channel, in n-channel form.
    int D = kD, S = kS;
    int nodeD = d_, nodeS = s_;
    if (pol_ * (ctx.v(d_) - ctx.v(s_)) < 0.0) {
        std::swap(D, S);
        std::swap(nodeD, nodeS);
    }
    const double vgs = pol_ * (ctx.v(g_) - ctx.v(nodeS));
    const double vds = pol_ * (ctx.v(nodeD) - ctx.v(nodeS));
    const double vov = vgs - p_.vto;

    double id = 0.0, gm = 0.0, gds = 0.0;
    if (vov > 0.0) {
        const double clm = 1.0 + p_.lambda * vds;
        if (vds >= vov) {  // saturation
            id = p_.beta * vov * vov * clm;
            gm = 2.0 * p_.beta * vov * clm;
            gds = p_.beta * vov * vov * p_.lambda;
        } else {
            id = p_.beta * vds * (2.0 * vov - vds) * clm;
            gm = 2.0 * p_.beta * vds * clm;
            gds = 2.0 * p_.beta * (vov - vds) * clm + p_.beta * vds * (2.0 * vov - vds) * p_.lambda;
        }
    }
    opId_ = id;
    opGm_ = gm;
    opGds_ = gds;
    id += ctx.gmin * vds;
    gds += ctx.gmin;

    f[D] += pol_ * id;
    f[S] -= pol_ * id;
    g[D][kG] += gm;
    g[D][D] += gds;
    g[D][S] -= gm + gds;
    g[S][kG] -= gm;
    g[S][D] -= gds;
    g[S][S] += gm + gds;

    // Gate junctions to source and drain.
    opIg_ = 0.0;
    auto junction = [&](PnJunction& j, int to, int node, double cj0) {
        const double v = pol_ * (ctx.v(g_) - ctx.v(node));
        const double vl = j.limit(v, ctx.limiting, ctx.out.limited);
        double i, gj, qj, cj;
        j.current(vl, i, gj);
        i += ctx.gmin * vl;
        gj += ctx.gmin;
        depletionCharge(vl, cj0, p_.pb, 0.5, p_.fc, qj, cj);
        i += gj * (v - vl);
        qj += cj * (v - vl);
        opIg_ += i;
        f[kG] += pol_ * i;
        f[to] -= pol_ * i;
        g[kG][kG] += gj;
        g[kG][to] -= gj;
        g[to][kG] -= gj;
        g[to][to] += gj;
        q[kG] += pol_ * qj;
        q[to] -= pol_ * qj;
        c[kG][kG] += cj;
        c[kG][to] -= cj;
        c[to][kG] -= cj;
        c[to][to] += cj;
    };
    junction(jgs_, kS, s_, p_.cgs);
    junction(jgd_, kD, d_, p_.cgd);

    const int node[3] = {d_, g_, s_};
    for (int i = 0; i < 3; ++i) {
        ctx.addF(node[i], f[i]);
        for (int j = 0; j < 3; ++j) ctx.addG(node[i], node[j], g[i][j]);
    }
    if (hasCharge_) {
        for (int i = 0; i < 3; ++i) {
            ctx.addQ(node[i], q[i]);
            for (int j = 0; j < 3; ++j) ctx.addC(node[i], node[j], c[i][j]);
        }
    }
}

}  // namespace esim
