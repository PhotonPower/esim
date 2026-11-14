#pragma once

#include <string>

#include "circuit/Device.hpp"
#include "expr/Expression.hpp"

namespace esim {

// Elements with two linear branches that change over at events. While the
// integrator steps their branch is frozen (StampContext::frozenModes); in the
// DC analyses and in the re-initialization step after an event the branch
// follows the Newton iterate.

struct SwitchParams {
    double ron = 1e-3;         // resistance when closed [Ohm]
    double roff = 1e9;         // resistance when open [Ohm]
    double threshold = 0.0;    // closes above threshold + hysteresis,
    double hysteresis = 0.0;   // opens below threshold - hysteresis
    bool initiallyOn = false;  // position inside the hysteresis band at the start
};

// Switch between p and n, controlled by an expression (a node voltage, a
// branch current, an equation variable, time, ...).
class Switch final : public Device {
public:
    Switch(std::string name, int p, int n, expr::AstPtr control, const SwitchParams& params);
    void bind(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void reset() override { on_ = p_.initiallyOn; }
    int eventCount() const override { return 1; }
    void eventFunctions(double t, const Vector& x, double* g) const override;
    bool eventUpdate(double t, Vector& x, bool initial) override;

private:
    bool update(double control);  // apply the switching rule; true if the position changed

    int a_, b_;
    expr::AstPtr ast_;
    expr::Program control_;
    SwitchParams p_;
    bool on_;
};

struct IdealDiodeParams {
    double vf = 0.0;     // forward voltage [V]
    double ron = 1e-3;   // slope resistance when conducting [Ohm]
    double roff = 1e9;   // resistance when blocking [Ohm]
};

// Piecewise linear diode: blocks below vf, conducts above with slope 1/ron.
// The characteristic is continuous, i = v/roff + max(0, v - vf) (1/ron - 1/roff).
class IdealDiode final : public Device {
public:
    IdealDiode(std::string name, int anode, int cathode, const IdealDiodeParams& params);
    void stamp(StampContext& ctx) override;
    void reset() override { on_ = false; }
    int eventCount() const override { return 1; }
    void eventFunctions(double t, const Vector& x, double* g) const override;
    bool eventUpdate(double t, Vector& x, bool initial) override;

private:
    double voltage(const Vector& x) const {
        return (a_ >= 0 ? x[a_] : 0.0) - (c_ >= 0 ? x[c_] : 0.0);
    }

    int a_, c_;
    IdealDiodeParams p_;
    bool on_ = false;
};

}  // namespace esim
