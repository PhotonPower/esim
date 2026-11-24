#pragma once

#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "core/History.hpp"
#include "core/Types.hpp"

// Arithmetic expressions given as text: parsing, evaluation and exact
// derivatives (reverse-mode automatic differentiation).
//
// Syntax
//   numbers      1.5  2e-3  4.7k  100n       (SI prefixes f p n u m k M meg G T)
//   operators    + - * / ^ (or **)   < <= > >= == !=   && || !
//   functions    sin cos tan asin acos atan atan2 sinh cosh tanh exp log ln log10
//                sqrt abs sign min max pow if(condition, a, b)
//   tables       name(x) for a table registered under that name,
//                name(x, y) for a table over a grid
//   own          name(a, b, ...) for a function defined by the user
//   vectors      x[i]: element i of a vector; the index is a constant expression
//   sum          sum(i, from, to, e): e summed over i = from .. to
//   delay        delay(e, td): the value e had the constant time td ago. Outside
//                of a time integration (operating point, AC) it is e itself.
//   symbols      names of unknowns and constants, t (or time), pi
//   circuit      v(node)  v(a,b)  i(component)
//   equations    der(expression) for a time derivative
namespace esim::expr {

struct Ast;
using AstPtr = std::shared_ptr<const Ast>;

struct Ast {
    enum class Kind { Number, Symbol, Unary, Binary, Call, Probe };
    Kind kind = Kind::Number;
    double number = 0.0;
    std::string name;                // symbol, operator, function; Probe: "v" or "i"
    std::vector<AstPtr> args;        // operands / function arguments; Symbol: the index of x[i]
    std::vector<std::string> names;  // Probe: node or component names
};

// Both throw std::runtime_error with the position of a syntax error.
// Names may contain dots ("U1.T": something inside the subcircuit instance U1).
AstPtr parseExpression(const std::string& text);
AstPtr parseEquation(const std::string& text);  // "lhs = rhs", returned as lhs - rhs

// Copy of an expression in which symbols and v()/i() accesses are replaced.
// The callback gets every Symbol and Probe node and returns the replacement,
// or nullptr to keep the node.
AstPtr mapLeaves(const AstPtr& ast, const std::function<AstPtr(const Ast&)>& replace);
AstPtr makeNumber(double value);

// A function y(x) given by data points, e.g. a measured characteristic.
class Table {
public:
    enum class Interp { Linear, Cubic };  // Cubic: monotone, continuous slope (PCHIP)
    enum class Extrap { Hold, Linear };   // outside the data: last value / last slope

    // x has to be strictly increasing; at least two points.
    Table(std::vector<double> x, std::vector<double> y, Interp interp, Extrap extrap);
    double eval(double x, double& slope) const;

private:
    std::vector<double> x_, y_, d_;  // d_: slopes at the points (Cubic)
    Interp interp_;
    Extrap extrap_;
};

// A function z(x, y) given on a grid, e.g. a map of flux linkage over current
// and position. Linear: bilinear. Cubic: tensor product of cubic Hermite
// interpolants with slopes from three neighbouring points; continuous slope,
// and linear in the data, so that the derivatives are exact derivatives of the
// interpolant. Used in expressions as name(x, y).
class Table2 {
public:
    using Interp = Table::Interp;
    using Extrap = Table::Extrap;

    // x and y strictly increasing, at least two points each; z[i * y.size() + j]
    // is the value at (x[i], y[j]). The extrapolation applies to both axes.
    Table2(std::vector<double> x, std::vector<double> y, std::vector<double> z, Interp interp,
           Extrap extrap);
    double eval(double x, double y, double& dzdx, double& dzdy) const;

private:
    std::vector<double> x_, y_, z_;
    std::vector<double> dy_;  // slopes along y of every row of z (Cubic)
    Interp interp_;
    Extrap extrap_;
};

// A function defined by the user, e.g. sat(x, lim) = max(-lim, min(lim, x)).
// A call is replaced by the body with the arguments put in place of the
// parameters; other names in the body are resolved where the call stands.
struct FunctionDef {
    std::vector<std::string> params;
    AstPtr body;
};

bool isBuiltinFunction(const std::string& name);

struct DelayLine;

// Meaning of the names in an expression. Unknowns are identified by their
// index in the solution vector.
class Symbols {
public:
    virtual ~Symbols() = default;
    virtual const FunctionDef* function(const std::string& /*name*/) const { return nullptr; }
    // Takes charge of the history of a delay() in an expression: whoever
    // integrates over time has to record it. false: delays are not possible here.
    virtual bool addDelay(const std::shared_ptr<DelayLine>& /*line*/) const { return false; }
    // A table that can be called like a function, or nullptr.
    virtual std::shared_ptr<const Table> table(const std::string& /*name*/) const {
        return nullptr;
    }
    // A table of two arguments, called like name(x, y), or nullptr.
    virtual std::shared_ptr<const Table2> table2(const std::string& /*name*/) const {
        return nullptr;
    }
    virtual bool constant(const std::string& name, double& value) const = 0;
    virtual int variable(const std::string& name) const = 0;  // -1: not a variable
    // Index of the node voltage (-1 for ground) / branch current; throw if unknown.
    virtual int nodeVoltage(const std::string& node) const = 0;
    virtual int branchCurrent(const std::string& component) const = 0;
};

// Symbols consisting of named constants only.
class ConstantSymbols final : public Symbols {
public:
    explicit ConstantSymbols(const std::map<std::string, double>& constants)
        : constants_(constants) {}
    bool constant(const std::string& name, double& value) const override;
    int variable(const std::string&) const override { return -1; }
    int nodeVoltage(const std::string& node) const override;
    int branchCurrent(const std::string& component) const override;

private:
    const std::map<std::string, double>& constants_;
};

// A compiled expression e(x, t).
class Program {
public:
    Program() = default;  // the constant 0
    static Program compile(const AstPtr& ast, const Symbols& symbols);

    // With frozen = true the outcome of every comparison, abs, sign, min and
    // max is taken from the stored modes instead of the current values, which
    // makes e smooth in x and t (see updateModes).
    double eval(const Vector& x, double t, bool frozen = false) const;
    // Value and gradient; grad[k] is the derivative with respect to x[vars()[k]].
    double evalGrad(const Vector& x, double t, std::vector<double>& grad,
                    bool frozen = false) const;

    // Event functions: quantities whose sign change makes e non-smooth, i.e.
    // a - b of every comparison, min and max, and the argument of abs and sign.
    // An integrator keeps the modes frozen while it steps, watches the event
    // functions and calls updateModes() at their sign changes; the step that
    // crosses an event is then still computed with the old, smooth branch.
    int eventCount() const { return static_cast<int>(events_.size()); }
    void events(const Vector& x, double t, double* out, bool frozen = true) const;
    void updateModes(const Vector& x, double t);  // modes := current outcomes

    const std::vector<int>& vars() const { return vars_; }  // unknowns e depends on
    bool dependsOnTime() const { return usesTime_; }
    bool isConstant() const { return vars_.empty() && !usesTime_; }
    bool hasDelay() const { return !delays_.empty(); }

    enum class Op : unsigned char;
    struct Instr {
        Op op;
        int a = -1, b = -1, c = -1;  // operand slots (Var: position in vars_)
        double k = 0.0;              // Const: value
        int e = -1;                  // index of the event function deciding this operation
        int table = -1;              // index of the table that is looked up, or of the delay
    };

private:
    void run(const Vector& x, double t, bool frozen) const;

    std::vector<Instr> code_;  // instruction i writes slot i; the last slot is the result
    std::vector<int> vars_;
    std::vector<int> events_;  // slots holding the event functions
    std::vector<unsigned char> modes_;  // per event function: was it positive?
    std::vector<std::shared_ptr<const Table>> tables_;
    std::vector<std::shared_ptr<const Table2>> tables2_;
    std::vector<std::shared_ptr<DelayLine>> delays_;
    bool usesTime_ = false;
    mutable std::vector<double> val_, adj_;  // scratch (not thread safe)
};

// Evaluate text as a constant expression. Throws if it depends on unknowns or time.
double evalConstant(const std::string& text, const std::map<std::string, double>& constants);

// The past of the argument of one delay(e, td).
struct DelayLine {
    Program inner;  // e
    double td = 0.0;
    History history;

    bool active() const { return !history.empty(); }
    double value(double t) const { return history.delayed(t); }
    // Start point (start = true: forget the past) or accepted step of an integration.
    void record(double t, const Vector& x, bool start) {
        if (start) {
            history.clear();
            history.setDelay(td);
        }
        history.add(t, inner.eval(x, t));
        if (start) history.mark(t);
    }
};

// An equation brought into the form  d/dt q(x) + f(x, t) = 0.
struct DaeEquation {
    Program f;
    Program q;
    bool differential = false;  // false: q is identically 0
};

// der() may wrap any expression of the unknowns, but has to enter the equation
// linearly with constant coefficients, e.g. "m*der(v) = F - k*x" or
// "der(C*u) + der(w) = -u/R". Anything else is rejected with an explanation.
DaeEquation compileEquation(const std::string& text, const Symbols& symbols);
// The same for an already parsed equation "lhs - rhs"; errors do not quote a text.
DaeEquation compileEquation(const AstPtr& residual, const Symbols& symbols);

// For an equation of the form  c * der(x) + rest = 0  with a constant factor c
// and one unknown x under der(): the name of x, the factor c and the expression
// for the derivative, -rest / c. Returns false for every other equation.
bool explicitDerivative(const AstPtr& residual, const Symbols& symbols, std::string& name,
                        double& factor, AstPtr& rhs);

// Total time derivative of e along a solution, as an expression. Every
// unknown x in e contributes de/dx * derivativeOf[x]; parameters are constant,
// the time counts as itself. Throws if e contains an unknown without entry, a
// voltage or current of the circuit, a table, or a delay.
AstPtr timeDerivative(const AstPtr& e, const Symbols& symbols,
                      const std::map<std::string, AstPtr>& derivativeOf);

// Partial derivative of e with respect to the unknown `variable`.
AstPtr partialDerivative(const AstPtr& e, const Symbols& symbols, const std::string& variable);

// Names of the unknowns e depends on (elements of vectors as "x[3]"; voltages
// and currents of the circuit as "v(a)", "i(R1)").
std::set<std::string> unknownsIn(const AstPtr& e, const Symbols& symbols);

// The expression as text that parseExpression() reads back.
std::string toString(const AstPtr& e);

}  // namespace esim::expr
