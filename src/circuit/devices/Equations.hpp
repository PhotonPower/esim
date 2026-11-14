#pragma once

#include <string>
#include <vector>

#include "circuit/Device.hpp"
#include "expr/Expression.hpp"

namespace esim {

struct VariableSpec {
    std::string name;
    double start = 0.0;    // initial value or start value for Newton
    bool hold = false;     // true: start is a prescribed initial value
    double abstol = -1.0;  // < 0: options.vartol
    bool discrete = false; // constant between events (its equation is der(x) = 0)
};

// An equation "lhs = rhs": its text (for messages) and lhs - rhs.
struct EquationSpec {
    std::string text;
    expr::AstPtr residual;
};

// An event: assignments that are carried out
//   - when a condition becomes true          ("when"),
//   - periodically at start, start + T, ...  ("every"), or
//   - at given times                         ("at").
struct WhenSpec {
    struct Assignment {
        std::string variable;  // variable name, "v(node)" or "i(component)"
        std::string text;
        expr::AstPtr value;
    };
    std::string text;            // for messages
    expr::AstPtr condition;      // null for time events
    double period = 0.0;         // > 0: periodic
    double start = 0.0;
    std::vector<double> times;   // single time events, increasing
    std::vector<Assignment> assignments;
};

// A block of user-defined equations
//
//     d/dt q_k(x) + f_k(x, t) = 0,     k = 1 .. number of variables
//
// given as text ("der(x) = -x + y", "0 = x + y - 1"). Each variable becomes an
// unknown of the system and each equation one of its rows, so the block can
// stand alone (a plain DAE) or be coupled to a circuit: the equations may read
// v(node) and i(component), and behavioral sources may read the variables.
//
// Initial values. A variable is a state if it appears inside a der(). For the
// initial point of a transient analysis each state with a prescribed initial
// value is held at that value in place of one differential equation; all other
// unknowns follow from the remaining equations (consistent initialization).
// States without a prescribed value start in steady state.
//
// Events. Every comparison, abs, sign, min and max in the equations is watched:
// the integrator ends a step where its outcome changes. A when-clause fires at
// the moment its condition becomes true and then assigns new values to states
// (all right-hand sides are evaluated before the first assignment). Discrete
// variables are states that only change through such assignments. Events may
// also fire at given times or periodically (sampled controllers), and they may
// assign node voltages and branch currents of the circuit, which makes sense
// for the voltage of a capacitor and the current of an inductor.
class EquationBlock final : public Device {
public:
    EquationBlock(std::string name, std::vector<VariableSpec> variables,
                  std::vector<EquationSpec> equations, std::vector<WhenSpec> whens = {});
    void setup(Circuit& ckt) override;
    void bind(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void applyIc(Vector& x0) const override { applyGuess(x0); }
    void applyGuess(Vector& x) const override;
    double nextBreakpoint(double t) const override;
    int eventCount() const override { return eventCount_; }
    void eventFunctions(double t, const Vector& x, double* g) const override;
    bool eventUpdate(double t, Vector& x, bool initial) override;

private:
    struct Row {
        expr::DaeEquation eq;
        int held = -1;         // variable held by this row at the initial point, -1: none
        bool heldInDc = false; // also held in DC analyses (discrete variable)
    };
    struct When {
        bool timed = false;
        expr::Program condition;
        double period = 0.0, start = 0.0;
        std::vector<double> times;
        double next = 0.0;    // timed: next time at which it fires
        std::vector<std::pair<int, expr::Program>> assignments;  // unknown, new value
        bool active = false;  // value of the condition at the last event

        double timeAfter(double t) const;  // first firing time > t
    };

    void assignHeldStates();
    void updateModes(const Vector& x, double t);

    std::vector<VariableSpec> vars_;
    std::vector<EquationSpec> text_;
    std::vector<int> index_;  // unknown of variable k = row of equation k
    std::vector<Row> rows_;
    std::vector<WhenSpec> whenText_;
    std::vector<When> whens_;
    int eventCount_ = 0;
    std::vector<double> grad_;
};

}  // namespace esim
