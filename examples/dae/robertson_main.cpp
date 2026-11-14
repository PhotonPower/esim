// Solves a DAE that is not a circuit with the generic solvers from src/core.
//
//   ./build/dae_robertson

#include <cstdio>

#include "Robertson.hpp"
#include "core/Transient.hpp"

int main() {
    Robertson sys;

    esim::Vector y0(3);
    y0 << 1.0, 0.0, 0.0;  // consistent with the constraint y1 + y2 + y3 = 1

    esim::TransientOptions opt;
    opt.tstop = 4e5;
    opt.hinit = 1e-6;
    opt.hmax = 4e4;
    opt.method = esim::IntegrationMethod::Bdf2;
    opt.reltol = 1e-5;
    opt.peakFloor = 0.0;  // decaying solution: error relative to the current value
    opt.newton.reltol = 1e-5;

    std::printf("%14s %14s %14s %14s %12s\n", "t", "y1", "y2", "y3", "sum - 1");
    double nextPrint = 0.0;
    const auto stats = esim::runTransient(sys, y0, nullptr, opt,
                                          [&](double t, const esim::Vector& y) {
        if (t < nextPrint && t < opt.tstop) return;
        std::printf("%14.6g %14.8f %14.6e %14.8f %12.1e\n", t, y[0], y[1], y[2], y.sum() - 1.0);
        nextPrint = t == 0.0 ? 0.4 : 10.0 * nextPrint;
    });

    std::printf("\n%d steps accepted, %d rejected, %d Newton iterations\n", stats.accepted,
                stats.rejected, stats.newtonIterations);
    return 0;
}
