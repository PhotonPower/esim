#pragma once

#include <complex>
#include <functional>
#include <string>
#include <vector>

#include "circuit/Circuit.hpp"
#include "core/DcSolver.hpp"

namespace esim {

struct SimOptions {
    double reltol = 1e-4;  // Newton convergence and truncation error
    double trtol = 1.0;    // scales the accepted truncation error
    double lteFloor = 0.1; // see TransientOptions::peakFloor
    int maxIterDc = 100;
    int maxIterTran = 20;
};

// DC operating point. Starts from guess (or from the circuit's nodeset) and
// throws if no strategy converges.
Vector solveOperatingPoint(Circuit& ckt, const SimOptions& opt, const Vector* guess = nullptr,
                           DcResult* info = nullptr);

// kind: "dec" / "oct" (points per decade / octave) or "lin" (points in total).
std::vector<double> frequencySweep(const std::string& kind, int points, double fstart,
                                   double fstop);

using AcCallback = std::function<void(double freq, const Eigen::VectorXcd& x)>;

// Small-signal analysis around the operating point xop:
//     (G + j w C) X = rhs,   G = df/dx, C = dq/dx at xop
void runAc(Circuit& ckt, const Vector& xop, const std::vector<double>& freqs,
           const AcCallback& onPoint);

}  // namespace esim
