#pragma once

#include "core/DaeSystem.hpp"

// Robertson's chemical kinetics problem, written as a semi-explicit DAE of
// index 1: two stiff rate equations plus the conservation law as an algebraic
// constraint.
//
//     y1' = -0.04 y1 + 1e4 y2 y3
//     y2' =  0.04 y1 - 1e4 y2 y3 - 3e7 y2^2
//      0  =  y1 + y2 + y3 - 1
//
// In the form d/dt q(x) + f(x, t) = 0 this is q = (y1, y2, 0) and
// f = -(right-hand sides). It shows how a system that is not a circuit plugs
// into the solvers: implement size() and evaluate(), nothing else is required.
class Robertson final : public esim::DaeSystem {
public:
    int size() const override { return 3; }

    void evaluate(double /*t*/, const esim::Vector& y, esim::LimitMode /*limiting*/,
                  esim::Evaluation& out) override {
        out.reset(3);

        out.q[0] = y[0];
        out.q[1] = y[1];
        out.C.emplace_back(0, 0, 1.0);
        out.C.emplace_back(1, 1, 1.0);

        const double r1 = 0.04 * y[0];
        const double r2 = 1e4 * y[1] * y[2];
        const double r3 = 3e7 * y[1] * y[1];
        out.f[0] = r1 - r2;
        out.f[1] = -r1 + r2 + r3;
        out.f[2] = y[0] + y[1] + y[2] - 1.0;

        out.G.emplace_back(0, 0, 0.04);
        out.G.emplace_back(0, 1, -1e4 * y[2]);
        out.G.emplace_back(0, 2, -1e4 * y[1]);
        out.G.emplace_back(1, 0, -0.04);
        out.G.emplace_back(1, 1, 1e4 * y[2] + 6e7 * y[1]);
        out.G.emplace_back(1, 2, 1e4 * y[1]);
        out.G.emplace_back(2, 0, 1.0);
        out.G.emplace_back(2, 1, 1.0);
        out.G.emplace_back(2, 2, 1.0);
    }

    std::string unknownName(int i) const override { return "y" + std::to_string(i + 1); }

    // y2 is of the order 1e-5, so the default tolerance would be too coarse.
    double absTol(int /*i*/) const override { return 1e-10; }
};
