#pragma once

#include <complex>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/DaeSystem.hpp"

namespace esim {

class Circuit;

// Everything a device needs to add its contribution to
//     d/dt q(x) + f(x, t) = 0.
// Row i of a node is Kirchhoff's current law: f collects the currents leaving
// the node, q the charges whose time derivative leaves the node. Ground has the
// index -1 and is silently skipped by all add* functions.
struct StampContext {
    double t;
    const Vector& x;
    Evaluation& out;
    LimitMode limiting;
    bool transient;   // independent sources use their waveform instead of the DC value
    bool useIc;       // reactive elements report their prescribed initial state
    bool initPoint;   // initial point of a transient: hold prescribed states
    bool liveModes;   // re-initialization step after an event, see frozenModes()
    double srcScale;  // homotopy factor for independent sources
    double gmin;      // conductance in parallel to every pn junction
    double vt;        // thermal voltage kT/q

    double v(int node) const { return node < 0 ? 0.0 : x[node]; }

    // While the integrator steps, switching elements and expressions keep
    // their branch frozen; they switch at events only. In the DC analyses and
    // in the re-initialization step after an event they follow the iterate.
    bool frozenModes() const { return transient && !initPoint && !useIc && !liveModes; }

    void addF(int row, double val) {
        if (row >= 0) out.f[row] += val;
    }
    void addQ(int row, double val) {
        if (row >= 0) out.q[row] += val;
    }
    void addG(int row, int col, double val) {
        if (row >= 0 && col >= 0) out.G.emplace_back(row, col, val);
    }
    void addC(int row, int col, double val) {
        if (row >= 0 && col >= 0) out.C.emplace_back(row, col, val);
    }

    // Current i flowing from node a through the device to node b.
    void stampCurrent(int a, int b, double i) {
        addF(a, i);
        addF(b, -i);
    }
    // d i / d (va - vb) = g for a current stamped with stampCurrent(a, b, i).
    void stampConductance(int a, int b, double g) {
        addG(a, a, g);
        addG(a, b, -g);
        addG(b, a, -g);
        addG(b, b, g);
    }
    // Charge q stored between a and b (positive plate at a).
    void stampCharge(int a, int b, double q) {
        addQ(a, q);
        addQ(b, -q);
    }
    void stampCapacitance(int a, int b, double c) {
        addC(a, a, c);
        addC(a, b, -c);
        addC(b, a, -c);
        addC(b, b, c);
    }
    // Branch with its own current unknown x[br], flowing from p through the
    // device to n. Adds the current to both node equations and "v(p) - v(n)"
    // to the branch equation; the device completes the branch equation.
    void stampBranch(int p, int n, int br) {
        const double i = x[br];
        addF(p, i);
        addF(n, -i);
        addG(p, br, 1.0);
        addG(n, br, -1.0);
        addF(br, v(p) - v(n));
        addG(br, p, 1.0);
        addG(br, n, -1.0);
    }
};

// A noise current source between two nodes (from a to b), with the spectral
// density  white + flicker / f  in A^2/Hz, valid at the operating point of the
// device's last evaluation.
struct NoiseSource {
    std::string name;  // e.g. "R1", "Q1.ic"
    int a, b;
    double white;
    double flicker;
};

constexpr double kBoltzmann = 1.380649e-23;       // J/K
constexpr double kElementaryCharge = 1.602176634e-19;  // C

class Device {
public:
    explicit Device(std::string name) : name_(std::move(name)) {}
    virtual ~Device() = default;

    const std::string& name() const { return name_; }

    // Called once by Circuit::finalize(): first setup() on all devices (allocate
    // internal nodes and branch currents), then bind() (resolve references to
    // other devices).
    virtual void setup(Circuit& /*ckt*/) {}
    virtual void bind(Circuit& /*ckt*/) {}

    virtual void stamp(StampContext& ctx) = 0;

    // Small-signal excitation: contribution to the right-hand side of
    // (G + jwC) X = rhs.
    virtual void stampAc(Eigen::VectorXcd& /*rhs*/) const {}

    // Noise sources at the operating point of the last evaluation.
    virtual void noiseSources(std::vector<NoiseSource>& /*out*/) const {}

    // Index of the branch-current unknown, -1 if the device has none.
    virtual int branch() const { return -1; }

    virtual double nextBreakpoint(double /*t*/) const {
        return std::numeric_limits<double>::infinity();
    }

    virtual double maxStep() const { return std::numeric_limits<double>::infinity(); }

    // Start point (start = true) and accepted steps of a time integration, for
    // devices that refer to their past.
    virtual void acceptStep(double /*t*/, const Vector& /*x*/, bool /*start*/) {}
    virtual bool usesHistory() const { return false; }
    virtual void noteDiscontinuity(double /*t*/) {}

    // Entries of the AC matrix beyond G + jwC of the operating point
    // (frequency-dependent elements).
    virtual void stampAcMatrix(double /*omega*/,
                               std::vector<Eigen::Triplet<std::complex<double>>>& /*out*/) const {}

    // Events, see DaeSystem. eventFunctions() writes eventCount() values to g.
    virtual int eventCount() const { return 0; }
    virtual void eventFunctions(double /*t*/, const Vector& /*x*/, double* /*g*/) const {}
    virtual bool eventUpdate(double /*t*/, Vector& /*x*/, bool /*initial*/) { return false; }

    // Write a prescribed initial condition ("ic") into the start vector.
    virtual void applyIc(Vector& /*x0*/) const {}

    // Write start values for the Newton iteration of a DC solution.
    virtual void applyGuess(Vector& /*x*/) const {}

    // Forget what earlier analyses left behind (switch positions).
    virtual void reset() {}

private:
    std::string name_;
};

}  // namespace esim
