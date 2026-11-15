#include "circuit/IndexReduction.hpp"

#include <map>
#include <stdexcept>

#include "circuit/Symbols.hpp"
#include "core/StructuralIndex.hpp"

namespace esim {

namespace {

struct Ode {  // c * der(x) + rest = 0
    size_t equation;
    double factor;
    expr::AstPtr rhs;  // der(x) = rhs
};

expr::AstPtr binary(const char* op, expr::AstPtr a, expr::AstPtr b) {
    auto n = std::make_shared<expr::Ast>();
    n->kind = expr::Ast::Kind::Binary;
    n->name = op;
    n->args = {std::move(a), std::move(b)};
    return n;
}

expr::AstPtr symbol(const std::string& name) {
    auto n = std::make_shared<expr::Ast>();
    n->kind = expr::Ast::Kind::Symbol;
    n->name = name;
    return n;
}

bool isZero(const expr::AstPtr& e) { return e->kind == expr::Ast::Kind::Number && e->number == 0.0; }

}  // namespace

bool reduceIndex(Circuit& ckt, const EquationSystem& in, EquationSystem& out,
                 std::vector<std::string>& notes) {
    ckt.setTransientMode(true);
    const IndexAnalysis a = structuralIndex(ckt, Vector::Zero(ckt.size()));
    ckt.setTransientMode(false);
    if (!a.analyzed || a.singular || a.index <= 2) return false;

    const CircuitSymbols symbols(ckt);
    const std::string head = "index " + std::to_string(a.index);
    try {
        // The equations that give the derivative of an unknown explicitly.
        std::map<std::string, Ode> odes;
        std::map<std::string, expr::AstPtr> derivativeOf;
        for (size_t j = 0; j < in.equations.size(); ++j) {
            std::string name;
            Ode ode{j, 1.0, nullptr};
            if (expr::explicitDerivative(in.equations[j].residual, symbols, name, ode.factor, ode.rhs)) {
                odes[name] = ode;
                derivativeOf[name] = ode.rhs;
            }
        }

        out = in;
        std::string report;
        int multipliers = 0;
        for (size_t k = 0; k < in.equations.size(); ++k) {
            const int row = ckt.findVariable(in.variables[k].name);
            const int times = a.differentiations[row];
            if (times < 2) continue;
            const std::string quoted = "\"" + in.equations[k].text + "\"";
            // Only constraints are differentiated. The differential equations the
            // analysis lists as well are used up when their right-hand sides are
            // inserted into the derivatives of the constraints.
            if (expr::compileEquation(in.equations[k].residual, symbols).differential) continue;

            expr::AstPtr g = in.equations[k].residual;
            report += "\n  " + quoted + ":";
            for (int level = 1; level < times; ++level) {
                // Multiplier that keeps g in force next to its derivative.
                const std::string mu = "index_mu" + std::to_string(++multipliers);
                std::string where;
                for (const std::string& x : expr::unknownsIn(g, symbols)) {
                    const expr::AstPtr slope = expr::partialDerivative(g, symbols, x);
                    if (isZero(slope)) continue;
                    auto ode = odes.find(x);
                    if (ode == odes.end())
                        throw std::runtime_error(quoted + " cannot be differentiated: the derivative of '" + x +
                                                 "' is not given by an equation of the form der(" + x + ") = ...");
                    // c der(x) + rest = 0  becomes  c der(x) + rest + c dg/dx mu = 0
                    EquationSpec& target = out.equations[ode->second.equation];
                    auto factor = std::make_shared<expr::Ast>();
                    factor->number = ode->second.factor;
                    target.residual = binary("+", target.residual, binary("*", binary("*", factor, slope), symbol(mu)));
                    if (target.text.find("  [stabilized") == std::string::npos) target.text += "  [stabilized]";
                    where += (where.empty() ? "" : ", ") + x;
                }
                g = expr::timeDerivative(g, symbols, derivativeOf);
                // The sign of an equation "0 = g" does not matter: drop a leading minus.
                if (g->kind == expr::Ast::Kind::Unary && g->name == "-") g = g->args[0];
                VariableSpec multiplier;
                multiplier.name = mu;
                out.variables.push_back(multiplier);
                out.equations.push_back({"0 = " + expr::toString(g), g});
                report += "\n    added  0 = " + expr::toString(g) + "   (multiplier " + mu + " in the equations of " +
                          where + ")";
            }
        }
        if (multipliers == 0)
            throw std::runtime_error("no algebraic equation of the user equations is the cause "
                                     "(the constraint lies in the circuit)");
        notes.push_back(head + " reduced to 2 by adding time derivatives of constraints:" + report);
        return true;
    } catch (const std::runtime_error& e) {
        notes.push_back(head + ", automatic reduction not possible: " + e.what());
        return false;
    }
}

}  // namespace esim
