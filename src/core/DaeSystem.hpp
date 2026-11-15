#pragma once

#include <limits>
#include <string>

#include "core/Types.hpp"

namespace esim {

// How a system may treat nonlinear limiting during an evaluation.
//   Off  : plain evaluation at x, no internal state is touched.
//   Init : plain evaluation at x; limiters remember x as their reference point.
//   On   : Newton iteration; limiters may evaluate at a point damped towards
//          their reference, report this through Evaluation::limited and move
//          the reference along.
enum class LimitMode { Off, Init, On };

// Result of evaluating a DAE system at (t, x).
struct Evaluation {
    Vector f;      // f(x, t)
    Vector q;      // q(x)
    Triplets G;    // df/dx (duplicates are summed)
    Triplets C;    // dq/dx (duplicates are summed)
    bool limited = false;  // a limiter was active: Newton must not accept convergence

    void reset(int n) {
        f.setZero(n);
        q.setZero(n);
        G.clear();
        C.clear();
        limited = false;
    }
};

// A differential-algebraic system in charge-oriented (linear-implicit) form
//
//     d/dt q(x) + f(x, t) = 0
//
// Rows with q_i == 0 are algebraic constraints, the others are differential
// equations. Circuits in modified nodal analysis have exactly this shape, and so
// does any ODE system with algebraic side conditions (M x' = g(x, t) with a
// singular mass matrix M is the special case q = M x, f = -g).
//
// The solvers in core/ only talk to this interface.
class DaeSystem {
public:
    virtual ~DaeSystem() = default;

    virtual int size() const = 0;

    // Fill f, q and the Jacobian triplets G = df/dx, C = dq/dx.
    // The sparsity pattern of G and C should not depend on x.
    virtual void evaluate(double t, const Vector& x, LimitMode limiting, Evaluation& out) = 0;

    virtual std::string unknownName(int i) const { return "x" + std::to_string(i); }

    // Absolute tolerance of unknown i (Newton convergence and step-size control).
    virtual double absTol(int /*i*/) const { return 1e-9; }

    // Smallest time > t at which f is not smooth in t (e.g. a pulse edge).
    virtual double nextBreakpoint(double /*t*/) const {
        return std::numeric_limits<double>::infinity();
    }

    // Largest sensible step at time t, e.g. a fraction of the period of a
    // sinusoidal excitation. The integrator would otherwise step over whole
    // periods wherever the differential part of the solution is smooth.
    virtual double maxStep(double /*t*/) const {
        return std::numeric_limits<double>::infinity();
    }

    // --- Events (optional) ----------------------------------------------------
    // Event functions g_k(t, x). The integrator detects their sign changes,
    // ends a step just behind the first crossing and then calls eventUpdate().
    // Use them for everything that makes f non-smooth depending on the state
    // (a branch of an if(), |x|, a switch) and for discrete actions.
    // Between two eventUpdate() calls f, q and g have to be smooth: the system
    // keeps computing with the branch that was valid at the last update, also
    // beyond the crossing. Only eventUpdate() switches branches.
    virtual int eventCount() const { return 0; }
    virtual void eventFunctions(double /*t*/, const Vector& /*x*/, Vector& /*g*/) {}

    // Called once at the start (initial = true, x must not be changed), at
    // every event and at every breakpoint (for actions at given times). The system may switch internal modes and reset entries of
    // x; it returns true if it changed x. The integration restarts afterwards.
    virtual bool eventUpdate(double /*t*/, Vector& /*x*/, bool /*initial*/) { return false; }

    // Right after an event the integrator takes one very short step with
    // "live" modes: branches follow the current Newton iterate instead of
    // staying frozen. This lets modes that depend on each other settle (a
    // switch opens, so a diode has to conduct) before eventUpdate() freezes
    // them again.
    virtual void setLiveModes(bool /*on*/) {}

    // Called by a time integration at its start point (start = true) and after
    // every accepted step, for systems that refer to their past (delays).
    virtual void acceptStep(double /*t*/, const Vector& /*x*/, bool /*start*/) {}
    // The integration restarted at t (a breakpoint or an event): the solution
    // may have a kink or jump there.
    virtual void noteDiscontinuity(double /*t*/) {}

    // Optional homotopy hooks used by the DC solver when plain Newton fails.
    // Return false if the system does not support the respective continuation.
    virtual bool setSourceScale(double /*scale*/) { return false; }  // excitations * scale
    virtual bool setShunt(double /*g*/) { return false; }            // f += g * x (regularization)
    // Damping towards a given state: f += g * (x - anchor) on the unknowns the
    // system chooses (g = 0 switches it off). Stepping from one anchor to the
    // next with decreasing g is a pseudo-transient continuation.
    virtual bool setDamping(double /*g*/, const Vector* /*anchor*/) { return false; }
};

}  // namespace esim
