#include "core/Transient.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace esim {

const char* methodName(IntegrationMethod m) {
    switch (m) {
        case IntegrationMethod::BackwardEuler: return "be";
        case IntegrationMethod::Trapezoidal: return "trap";
        case IntegrationMethod::Bdf2: return "gear2";
        case IntegrationMethod::Bdf: return "bdf";
    }
    return "?";
}

namespace {

constexpr int kMaxOrder = 5;
constexpr int kHist = kMaxOrder + 2;  // history points kept (enough to judge order + 1)

std::runtime_error stepError(double t, const std::string& why) {
    std::ostringstream os;
    os << "transient: time step too small at t = " << t << " (" << why << ")";
    return std::runtime_error(os.str());
}

// An event function has crossed zero between two points if its sign differs
// ("positive" versus "not positive").
bool crossed(const Vector& a, const Vector& b) {
    for (Eigen::Index k = 0; k < a.size(); ++k)
        if ((a[k] > 0.0) != (b[k] > 0.0)) return true;
    return false;
}

double factorial(int k) {
    double f = 1.0;
    for (int i = 2; i <= k; ++i) f *= i;
    return f;
}

}  // namespace

TransientStats runTransient(DaeSystem& sys, const Vector& x0, const Vector* q0,
                            const TransientOptions& opt, const StepCallback& onStep) {
    if (!(opt.tstop > 0.0)) throw std::invalid_argument("transient: tstop must be > 0");
    const int n = sys.size();
    if (x0.size() != n) throw std::invalid_argument("transient: x0 has the wrong size");

    const double tstop = opt.tstop;
    const double hmax = opt.hmax > 0.0 ? std::min(opt.hmax, tstop) : tstop / 50.0;
    const double hinit = opt.hinit > 0.0 ? std::min(opt.hinit, hmax) : hmax / 100.0;
    const double hmin = opt.hmin > 0.0 ? opt.hmin : 1e-7 * hinit;
    const bool trap = opt.method == IntegrationMethod::Trapezoidal;
    const bool variableOrder = opt.method == IntegrationMethod::Bdf;
    const int maxOrder = std::min(std::max(opt.maxOrder, 1), kMaxOrder);

    Vector abstol(n);
    for (int i = 0; i < n; ++i) abstol[i] = sys.absTol(i);

    // History, newest first: xh[0] = x(t_n), xh[1] = x(t_{n-1}), ... at the
    // times th[0], th[1], ...; qh holds the charges. nhist entries are usable
    // (the history is dropped at every restart).
    std::array<Vector, kHist> xh, qh;
    std::array<double, kHist> th{};
    xh[0] = x0;
    int nhist = 1;
    double t = 0.0;

    // Let the system set up its discrete modes for the start point.
    {
        Vector xe = x0;
        sys.eventUpdate(0.0, xe, true);
    }
    sys.acceptStep(0.0, x0, true);

    // Evaluations at accepted points use LimitMode::Init: the limiters then
    // refer to the last accepted solution when the next step starts.
    Evaluation ev;
    sys.evaluate(0.0, x0, LimitMode::Init, ev);
    bool limiterAtXn = true;
    qh[0] = q0 ? *q0 : ev.q;
    Vector qdot = Vector::Zero(n);  // d/dt q at t_n (trapezoidal rule only)
    // f at t_n. Since d/dt q = -f, it gives the slope of the charges at the
    // current point, which is what the error estimate of the first step after
    // a restart is built on (there is no history to extrapolate yet).
    Vector fn = ev.f;

    // The truncation error is controlled on the charges q, the quantities the
    // method actually integrates, not on the unknowns x:
    //  - Algebraic unknowns are left out. They follow from the constraints, and
    //    with the trapezoidal rule their error alternates without ever decaying.
    //  - A charge like C (v1 - v2) stays accurate when v1 and v2 share a poorly
    //    determined common part (a capacitor in a section of the circuit that
    //    floats against ground), where each voltage alone is dominated by
    //    rounding noise that grows as the step shrinks.
    // The tolerance of a charge is what the tolerances of the unknowns it
    // depends on amount to: tol_q = |dq/dx| * tol_x.
    //
    // Largest magnitude each unknown has had so far. A fraction of it is the
    // floor of the reference of the relative tolerance, which would otherwise
    // shrink to abstol at every zero crossing of a signal.
    Vector xpeak = x0.cwiseAbs();
    Vector qp(n), tolx(n), tolq(n);
    Vector xp(n), xnew(n), b(n), xi(n);  // predictor, step result, history term, scratch

    // Events: sign pattern of the event functions at t_n.
    const int nev = sys.eventCount();
    Vector gn(nev), glo(nev), ghi(nev), gtry(nev);
    if (nev > 0) sys.eventFunctions(0.0, x0, gn);

    // Step size right after a breakpoint, where no history is available.
    auto restartStep = [&](double tnow) {
        const double tnext = std::min(sys.nextBreakpoint(tnow + hmin), tstop);
        return std::max(hmin, std::min(hinit, 0.1 * (tnext - tnow)));
    };

    // Value at time ta of the polynomial through the newest m history points.
    auto extrapolate = [&](const std::array<Vector, kHist>& v, int m, double ta, Vector& out) {
        out.setZero(n);
        for (int j = 0; j < m; ++j) {
            double w = 1.0;
            for (int i = 0; i < m; ++i)
                if (i != j) w *= (ta - th[i]) / (th[j] - th[i]);
            out += w * v[j];
        }
    };

    // Value at time ta of the polynomial through the step result (tn, xnew)
    // and the newest m history points: the solution inside the current step.
    auto interpolateStep = [&](int m, double tn, double ta, Vector& out) {
        double w = 1.0;
        for (int i = 0; i < m; ++i) w *= (ta - th[i]) / (tn - th[i]);
        out = w * xnew;
        for (int j = 0; j < m; ++j) {
            w = (ta - tn) / (th[j] - tn);
            for (int i = 0; i < m; ++i)
                if (i != j) w *= (ta - th[i]) / (th[j] - th[i]);
            out += w * xh[j];
        }
    };

    // BDF of order k for a step to tn: the derivative at tn of the polynomial
    // through the new point and the newest k history points,
    //     d/dt q(tn) ~ lead * q_new + sum_j alpha[j] * qh[j].
    auto bdfCoefficients = [&](int k, double tn, double& lead, std::array<double, kMaxOrder>& alpha) {
        lead = 0.0;
        for (int m = 0; m < k; ++m) lead += 1.0 / (tn - th[m]);
        for (int j = 0; j < k; ++j) {
            double num = 1.0, den = th[j] - tn;
            for (int m = 0; m < k; ++m) {
                if (m == j) continue;
                num *= tn - th[m];
                den *= th[j] - th[m];
            }
            alpha[j] = num / den;
        }
    };

    // Ratio between the truncation error of a step of order k to tn and the
    // difference "result minus extrapolation through k + 1 history points".
    auto errorFactor = [&](int k, double tn, bool trapezoidal) {
        double prod = 1.0;  // (tn - t_n) ... (tn - t_{n-k+1})
        for (int m = 0; m < k; ++m) prod *= tn - th[m];
        const double errP = prod * (tn - th[k]) / factorial(k + 1);
        double errC;
        if (trapezoidal) {
            const double hstep = tn - th[0];
            errC = hstep * hstep * hstep / 12.0;
        } else {
            double lead = 0.0;
            for (int m = 0; m < k; ++m) lead += 1.0 / (tn - th[m]);
            errC = prod / (factorial(k + 1) * lead);
        }
        return errC / (errC + errP);
    };

    TransientStats stats;

    // Output: every accepted point, or a uniform grid filled by interpolating
    // the history polynomial (quadratic once three points are available).
    long long nextSample = 0;
    auto output = [&]() {
        if (!onStep) return;
        if (!(opt.outputStep > 0.0)) {
            onStep(t, xh[0]);
            return;
        }
        for (;; ++nextSample) {
            const double ts = static_cast<double>(nextSample) * opt.outputStep;
            if (ts > t + 1e-12 * tstop) break;
            if (nhist < 2 || ts > t - 1e-12 * tstop) {
                onStep(ts, xh[0]);
                continue;
            }
            extrapolate(xh, std::min(nhist, 3), ts, xi);
            onStep(ts, xi);
        }
    };
    output();

    double h = restartStep(t);

    NewtonSolver newton;
    NewtonOptions nopt = opt.newton;
    nopt.limitFromMemory = true;
    nopt.reuseJacobian = opt.reuseJacobian;
    int holdGrowth = 0;     // accepted steps without growth after a failed step
    int nextOrder = 1;      // variable order: order to use for the next step
    int stepsAtOrder = 0;   // accepted steps since the order was changed

    // One implicit step from t_n to tn: fills hs, order, a0, b, xp, xnew and
    // leaves the evaluation at xnew in ev.
    double hs = 0.0, a0 = 0.0;
    int order = 1;
    std::array<double, kMaxOrder> alpha{};
    auto attempt = [&](double tn) {
        hs = tn - t;
        if (variableOrder) {
            // The error estimate of order k needs k + 1 history points.
            order = std::max(1, std::min({nextOrder, maxOrder, nhist - 1}));
        } else {
            // After a (re)start: two backward Euler steps, then the 2nd-order method.
            order = (opt.method != IntegrationMethod::BackwardEuler && nhist >= 3) ? 2 : 1;
        }

        // Predictor: polynomial extrapolation of the history. It is the Newton
        // start value and the reference of the truncation error estimate.
        extrapolate(xh, std::min(nhist, order + 1), tn, xp);

        // Corrector: d/dt q(t_{n+1}) ~ a0 * q(x_{n+1}) + b.
        if (trap && order == 2) {
            a0 = 2.0 / hs;
            b = -a0 * qh[0] - qdot;
        } else {
            bdfCoefficients(order, tn, a0, alpha);
            b = alpha[0] * qh[0];
            for (int j = 1; j < order; ++j) b += alpha[j] * qh[j];
        }

        if (!limiterAtXn) sys.evaluate(t, xh[0], LimitMode::Init, ev);  // retry: restore memory
        limiterAtXn = false;
        xnew = xp;
        const NewtonResult nr = newton.solve(sys, tn, a0, &b, xnew, nopt);
        stats.newtonIterations += nr.iterations;
        stats.factorizations += nr.factorizations;
        if (!nr.converged) return false;
        // f, q and dq/dx at the result: Newton already has them if it stopped
        // right after an evaluation (always so for a linear system).
        if (nr.evaluatedAtSolution) std::swap(ev, newton.evaluation());
        else sys.evaluate(tn, xnew, LimitMode::Init, ev);
        return true;
    };

    auto newtonFailed = [&](double hTried) {
        ++stats.newtonFailures;
        holdGrowth = 2;
        if (nextOrder > 1) {
            --nextOrder;
            stepsAtOrder = 0;
        }
        h = 0.25 * hTried;
        if (h < hmin) throw stepError(t, "Newton iteration does not converge");
    };

    // Scaled truncation error of the step in ev (ending at tn) if it had been
    // taken with order k. Needs k + 1 history points and tolq. With a single
    // history point (first step after a restart, backward Euler) the reference
    // is the tangent q_n - h f_n instead of an extrapolation; the error of the
    // step is half the distance to it.
    auto scaledError = [&](int k, double tn, bool trapezoidal, int* worst) {
        double factor;
        if (nhist == 1) {
            qp = qh[0] - (tn - t) * fn;
            factor = 0.5;
        } else {
            extrapolate(qh, k + 1, tn, qp);
            factor = errorFactor(k, tn, trapezoidal);
        }
        double err = 0.0;
        for (int i = 0; i < n; ++i) {
            if (!(tolq[i] > 0.0)) continue;  // algebraic equation
            const double e = factor * std::abs(ev.q[i] - qp[i]) / (opt.trtol * tolq[i]);
            if (e > err) {
                err = e;
                if (worst) *worst = i;
            }
        }
        return err;
    };

    // Step size that a truncation error err of order k allows.
    auto stepFor = [&](double err, int k) {
        return hs * std::min(2.0, 0.9 * std::pow(err, -1.0 / (k + 1)));
    };

    // Take over the step held in xnew, a0, b.
    auto commit = [&](double tn) {
        limiterAtXn = true;  // attempt() has evaluated the system at xnew
        qdot = a0 * ev.q + b;
        std::rotate(xh.rbegin(), xh.rbegin() + 1, xh.rend());
        std::rotate(qh.rbegin(), qh.rbegin() + 1, qh.rend());
        std::rotate(th.rbegin(), th.rbegin() + 1, th.rend());
        xh[0] = xnew;
        qh[0] = ev.q;
        fn = ev.f;
        th[0] = tn;
        nhist = std::min(nhist + 1, kHist);
        xpeak = xpeak.cwiseMax(xnew.cwiseAbs());
        t = tn;
        ++stats.accepted;
        stats.maxOrder = std::max(stats.maxOrder, order);
        sys.acceptStep(tn, xnew, false);
        output();
    };

    auto restart = [&]() {
        nhist = 1;
        nextOrder = 1;
        stepsAtOrder = 0;
        h = restartStep(t);
    };

    // Let the system switch modes and reset states at the current point, then
    // restart the integration (the right-hand side has a kink or jump here).
    // Returns true if states were reset.
    auto switchModes = [&]() {
        xnew = xh[0];
        const bool reset = sys.eventUpdate(t, xnew, false);
        if (reset) {
            xh[0] = xnew;
            sys.evaluate(t, xnew, LimitMode::Init, ev);
            limiterAtXn = true;
            qh[0] = ev.q;
            fn = ev.f;
            xpeak = xpeak.cwiseMax(xnew.cwiseAbs());
            sys.acceptStep(t, xnew, false);  // the state after the jump
        }
        if (nev > 0) sys.eventFunctions(t, xh[0], gn);
        sys.noteDiscontinuity(t);
        restart();
        return reset;
    };

    auto countEvent = [&]() {
        if (++stats.events <= opt.maxEvents) return;
        std::ostringstream os;
        os << "transient: more than " << opt.maxEvents << " events up to t = " << t
           << "; the model probably switches back and forth without end (chattering)";
        throw std::runtime_error(os.str());
    };

    // After an event the modes are re-established by one very short step with
    // live modes, see DaeSystem::setLiveModes(). The same step makes the
    // algebraic unknowns consistent, which is also needed at the start if the
    // initial charges are prescribed (the start vector then need not satisfy
    // the algebraic equations).
    bool reinit = nev > 0 || q0 != nullptr;

    while (t < tstop) {
        if (reinit) {
            reinit = false;
            const double tr = std::min(t + std::max(hmin, 1e-4 * hinit), tstop);
            sys.setLiveModes(true);
            const bool ok = attempt(tr);
            sys.setLiveModes(false);
            if (!ok) {  // carry on with the frozen modes
                ++stats.newtonFailures;
                continue;
            }
            commit(tr);
            if (switchModes()) {  // the settled state triggered an assignment
                countEvent();
                reinit = true;
            }
            continue;
        }

        h = std::min({h, hmax, std::max(hmin, sys.maxStep(t))});

        // Land exactly on breakpoints and avoid a tiny step in front of them.
        const double tbp = std::min(sys.nextBreakpoint(t + hmin), tstop);
        bool hitBp = false;
        double tn;
        if (t + 1.0001 * h >= tbp) {
            tn = tbp;
            hitBp = true;
        } else if (t + 2.0 * h > tbp) {
            tn = t + 0.5 * (tbp - t);
        } else {
            tn = t + h;
        }

        if (!attempt(tn)) {
            newtonFailed(hs);
            continue;
        }

        // Local truncation error from the difference between the charges and
        // their extrapolation from the history.
        double err = 0.0;
        const bool trapStep = trap && order == 2;
        {
            for (int j = 0; j < n; ++j) {
                const double ref = std::max({std::abs(xnew[j]), std::abs(xh[0][j]),
                                             opt.peakFloor * xpeak[j]});
                tolx[j] = opt.reltol * ref + abstol[j];
            }
            tolq.setZero();
            for (const Triplet& c : ev.C) tolq[c.row()] += std::abs(c.value()) * tolx[c.col()];

            int worst = -1;
            err = scaledError(order, tn, trapStep, &worst);
            if (err > 1.0) {
                ++stats.rejected;
                h = hs * std::max(0.1, 0.9 * std::pow(err, -1.0 / (order + 1)));
                // A lower order may allow a longer step where the solution is not smooth.
                if (variableOrder && order > 1) {
                    const double errLow = scaledError(order - 1, tn, false, nullptr);
                    const double hLow = hs * std::max(0.1, 0.9 * std::pow(errLow, -1.0 / order));
                    if (hLow > h) {
                        h = std::min(hLow, hs);
                        nextOrder = order - 1;
                        stepsAtOrder = 0;
                    }
                }
                if (h < hmin)
                    throw stepError(t, "truncation error cannot be met in the equation of " +
                                           sys.unknownName(worst));
                continue;
            }
        }

        // Events: if an event function changed its sign during the step, shorten
        // the step until it ends just behind the first crossing. Every trial is
        // a complete step from t_n, so the event point is a true solution point.
        bool eventHit = false;
        if (nev > 0) {
            sys.eventFunctions(tn, xnew, ghi);
            bool crossing = crossed(gn, ghi);
            if (!crossing) {
                // An event function may cross zero and return within the step
                // (a diode conducts for a short interval). Look at the solution
                // inside the step, and if a crossing shows up, end the step there.
                const double tEnd = tn;
                const int points = std::min(nhist, order + 1);
                bool failed = false;
                for (double part : {0.25, 0.5, 0.75}) {
                    const double ts = t + part * (tEnd - t);
                    interpolateStep(points, tEnd, ts, xi);
                    sys.eventFunctions(ts, xi, gtry);
                    if (!crossed(gn, gtry)) continue;
                    if (!attempt(ts)) {
                        failed = true;
                        break;
                    }
                    tn = ts;
                    hitBp = false;
                    sys.eventFunctions(tn, xnew, ghi);
                    crossing = crossed(gn, ghi);  // else: a false alarm, the shorter step stands
                    break;
                }
                if (failed) {
                    newtonFailed(tEnd - t);
                    continue;
                }
            }
            if (crossing) {
                const double tolT = std::max({hmin, 1e-8 * hs, 8.0 * DBL_EPSILON * std::abs(tn)});
                double lo = t, hi = tn;  // no crossing up to lo, crossing up to hi
                double solved = tn;      // end of the step currently held in xnew, a0, b
                glo = gn;
                bool aimBehind = true;   // aim just behind / just before the estimated root
                bool failed = false;
                for (int iter = 0; hi - lo > tolT && iter < 100; ++iter) {
                    // Earliest root of the secants through (lo, glo) and (hi, ghi).
                    double root = hi;
                    for (int k = 0; k < nev; ++k) {
                        if ((glo[k] > 0.0) == (ghi[k] > 0.0)) continue;
                        root = std::min(root, lo + (hi - lo) * glo[k] / (glo[k] - ghi[k]));
                    }
                    double trial = root + (aimBehind ? 0.4 : -0.4) * tolT;
                    if (iter % 3 == 2) trial = 0.5 * (lo + hi);  // safeguard: bisection
                    const double margin = 0.05 * (hi - lo);
                    trial = std::min(std::max(trial, lo + margin), hi - margin);

                    if (!attempt(trial)) {
                        failed = true;
                        break;
                    }
                    solved = trial;
                    sys.eventFunctions(trial, xnew, gtry);
                    if (crossed(gn, gtry)) {
                        hi = trial;
                        ghi = gtry;
                        aimBehind = false;
                    } else {
                        lo = trial;
                        glo = gtry;
                        aimBehind = true;
                    }
                }
                if (!failed && solved != hi) failed = !attempt(hi);
                if (failed) {
                    newtonFailed(tn - t);
                    continue;
                }
                tn = hi;
                eventHit = true;
            }
        }

        // Next step size.
        double hNext = stepFor(err, order);
        if (variableOrder && !eventHit && !hitBp) {
            // Variable order: take the order that allows the longest step. It is
            // changed by one at most, and raised only after it has been kept for
            // a few steps, so that the history is smooth enough to judge.
            // gain(e, k): factor by which a step of order k with error e may grow.
            auto gain = [](double e, int k) { return std::pow(2.0 * e + 1e-4, -1.0 / (k + 1)); };
            int best = order;
            double ratio = gain(err, order);
            if (order > 1) {
                const double low = gain(scaledError(order - 1, tn, false, nullptr), order - 1);
                if (low > 1.2 * ratio) {
                    best = order - 1;
                    ratio = low;
                }
            }
            if (best == order && order < maxOrder && nhist >= order + 2 &&
                stepsAtOrder >= order) {
                const double high = gain(scaledError(order + 1, tn, false, nullptr), order + 1);
                if (high > 1.2 * ratio) {
                    best = order + 1;
                    ratio = high;
                }
            }
            if (best != order) stepsAtOrder = -1;
            nextOrder = best;
            // Keep the step size unless it can be doubled or has to shrink:
            // formulas of higher order do not tolerate a steadily growing step.
            if (ratio >= 2.0) hNext = 2.0 * hs;
            else if (ratio <= 1.0) hNext = hs * std::max(0.5, std::min(0.9, ratio));
            else hNext = hs;
        }

        commit(tn);
        ++stepsAtOrder;

        if (eventHit) {
            countEvent();
            switchModes();
            reinit = true;
        } else {
            if (nev > 0) gn = ghi;
            if (hitBp && t < tstop) {
                // Derivatives jump at a breakpoint: drop the history. The system
                // may also act at this time (a sampled controller updates).
                if (switchModes()) {
                    countEvent();
                    reinit = true;
                }
            } else {
                h = hNext;
                if (holdGrowth > 0) {
                    --holdGrowth;
                    h = std::min(h, hs);
                }
            }
        }
    }
    return stats;
}

}  // namespace esim
