#pragma once

#include <string>

#include "circuit/Device.hpp"
#include "expr/Expression.hpp"

namespace esim {

// Two-terminal element defined by an expression of node voltages, branch
// currents, equation variables and time:
//
//   Current : the expression is the current from p through the element to n
//             (a nonlinear resistor is "i": "f(v(p,n))")
//   Voltage : the expression is v(p) - v(n); adds a branch current unknown
//   Charge  : the expression is the charge stored between p and n; the current
//             is its time derivative (nonlinear capacitor)
//   Flux    : the expression is the magnetic flux linkage; the voltage is its
//             time derivative (nonlinear inductor). It normally depends on the
//             element's own current i(name), which is an additional unknown
class BehavioralSource final : public Device {
public:
    enum class Kind { Current, Voltage, Charge, Flux };

    BehavioralSource(std::string name, int p, int n, Kind kind, std::string text,
                     expr::AstPtr expression);
    void setup(Circuit& ckt) override;
    void bind(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    int branch() const override { return br_; }
    int eventCount() const override { return prog_.eventCount(); }
    void eventFunctions(double t, const Vector& x, double* g) const override {
        prog_.events(x, t, g);
    }
    bool eventUpdate(double t, Vector& x, bool /*initial*/) override {
        prog_.updateModes(x, t);
        return false;
    }

private:
    int p_, n_, br_ = -1;
    Kind kind_;
    std::string text_;
    expr::AstPtr ast_;
    expr::Program prog_;
    std::vector<double> grad_;
};

}  // namespace esim
