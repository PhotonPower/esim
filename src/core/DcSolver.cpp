#include "core/DcSolver.hpp"

#include <algorithm>

namespace esim {

namespace {

bool gminStepping(DaeSystem& sys, double t, Vector& x, const DcOptions& opt, NewtonSolver& newton,
                  int& iterations) {
    if (!sys.setShunt(0.0)) return false;
    bool ok = true;
    for (double g = 1e-2; g > 1e-13 && ok; g *= 0.1) {
        sys.setShunt(g);
        const NewtonResult r = newton.solve(sys, t, 0.0, nullptr, x, opt.newton);
        iterations += r.iterations;
        ok = r.converged;
    }
    sys.setShunt(0.0);
    if (!ok) return false;
    const NewtonResult r = newton.solve(sys, t, 0.0, nullptr, x, opt.newton);
    iterations += r.iterations;
    return r.converged;
}

// Pseudo-transient continuation: every step solves f(x) + g (x - x_prev) = 0,
// i.e. one implicit Euler step of the system with a capacitor g*dt on every
// damped unknown. g is lowered after a success (longer step) and raised after
// a failure; once the damping is negligible the plain problem is solved.
bool pseudoTransient(DaeSystem& sys, double t, Vector& x, const DcOptions& opt,
                     NewtonSolver& newton, int& iterations) {
    Vector anchor = x, trial;
    if (!sys.setDamping(0.0, &anchor)) return false;
    bool ok = false;
    double g = 1e-2;
    for (int step = 0; step < 3000 && g < 1e9; ++step) {
        if (g < 1e-12) {  // damping negligible: finish without it
            sys.setDamping(0.0, &anchor);
            trial = anchor;
            const NewtonResult r = newton.solve(sys, t, 0.0, nullptr, trial, opt.newton);
            iterations += r.iterations;
            if (r.converged) {
                x = trial;
                ok = true;
                break;
            }
            g = 1e-9;  // not yet settled: continue with some damping
        }
        sys.setDamping(g, &anchor);
        trial = anchor;
        const NewtonResult r = newton.solve(sys, t, 0.0, nullptr, trial, opt.newton);
        iterations += r.iterations;
        if (!r.converged) {
            g *= 10.0;
            continue;
        }
        anchor = trial;
        g *= r.iterations <= 8 ? 0.2 : 0.5;
    }
    sys.setDamping(0.0, nullptr);
    return ok;
}

bool sourceStepping(DaeSystem& sys, double t, Vector& x, const DcOptions& opt,
                    NewtonSolver& newton, int& iterations) {
    if (!sys.setSourceScale(0.0)) return false;
    NewtonResult r = newton.solve(sys, t, 0.0, nullptr, x, opt.newton);
    iterations += r.iterations;
    bool ok = r.converged;

    double scale = 0.0, step = 0.1;
    Vector trial;
    while (ok && scale < 1.0) {
        const double next = std::min(1.0, scale + step);
        sys.setSourceScale(next);
        trial = x;
        r = newton.solve(sys, t, 0.0, nullptr, trial, opt.newton);
        iterations += r.iterations;
        if (r.converged) {
            x = trial;
            scale = next;
            step *= 2.0;
        } else {
            step *= 0.25;
            if (step < 1e-6) ok = false;
        }
    }
    sys.setSourceScale(1.0);
    return ok;
}

}  // namespace

DcResult solveDc(DaeSystem& sys, double t, Vector& x, const DcOptions& opt) {
    NewtonSolver newton;
    DcResult res;
    const Vector guess = x;

    const NewtonResult r = newton.solve(sys, t, 0.0, nullptr, x, opt.newton);
    res.iterations += r.iterations;
    if (r.converged) {
        res.converged = true;
        res.strategy = "newton";
        return res;
    }

    x = guess;
    if (pseudoTransient(sys, t, x, opt, newton, res.iterations)) {
        res.converged = true;
        res.strategy = "pseudo-transient";
        return res;
    }

    x = guess;
    if (gminStepping(sys, t, x, opt, newton, res.iterations)) {
        res.converged = true;
        res.strategy = "gmin stepping";
        return res;
    }

    x = guess;
    if (sourceStepping(sys, t, x, opt, newton, res.iterations)) {
        res.converged = true;
        res.strategy = "source stepping";
        return res;
    }

    x = guess;
    return res;
}

}  // namespace esim
