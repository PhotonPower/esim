#include "circuit/devices/Equations.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

#include "circuit/Circuit.hpp"
#include "circuit/Symbols.hpp"

namespace esim {

EquationBlock::EquationBlock(std::string name, std::vector<VariableSpec> variables,
                             std::vector<EquationSpec> equations, std::vector<WhenSpec> whens)
    : Device(std::move(name)),
      vars_(std::move(variables)),
      text_(std::move(equations)),
      whenText_(std::move(whens)) {
    if (vars_.size() != text_.size())
        throw std::runtime_error("equations: " + std::to_string(text_.size()) +
                                 " equation(s) for " + std::to_string(vars_.size()) +
                                 " variable(s); the numbers must match");
}

void EquationBlock::setup(Circuit& ckt) {
    for (const VariableSpec& v : vars_) {
        index_.push_back(ckt.addVariable(v.name, v.abstol));
        if (v.discrete) ckt.setDiscrete(index_.back());
    }
}

void EquationBlock::bind(Circuit& ckt) {
    const CircuitSymbols symbols(ckt);
    for (const EquationSpec& spec : text_) {
        try {
            rows_.push_back({expr::compileEquation(spec.residual, symbols), -1, false});
        } catch (const std::runtime_error& e) {
            throw std::runtime_error("equation \"" + spec.text + "\": " + e.what());
        }
    }
    assignHeldStates();
    for (Row& r : rows_)
        if (r.held >= 0 && vars_[r.held].discrete) r.heldInDc = true;

    // when-clauses
    auto isState = [&](int unknown) {
        for (const Row& r : rows_) {
            const auto& qv = r.eq.q.vars();
            if (r.eq.differential && std::find(qv.begin(), qv.end(), unknown) != qv.end())
                return true;
        }
        return false;
    };
    auto compile = [&](const expr::AstPtr& ast, const std::string& text) {
        try {
            return expr::Program::compile(ast, symbols);
        } catch (const std::runtime_error& e) {
            throw std::runtime_error("events: \"" + text + "\": " + e.what());
        }
    };
    // Target of an assignment: a variable of the equations, or a voltage or
    // current of the circuit.
    auto target = [&](const std::string& name) {
        const bool probe = name.size() > 3 && name[1] == '(' && name.back() == ')' &&
                           (name[0] == 'v' || name[0] == 'i');
        if (probe) {
            try {
                const std::string inner = name.substr(2, name.size() - 3);
                const int unknown =
                    name[0] == 'v' ? symbols.nodeVoltage(inner) : symbols.branchCurrent(inner);
                if (unknown < 0) throw std::runtime_error("ground cannot be assigned");
                return unknown;
            } catch (const std::runtime_error& e) {
                throw std::runtime_error("events: \"set\": " + name + ": " + e.what());
            }
        }
        const int unknown = ckt.findVariable(name);
        if (unknown < 0 || std::find(index_.begin(), index_.end(), unknown) == index_.end())
            throw std::runtime_error("events: \"set\" refers to '" + name +
                                     "', which is not a variable");
        if (!isState(unknown))
            throw std::runtime_error(
                "events: '" + name + "' cannot be set because it is an algebraic variable "
                "(it follows from the equations). Only states, i.e. variables inside der(), "
                "and discrete variables can be assigned");
        return unknown;
    };
    for (const WhenSpec& spec : whenText_) {
        When w;
        w.timed = !spec.condition;
        if (!w.timed) w.condition = compile(spec.condition, spec.text);
        w.period = spec.period;
        w.start = spec.start;
        w.times = spec.times;
        for (const WhenSpec::Assignment& a : spec.assignments)
            w.assignments.emplace_back(target(a.variable), compile(a.value, a.text));
        whens_.push_back(std::move(w));
    }

    for (const Row& r : rows_) eventCount_ += r.eq.f.eventCount() + r.eq.q.eventCount();
    for (const When& w : whens_)
        if (!w.timed) eventCount_ += w.condition.eventCount();
}

double EquationBlock::When::timeAfter(double t) const {
    if (period > 0.0) {
        if (t < start) return start;
        // Guard against t sitting a rounding error below a multiple of the period.
        double k = std::floor((t - start) / period) + 1.0;
        double next = start + k * period;
        if (next <= t) next = start + (k + 1.0) * period;
        return next;
    }
    auto it = std::upper_bound(times.begin(), times.end(), t);
    return it == times.end() ? std::numeric_limits<double>::infinity() : *it;
}

double EquationBlock::nextBreakpoint(double t) const {
    double best = std::numeric_limits<double>::infinity();
    for (const When& w : whens_)
        if (w.timed) best = std::min(best, w.timeAfter(t));
    return best;
}

void EquationBlock::eventFunctions(double t, const Vector& x, double* g) const {
    for (const Row& r : rows_) {
        r.eq.f.events(x, t, g);
        g += r.eq.f.eventCount();
        r.eq.q.events(x, t, g);
        g += r.eq.q.eventCount();
    }
    for (const When& w : whens_) {
        if (w.timed) continue;
        w.condition.events(x, t, g);
        g += w.condition.eventCount();
    }
}

void EquationBlock::updateModes(const Vector& x, double t) {
    for (Row& r : rows_) {
        r.eq.f.updateModes(x, t);
        r.eq.q.updateModes(x, t);
    }
    for (When& w : whens_)
        if (!w.timed) w.condition.updateModes(x, t);
}

bool EquationBlock::eventUpdate(double t, Vector& x, bool initial) {
    if (initial) {
        for (When& w : whens_) {
            if (w.timed) w.next = w.timeAfter(t);  // nothing fires at the start itself
            else w.active = w.condition.eval(x, t) != 0.0;
        }
        updateModes(x, t);
        return false;
    }
    // An assignment can make further conditions true: repeat until nothing fires.
    bool changed = false;
    std::vector<std::pair<int, double>> assign;
    for (int round = 0; round < 100; ++round) {
        assign.clear();
        for (When& w : whens_) {
            bool fire;
            if (w.timed) {
                // The integrator ends a step exactly on w.next (a breakpoint).
                fire = t >= w.next - 1e-9 * std::max(w.period, 1e-9 * std::abs(t));
                if (fire) w.next = w.timeAfter(std::max(t, w.next));
            } else {
                const bool now = w.condition.eval(x, t) != 0.0;
                fire = now && !w.active;
                w.active = now;
            }
            if (fire)
                for (const auto& [unknown, value] : w.assignments)
                    assign.emplace_back(unknown, value.eval(x, t));
        }
        if (assign.empty()) {
            updateModes(x, t);
            return changed;
        }
        for (const auto& [unknown, value] : assign) x[unknown] = value;
        changed = true;
    }
    throw std::runtime_error("events: the when-clauses keep triggering each other at t = " +
                             std::to_string(t));
}

// Give every state with a prescribed initial value a differential equation
// that mentions it inside der(); at the initial point that equation is
// replaced by "state = initial value" (bipartite matching, Kuhn's algorithm).
void EquationBlock::assignHeldStates() {
    const int n = static_cast<int>(vars_.size());
    auto inDer = [&](int row, int var) {
        const auto& qv = rows_[row].eq.q.vars();
        return rows_[row].eq.differential &&
               std::find(qv.begin(), qv.end(), index_[var]) != qv.end();
    };

    std::vector<char> seen;
    std::function<bool(int)> augment = [&](int var) {
        for (int row = 0; row < n; ++row) {
            if (seen[row] || !inDer(row, var)) continue;
            seen[row] = 1;
            if (rows_[row].held < 0 || augment(rows_[row].held)) {
                rows_[row].held = var;
                return true;
            }
        }
        return false;
    };

    for (int var = 0; var < n; ++var) {
        if (!vars_[var].hold) continue;
        bool isState = false;
        for (int row = 0; row < n; ++row) isState = isState || inDer(row, var);
        if (!isState) continue;  // algebraic variable: the value is a start value only
        seen.assign(n, 0);
        if (!augment(var))
            throw std::runtime_error(
                "equations: cannot prescribe the initial value of '" + vars_[var].name +
                "': there are fewer differential equations than initial values for the states "
                "they contain. Use {\"guess\": ...} for the dependent ones");
    }
}

void EquationBlock::applyGuess(Vector& x) const {
    for (size_t k = 0; k < vars_.size(); ++k) x[index_[k]] = vars_[k].start;
}

void EquationBlock::stamp(StampContext& ctx) {
    const bool frozen = ctx.frozenModes();
    for (size_t k = 0; k < rows_.size(); ++k) {
        const Row& r = rows_[k];
        const int row = index_[k];

        // Held states: at the initial point of a transient analysis; discrete
        // variables also in the DC analyses, where der(x) = 0 says nothing.
        if (r.held >= 0 && (ctx.initPoint || (!ctx.transient && r.heldInDc))) {
            const int j = index_[r.held];
            ctx.addF(row, ctx.x[j] - vars_[r.held].start);
            ctx.addG(row, j, 1.0);
            continue;
        }

        ctx.addF(row, r.eq.f.evalGrad(ctx.x, ctx.t, grad_, frozen));
        const auto& fv = r.eq.f.vars();
        for (size_t i = 0; i < fv.size(); ++i) ctx.addG(row, fv[i], grad_[i]);

        if (r.eq.differential) {
            ctx.addQ(row, r.eq.q.evalGrad(ctx.x, ctx.t, grad_, frozen));
            const auto& qv = r.eq.q.vars();
            for (size_t i = 0; i < qv.size(); ++i) ctx.addC(row, qv[i], grad_[i]);
        }
    }
}

}  // namespace esim
