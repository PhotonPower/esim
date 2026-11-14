#pragma once

#include <complex>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "circuit/Circuit.hpp"
#include "expr/Expression.hpp"

namespace esim {

// An output quantity: either the linear combination x[pos] - x[neg] (index -1
// means 0 / ground) or a general expression of the unknowns and time.
struct Probe {
    std::string label;
    int pos = -1;
    int neg = -1;
    std::shared_ptr<const expr::Program> expression;

    double value(const Vector& x, double t = 0.0) const {
        if (expression) return expression->eval(x, t);
        return (pos >= 0 ? x[pos] : 0.0) - (neg >= 0 ? x[neg] : 0.0);
    }

    // Small-signal phasor (AC analysis); only defined for linear probes.
    std::complex<double> phasor(const Eigen::VectorXcd& x) const;
};

// "v(node)", "v(a,b)", "i(component)", the name of an equation variable, or
// any expression of those, e.g. "v(a)*i(V1)". Branch currents exist for
// voltage sources, inductors, E/H sources and behavioral voltage sources.
Probe parseProbe(const Circuit& ckt, const std::string& expr);

// Probes for the given expressions; for an empty list all node voltages and
// branch currents (without device-internal nodes).
std::vector<Probe> makeProbes(const Circuit& ckt, const std::vector<std::string>& exprs);

class CsvWriter {
public:
    CsvWriter(const std::string& path, const std::vector<std::string>& header);
    void row(const std::vector<double>& values);

private:
    std::ofstream out_;
};

}  // namespace esim
