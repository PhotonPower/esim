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
    bool reuseJacobian = true;  // see TransientOptions::reuseJacobian
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

struct NoiseSource;
class IndependentSource;

// Noise analysis around the operating point xop. For every frequency:
//   onoise2        spectral density of the output noise, squared (e.g. V^2/Hz)
//   gain           |output / input source| (0 without an input source)
//   contributions  share of every noise source in onoise2 (same order as `sources`)
// The output is x[outPos] - x[outNeg]. Each noise source is a current
// injected between two nodes; its transfer to the output comes from one
// adjoint solve per frequency, (G + jwC)^T y = e_out.
using NoiseCallback = std::function<void(double freq, double onoise2, double gain,
                                         const std::vector<double>& contributions)>;
void runNoise(Circuit& ckt, const Vector& xop, const std::vector<double>& freqs, int outPos,
              int outNeg, const IndependentSource* input, std::vector<NoiseSource>& sources,
              const NoiseCallback& onPoint);

}  // namespace esim
