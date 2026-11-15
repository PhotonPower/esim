#pragma once

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/DaeSystem.hpp"
#include "expr/Expression.hpp"

namespace esim {

class Device;

enum class UnknownKind { Voltage, Current, Variable };

struct CircuitOptions {
    double gmin = 1e-12;    // S, in parallel to pn junctions
    double vntol = 1e-6;    // V, absolute tolerance of voltages
    double abstol = 1e-12;  // A, absolute tolerance of currents
    double vartol = 1e-9;   // absolute tolerance of equation variables
    double temp = 27.0;     // degC, temperature of the circuit
    double tnom = 27.0;     // degC, temperature the model parameters are given for

    double vt() const { return 8.617333262e-5 * (temp + 273.15); }
    double tempRatio() const { return (temp + 273.15) / (tnom + 273.15); }
    // Saturation current of a pn junction at the circuit temperature (SPICE):
    // is * ratio^(xti/n) * exp((ratio - 1) * eg / (n * vt)).
    double saturationCurrent(double is, double n, double eg, double xti) const {
        const double r = tempRatio();
        return is * std::pow(r, xti / n) * std::exp((r - 1.0) * eg / (n * vt()));
    }
};

// A circuit in modified nodal analysis, optionally extended by user-defined
// equations. The unknowns are the node voltages (ground excluded), the branch
// currents of voltage-defined elements and the variables of the equations.
class Circuit final : public DaeSystem {
public:
    static constexpr int Ground = -1;

    Circuit();
    ~Circuit() override;
    Circuit(const Circuit&) = delete;
    Circuit& operator=(const Circuit&) = delete;

    CircuitOptions options;
    std::map<std::string, double> nodeset;  // start values for the DC solution
    std::map<std::string, double> ic;       // node voltages at t = 0 (transient with uic)
    std::map<std::string, double> parameters;  // named constants for expressions
    std::map<std::string, std::shared_ptr<const expr::Table>> tables;  // callable in expressions
    std::map<std::string, expr::FunctionDef> functions;                // user-defined functions

    // --- construction -----------------------------------------------------
    static bool isGroundName(const std::string& name);
    int node(const std::string& name);            // get or create
    int findNode(const std::string& name) const;  // throws if unknown
    int addUnknown(const std::string& label, UnknownKind kind, bool internal);
    // Variable of a user-defined equation; abstol < 0 selects options.vartol.
    int addVariable(const std::string& name, double abstol = -1.0);
    int findVariable(const std::string& name) const;  // -1 if there is none
    Device& add(std::unique_ptr<Device> dev);
    Device* findDevice(const std::string& name) const;
    const std::vector<std::unique_ptr<Device>>& devices() const { return devices_; }
    void finalize();

    // --- analysis support -------------------------------------------------
    void setTransientMode(bool on) { transient_ = on; }
    // Initial point of a transient analysis: like the DC problem, but states
    // with a prescribed initial value are held at that value.
    void setInitialPointMode(bool on) { initPoint_ = on; }
    void resetDevices();  // before each analysis: switches return to their initial position
    Vector initialGuess() const;                // start values of devices plus nodeset
    void initialState(Vector& x0, Vector& q0);  // prescribed initial conditions (uic)

    UnknownKind kind(int i) const { return unknowns_[i].kind; }
    bool isInternal(int i) const { return unknowns_[i].internal; }
    void setInternal(int i, bool internal) { unknowns_[i].internal = internal; }
    // Discrete unknowns only change at events (switch positions, sampled values).
    bool isDiscrete(int i) const { return unknowns_[i].discrete; }
    void setDiscrete(int i) { unknowns_[i].discrete = true; }

    // --- DaeSystem --------------------------------------------------------
    int size() const override { return static_cast<int>(unknowns_.size()); }
    void evaluate(double t, const Vector& x, LimitMode limiting, Evaluation& out) override;
    std::string unknownName(int i) const override { return unknowns_[i].label; }
    double absTol(int i) const override;
    double nextBreakpoint(double t) const override;
    double maxStep(double t) const override;
    int eventCount() const override { return eventCount_; }
    void eventFunctions(double t, const Vector& x, Vector& g) override;
    bool eventUpdate(double t, Vector& x, bool initial) override;
    void setLiveModes(bool on) override { liveModes_ = on; }
    void acceptStep(double t, const Vector& x, bool start) override;
    void noteDiscontinuity(double t) override;
    // History of a delay() in an expression compiled against this circuit.
    void addDelay(const std::shared_ptr<expr::DelayLine>& line) const { delays_.push_back(line); }
    bool hasHistory() const;  // delays or transmission lines: the future depends on more than the state
    bool setSourceScale(double scale) override {
        srcScale_ = scale;
        return true;
    }
    bool setShunt(double g) override {
        gshunt_ = g;
        return true;
    }
    bool setDamping(double g, const Vector* anchor) override {
        gdamp_ = g;
        if (g > 0.0 && anchor) anchor_ = *anchor;
        return true;
    }

private:
    struct Unknown {
        std::string label;
        UnknownKind kind;
        bool internal;
        double abstol = -1.0;  // < 0: default of the kind
        bool discrete = false;
    };

    std::vector<Unknown> unknowns_;
    std::unordered_map<std::string, int> nodes_;
    std::unordered_map<std::string, int> variables_;
    std::vector<std::unique_ptr<Device>> devices_;
    // Delays of all expressions (also of outputs, which come and go).
    mutable std::vector<std::weak_ptr<expr::DelayLine>> delays_;
    std::unordered_map<std::string, Device*> deviceByName_;
    bool finalized_ = false;
    int eventCount_ = 0;
    bool groundUsed_ = false;
    bool transient_ = false;
    bool useIc_ = false;
    bool initPoint_ = false;
    bool liveModes_ = false;
    double srcScale_ = 1.0;
    double gshunt_ = 0.0;
    double gdamp_ = 0.0;  // damping of the node voltages towards anchor_
    Vector anchor_;
};

}  // namespace esim
