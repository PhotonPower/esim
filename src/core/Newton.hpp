#pragma once

#include "core/DaeSystem.hpp"
#include "core/LinearSolver.hpp"

namespace esim {

struct NewtonOptions {
    int maxIter = 50;
    double reltol = 1e-4;
    // The limiter memory of the system already refers to a nearby solution
    // (set by an evaluation with LimitMode::Init): limit from the first
    // iteration on instead of trusting the start value.
    bool limitFromMemory = false;
    // Keep the factorized Jacobian of the first iteration for the following
    // ones as long as they converge quickly (modified Newton). Meant for the
    // steps of a time integration, which start close to their solution; the
    // matrix is factorized anew as soon as an iteration gains less than a
    // factor of two, or a device limits its voltages.
    bool reuseJacobian = false;
};

struct NewtonResult {
    bool converged = false;
    bool singular = false;  // Jacobian singular or residual not finite
    int iterations = 0;
    int factorizations = 0;
    // The last evaluation was made at the returned x (nothing was changed
    // after it), so NewtonSolver::evaluation() holds f, q, G, C of the solution.
    bool evaluatedAtSolution = false;
};

// Newton-Raphson for the nonlinear system
//
//     f(x, t) + a * q(x) + b = 0
//
// which covers the DC problem (a = 0, b = 0) as well as every implicit
// integration formula (d/dt q ~ a * q(x) + b, b built from the history).
class NewtonSolver {
public:
    NewtonResult solve(DaeSystem& sys, double t, double a, const Vector* b, Vector& x,
                       const NewtonOptions& opt);
    Evaluation& evaluation() { return ev_; }

private:
    // J = G + a*C, written into the stored pattern of J_ through slot_.
    void assemble(double a, int n);

    Evaluation ev_;
    LinearSolver lin_;
    Triplets jt_;
    SparseMatrix J_;
    // slot_[k]: position in the value array of J_ of the k-th triplet (those of
    // G first, then those of C). Valid while the systems stamps the same
    // entries in the same order, which is checked through key_.
    std::vector<int> slot_;
    std::vector<std::pair<int, int>> key_;
    Vector r_, dx_, abstol_, scale_;
};

}  // namespace esim
