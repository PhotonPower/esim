#pragma once

#include <algorithm>
#include <cmath>

namespace esim {

// Value with its derivatives with respect to three variables (forward-mode
// differentiation), so that a device model can be written down as a formula
// and its conductances and capacitances follow by themselves.
struct Dual {
    double v = 0.0;
    double d[3] = {0.0, 0.0, 0.0};
    Dual() = default;
    Dual(double value) : v(value) {}  // NOLINT: constants convert implicitly
    static Dual variable(double value, int k) {
        Dual r(value);
        r.d[k] = 1.0;
        return r;
    }
    // f(v) with derivative df.
    Dual chain(double f, double df) const {
        Dual r(f);
        for (int k = 0; k < 3; ++k) r.d[k] = df * d[k];
        return r;
    }
};

inline Dual operator+(const Dual& a, const Dual& b) {
    Dual r(a.v + b.v);
    for (int k = 0; k < 3; ++k) r.d[k] = a.d[k] + b.d[k];
    return r;
}
inline Dual operator-(const Dual& a, const Dual& b) {
    Dual r(a.v - b.v);
    for (int k = 0; k < 3; ++k) r.d[k] = a.d[k] - b.d[k];
    return r;
}
inline Dual operator-(const Dual& a) { return a.chain(-a.v, -1.0); }
inline Dual operator*(const Dual& a, const Dual& b) {
    Dual r(a.v * b.v);
    for (int k = 0; k < 3; ++k) r.d[k] = a.d[k] * b.v + a.v * b.d[k];
    return r;
}
inline Dual operator/(const Dual& a, const Dual& b) {
    Dual r(a.v / b.v);
    for (int k = 0; k < 3; ++k) r.d[k] = (a.d[k] - r.v * b.d[k]) / b.v;
    return r;
}
inline Dual sqrt(const Dual& a) {
    const double s = std::sqrt(a.v);
    return a.chain(s, 0.5 / s);
}
inline Dual exp(const Dual& a) {
    const double e = std::exp(a.v);
    return a.chain(e, e);
}
inline Dual log(const Dual& a) { return a.chain(std::log(a.v), 1.0 / a.v); }
// ln(1 + exp(u)), without overflow.
inline Dual softplus(const Dual& a) {
    const double e = std::exp(-std::abs(a.v));
    const double sig = a.v >= 0.0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
    return a.chain(std::max(a.v, 0.0) + std::log1p(e), sig);
}
// Smooth |u| and max(u, 0); eps sets the width of the rounded corner.
inline Dual smoothAbs(const Dual& a, double eps) { return sqrt(a * a + eps * eps) - eps; }
inline Dual smoothPositive(const Dual& a, double eps) { return 0.5 * (a + sqrt(a * a + 4.0 * eps * eps)); }

}  // namespace esim
