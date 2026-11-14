#pragma once

#include <functional>

#include "core/DaeSystem.hpp"
#include "core/Newton.hpp"

namespace esim {

enum class IntegrationMethod { BackwardEuler, Trapezoidal, Bdf2, Bdf };

const char* methodName(IntegrationMethod m);

struct TransientOptions {
    double tstop = 0.0;
    double hmax = 0.0;   // 0: tstop / 50
    double hinit = 0.0;  // 0: hmax / 100
    double hmin = 0.0;   // 0: 1e-7 * hinit
    IntegrationMethod method = IntegrationMethod::Bdf;
    int maxOrder = 5;  // Bdf only: highest order used (1 .. 5)
    // Local truncation error per step. Each unknown has the tolerance
    // reltol * max(|x|, peakFloor * peak of |x| so far) + absTol; the error is
    // measured on the charges q, each against the tolerance its unknowns amount
    // to (for an ODE x' = g, i.e. q = x, that is the tolerance of x itself).
    // peakFloor = 0 is the classical "relative to the current value" (best for
    // decaying solutions, expensive at zero crossings of oscillating ones),
    // peakFloor = 1 is relative to the signal's peak.
    double reltol = 1e-4;
    double peakFloor = 0.1;
    double trtol = 1.0;  // scales the accepted truncation error
    // 0: onStep is called at every accepted step. > 0: onStep is called on the
    // uniform grid 0, outputStep, 2*outputStep, ... with interpolated values.
    double outputStep = 0.0;
    int maxEvents = 100000;  // guard against endless switching (chattering)
    NewtonOptions newton{20, 1e-4};
};

struct TransientStats {
    int accepted = 0;
    int rejected = 0;        // steps rejected by the truncation error estimate
    int newtonFailures = 0;  // steps repeated because Newton did not converge
    int newtonIterations = 0;
    int events = 0;          // located sign changes of event functions
    int maxOrder = 1;        // highest order of an accepted step
};

using StepCallback = std::function<void(double t, const Vector& x)>;

// Integrate d/dt q(x) + f(x, t) = 0 from t = 0 to tstop with a variable-step
// implicit method:
//   Bdf2 (Gear 2)  : fixed order 2. Stiffly stable, no ringing on algebraic
//                    unknowns, slight numerical damping of oscillations.
//   Trapezoidal    : no numerical damping (good for high-Q circuits), but errors
//                    on algebraic unknowns alternate in sign and never decay.
//   BackwardEuler  : first order, very robust, strongly damping.
//   Bdf            : default. BDF with variable order 1 .. 5. After every step the order
//                    that allows the longest next step is chosen. Far fewer
//                    steps for smooth solutions and tight tolerances; orders
//                    above 2 are unsuitable for weakly damped oscillations
//                    (the order control then falls back to 2 or 1).
// Every (re)start, i.e. t = 0, each breakpoint and each event, begins with two
// backward Euler steps. An event is followed by one very short extra step in
// which the system's modes settle (DaeSystem::setLiveModes). The step size is
// controlled by the truncation error of the charges q, estimated from the
// polynomial extrapolation of their history.
// x0 has to be a consistent initial state (normally the DC
// solution). If q0 is given it replaces q(x0) as the initial charge vector,
// which allows prescribed initial conditions on the differential part.
// onStep is called for t = 0 and after every accepted step (or on a uniform
// grid, see TransientOptions::outputStep). At an event it sees the state just
// before the system's eventUpdate().
TransientStats runTransient(DaeSystem& sys, const Vector& x0, const Vector* q0,
                            const TransientOptions& opt, const StepCallback& onStep);

}  // namespace esim
