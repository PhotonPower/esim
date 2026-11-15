#pragma once

#include <stdexcept>

#include "circuit/Circuit.hpp"
#include "circuit/Device.hpp"
#include "expr/Expression.hpp"

namespace esim {

// Resolves the names in an expression against a finalized (or currently
// binding) circuit: parameters, equation variables, node voltages and branch
// currents.
class CircuitSymbols final : public expr::Symbols {
public:
    explicit CircuitSymbols(const Circuit& ckt) : ckt_(ckt) {}

    bool constant(const std::string& name, double& value) const override {
        auto it = ckt_.parameters.find(name);
        if (it == ckt_.parameters.end()) return false;
        value = it->second;
        return true;
    }

    int variable(const std::string& name) const override { return ckt_.findVariable(name); }

    const expr::FunctionDef* function(const std::string& name) const override {
        auto it = ckt_.functions.find(name);
        return it == ckt_.functions.end() ? nullptr : &it->second;
    }

    bool addDelay(const std::shared_ptr<expr::DelayLine>& line) const override {
        ckt_.addDelay(line);
        return true;
    }

    std::shared_ptr<const expr::Table> table(const std::string& name) const override {
        auto it = ckt_.tables.find(name);
        return it == ckt_.tables.end() ? nullptr : it->second;
    }

    int nodeVoltage(const std::string& node) const override { return ckt_.findNode(node); }

    int branchCurrent(const std::string& component) const override {
        const Device* d = ckt_.findDevice(component);
        if (!d) throw std::runtime_error("unknown component '" + component + "'");
        if (d->branch() < 0)
            throw std::runtime_error("component '" + component +
                                     "' has no branch current (only voltage sources, "
                                     "inductors, E, H and B voltage sources have one)");
        return d->branch();
    }

private:
    const Circuit& ckt_;
};

}  // namespace esim
