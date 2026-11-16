#pragma once

#include <optional>
#include <string>

#include "circuit/Device.hpp"

namespace esim {

class Resistor final : public Device {
public:
    // noisy = false: no thermal noise (as the noiseless resistors of op amp macromodels)
    Resistor(std::string name, int a, int b, double r, bool noisy = true);
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void noiseSources(std::vector<NoiseSource>& out) const override;

private:
    int a_, b_;
    double g_;
    double kT_ = 0.0;
    bool noisy_ = true;
};

class Capacitor final : public Device {
public:
    Capacitor(std::string name, int a, int b, double c, std::optional<double> ic);
    void stamp(StampContext& ctx) override;
    void applyIc(Vector& x0) const override;

private:
    int a_, b_;
    double c_;
    std::optional<double> ic_;  // v(a) - v(b) at t = 0
};

// Inductor with its current (a -> b) as an additional unknown.
class Inductor final : public Device {
public:
    Inductor(std::string name, int a, int b, double l, std::optional<double> ic);
    void setup(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    void applyIc(Vector& x0) const override;
    int branch() const override { return br_; }
    double inductance() const { return l_; }

private:
    int a_, b_, br_ = -1;
    double l_;
    std::optional<double> ic_;  // current at t = 0
};

// Magnetic coupling of two inductors with the mutual inductance
// M = k sqrt(L1 L2). The first node of each inductor carries the dot.
class MutualInductance final : public Device {
public:
    MutualInductance(std::string name, std::string l1, std::string l2, double k);
    void bind(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;

private:
    std::string name1_, name2_;
    double k_, m_ = 0.0;
    int br1_ = -1, br2_ = -1;
};

}  // namespace esim
