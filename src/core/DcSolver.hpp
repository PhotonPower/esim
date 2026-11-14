#pragma once

#include <string>

#include "core/DaeSystem.hpp"
#include "core/Newton.hpp"

namespace esim {

struct DcOptions {
    NewtonOptions newton{100, 1e-4};
};

struct DcResult {
    bool converged = false;
    int iterations = 0;    // total Newton iterations over all strategies
    std::string strategy;  // "newton", "pseudo-transient", "gmin stepping" or "source stepping"
};

// Solve the stationary problem f(x, t) = 0, starting from the guess in x.
// If plain Newton fails, the continuation methods the system supports are
// tried in turn: pseudo-transient (damping towards the previous solution, which
// follows the way a circuit settles after power-up), shunt (gmin) stepping and
// source stepping.
DcResult solveDc(DaeSystem& sys, double t, Vector& x, const DcOptions& opt = {});

}  // namespace esim
