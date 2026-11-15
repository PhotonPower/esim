// Self-contained test suite: every test compares against an analytic result.
// Returns a non-zero exit code if any check fails.

#include <Eigen/Dense>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "app/Measure.hpp"
#include "app/Runner.hpp"
#include "circuit/devices/Bsim3.hpp"
#include "circuit/Analysis.hpp"
#include "circuit/Device.hpp"
#include "circuit/devices/Sources.hpp"
#include "core/StructuralIndex.hpp"
#include "core/Transient.hpp"
#include "dae/Robertson.hpp"
#include "expr/Expression.hpp"
#include "io/Netlist.hpp"
#include "io/Output.hpp"
#include "io/Units.hpp"

using namespace esim;

namespace {

int g_checks = 0, g_failures = 0;

void checkNear(double actual, double expected, double tol, const std::string& what) {
    ++g_checks;
    if (!(std::abs(actual - expected) <= tol)) {
        ++g_failures;
        std::cout << "  FAIL " << what << ": got " << actual << ", expected " << expected
                  << " +- " << tol << '\n';
    }
}

void checkTrue(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cout << "  FAIL " << what << '\n';
    }
}

double probe(const Circuit& ckt, const Vector& x, const std::string& expr) {
    return parseProbe(ckt, expr).value(x);
}

struct Trace {
    std::vector<double> t;
    std::vector<Vector> x;
};

Trace transient(Netlist& nl, double tstop, IntegrationMethod method, bool uic,
                TransientStats* stats = nullptr, double outputStep = 0.0) {
    Circuit& ckt = *nl.circuit;
    TransientOptions to;
    to.tstop = tstop;
    to.method = method;
    to.reltol = nl.options.reltol;
    to.peakFloor = nl.options.lteFloor;
    to.outputStep = outputStep;
    to.newton.reltol = nl.options.reltol;
    ckt.setTransientMode(true);
    Vector x0, q0;
    if (uic) {
        ckt.initialState(x0, q0);
    } else {
        ckt.setInitialPointMode(true);
        x0 = solveOperatingPoint(ckt, nl.options);
        ckt.setInitialPointMode(false);
    }
    Trace tr;
    const TransientStats s =
        runTransient(ckt, x0, uic ? &q0 : nullptr, to, [&](double t, const Vector& x) {
            tr.t.push_back(t);
            tr.x.push_back(x);
        });
    if (stats) *stats = s;
    ckt.setTransientMode(false);
    return tr;
}

// --- tests -------------------------------------------------------------------

void testUnits() {
    checkNear(parseValue("1k"), 1e3, 0, "1k");
    checkNear(parseValue("4.7u"), 4.7e-6, 1e-18, "4.7u");
    checkNear(parseValue("100nF"), 100e-9, 1e-18, "100nF");
    checkNear(parseValue("1meg"), 1e6, 0, "1meg");
    checkNear(parseValue("2.2M"), 2.2e6, 1e-6, "2.2M");
    checkNear(parseValue("10m"), 10e-3, 1e-15, "10m");
    checkNear(parseValue("1e-3"), 1e-3, 0, "1e-3");
    checkNear(parseValue("5V"), 5.0, 0, "5V");
    checkNear(parseValue("3 kOhm"), 3e3, 0, "3 kOhm");
    bool threw = false;
    try {
        parseValue("abc");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    checkTrue(threw, "parseValue rejects non-numbers");
}

void testVoltageDivider() {
    Netlist nl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 10},
            {"type": "R", "name": "R1", "nodes": ["in", "mid"], "value": "1k"},
            {"type": "R", "name": "R2", "nodes": ["mid", "0"], "value": "3k"}
        ]})json");
    const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
    checkNear(probe(*nl.circuit, x, "v(mid)"), 7.5, 1e-9, "divider v(mid)");
    checkNear(probe(*nl.circuit, x, "v(in,mid)"), 2.5, 1e-9, "divider v(in,mid)");
    checkNear(probe(*nl.circuit, x, "i(V1)"), -2.5e-3, 1e-12, "divider i(V1)");
}

void testControlledSources() {
    // Inverting amplifier with a VCVS as op-amp (gain 1e6): vout = -R2/R1 * vin.
    // F and H are driven by the input current, G by the input voltage.
    Netlist nl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "Vin", "nodes": ["in", "0"], "dc": 0.5},
            {"type": "R", "name": "R1", "nodes": ["in", "n"], "value": "1k"},
            {"type": "R", "name": "R2", "nodes": ["n", "out"], "value": "4k"},
            {"type": "E", "name": "E1", "nodes": ["out", "0", "0", "n"], "gain": 1e6},
            {"type": "F", "name": "F1", "nodes": ["0", "m"], "control": "Vin", "gain": 2},
            {"type": "R", "name": "R3", "nodes": ["m", "0"], "value": "1k"},
            {"type": "H", "name": "H1", "nodes": ["h", "0"], "control": "Vin", "r": 1000},
            {"type": "R", "name": "R4", "nodes": ["h", "0"], "value": "1k"},
            {"type": "G", "name": "G1", "nodes": ["0", "g", "in", "0"], "gm": 1e-3},
            {"type": "R", "name": "R5", "nodes": ["g", "0"], "value": "2k"}
        ]})json");
    const Circuit& ckt = *nl.circuit;
    const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
    const double iin = probe(ckt, x, "i(Vin)");  // about -0.5 mA
    checkNear(probe(ckt, x, "v(out)"), -2.0, 1e-4, "inverting amplifier v(out)");
    checkNear(iin, -0.5e-3, 1e-8, "inverting amplifier input current");
    checkNear(probe(ckt, x, "v(m)"), 2.0 * iin * 1e3, 1e-9, "cccs output");
    checkNear(probe(ckt, x, "v(h)"), 1000.0 * iin, 1e-9, "ccvs output");
    checkNear(probe(ckt, x, "v(g)"), 1e-3 * 0.5 * 2e3, 1e-9, "vccs output");
}

void testRcTransient() {
    // Step response of an RC low-pass: v = 1 - exp(-t / RC), RC = 1 ms.
    for (IntegrationMethod m : {IntegrationMethod::BackwardEuler, IntegrationMethod::Trapezoidal,
                                IntegrationMethod::Bdf2, IntegrationMethod::Bdf}) {
        Netlist nl = parseNetlistText(R"json({
            "options": {"reltol": 1e-5},
            "components": [
                {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1},
                {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k"},
                {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "1u", "ic": 0}
            ]})json");
        TransientStats stats;
        const Trace tr = transient(nl, 5e-3, m, true, &stats);
        const Probe out = parseProbe(*nl.circuit, "v(out)");
        double maxErr = 0.0;
        for (size_t k = 0; k < tr.t.size(); ++k)
            maxErr = std::max(maxErr, std::abs(out.value(tr.x[k]) - (1.0 - std::exp(-tr.t[k] / 1e-3))));
        const double tol = m == IntegrationMethod::BackwardEuler ? 5e-3 : 5e-4;
        checkNear(maxErr, 0.0, tol, std::string("rc step response, ") + methodName(m));
        checkNear(tr.t.back(), 5e-3, 1e-15, "rc ends exactly at tstop");
        std::cout << "    rc " << methodName(m) << ": " << stats.accepted << " steps, max error "
                  << maxErr << '\n';
    }
}

void testVariableOrder() {
    // Smooth problem, tight tolerance: the variable-order method needs far
    // fewer steps than the second-order one and is more accurate.
    const char* text = R"json({
        "variables": {"x": 1, "v": 0},
        "equations": ["der(x) = v", "der(v) = -x - 0.5*v"],
        "options": {"reltol": 1e-8, "vartol": 1e-12}
    })json";
    const double wd = std::sqrt(1.0 - 0.0625);
    const auto solve = [&](IntegrationMethod m, TransientStats& st) {
        Netlist nl = parseNetlistText(text);
        const Trace tr = transient(nl, 20.0, m, false, &st);
        double e = 0.0;
        for (size_t k = 0; k < tr.t.size(); ++k) {
            const double t = tr.t[k];
            const double ref = std::exp(-0.25 * t) * (std::cos(wd * t) + 0.25 / wd * std::sin(wd * t));
            e = std::max(e, std::abs(probe(*nl.circuit, tr.x[k], "x") - ref));
        }
        return e;
    };
    TransientStats s2, sv;
    const double e2 = solve(IntegrationMethod::Bdf2, s2);
    const double ev = solve(IntegrationMethod::Bdf, sv);
    std::cout << "    damped oscillator, reltol 1e-8: gear2 " << s2.accepted << " steps, error " << e2
              << "; bdf " << sv.accepted << " steps (order up to " << sv.maxOrder << "), error " << ev
              << '\n';
    checkTrue(sv.accepted * 3 < s2.accepted, "variable order needs less than a third of the steps");
    checkTrue(ev < e2, "variable order is more accurate");
    checkNear(ev, 0.0, 1e-6, "variable order: accuracy at reltol 1e-8");
    checkTrue(sv.maxOrder == 5, "variable order reaches order 5");
}

void testPulseBreakpoints() {
    // The solver has to land exactly on the corners of the pulse, and the
    // response to the (nearly ideal) edge has to match the analytic one.
    Netlist nl = parseNetlistText(R"json({
        "options": {"reltol": 1e-5},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"],
             "wave": {"type": "pulse", "v1": 0, "v2": 1, "delay": "1m", "rise": "1u",
                      "fall": "1u", "width": "2m"}},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "0.5u"}
        ]})json");
    const Trace tr = transient(nl, 6e-3, IntegrationMethod::Trapezoidal, false);
    const Probe out = parseProbe(*nl.circuit, "v(out)");
    for (double bp : {1e-3, 1.001e-3, 3.001e-3, 3.002e-3}) {
        bool hit = false;
        for (double t : tr.t) hit = hit || std::abs(t - bp) < 1e-12;
        checkTrue(hit, "time point at pulse corner t = " + std::to_string(bp));
    }
    const double tau = 0.5e-3;
    double maxErr = 0.0;
    for (size_t k = 0; k < tr.t.size(); ++k) {
        const double t = tr.t[k];
        double ref;  // ideal edges in the middle of the 1 us ramps
        if (t < 1.0005e-3) ref = 0.0;
        else if (t < 3.0015e-3) ref = 1.0 - std::exp(-(t - 1.0005e-3) / tau);
        else ref = (1.0 - std::exp(-2.001e-3 / tau)) * std::exp(-(t - 3.0015e-3) / tau);
        if (std::abs(t - 1.0005e-3) > 2e-6 && std::abs(t - 3.0015e-3) > 2e-6)
            maxErr = std::max(maxErr, std::abs(out.value(tr.x[k]) - ref));
    }
    checkNear(maxErr, 0.0, 1e-3, "rc pulse response");
}

void testLcOscillator() {
    // Lossless LC tank started from a charged capacitor: v = cos(w t).
    Netlist nl = parseNetlistText(R"json({
        "options": {"reltol": 1e-6},
        "components": [
            {"type": "C", "name": "C1", "nodes": ["a", "0"], "value": "1u", "ic": 1},
            {"type": "L", "name": "L1", "nodes": ["a", "0"], "value": "1m", "ic": 0}
        ]})json");
    const double w = 1.0 / std::sqrt(1e-3 * 1e-6);
    const Trace tr = transient(nl, 1e-3, IntegrationMethod::Trapezoidal, true);
    const Probe va = parseProbe(*nl.circuit, "v(a)"), il = parseProbe(*nl.circuit, "i(L1)");
    double maxErr = 0.0, maxEnergyErr = 0.0;
    for (size_t k = 0; k < tr.t.size(); ++k) {
        const double v = va.value(tr.x[k]), i = il.value(tr.x[k]);
        maxErr = std::max(maxErr, std::abs(v - std::cos(w * tr.t[k])));
        maxEnergyErr = std::max(maxEnergyErr, std::abs(0.5e-6 * v * v + 0.5e-3 * i * i - 0.5e-6));
    }
    checkNear(maxErr, 0.0, 2e-2, "lc oscillation v(a)");
    // The trapezoidal rule conserves the energy; only the two backward Euler
    // start-up steps dissipate a little.
    checkNear(maxEnergyErr / 0.5e-6, 0.0, 1e-4, "lc energy conservation (trapezoidal)");
}

void testDiode() {
    Netlist nl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 5},
            {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": "1k"},
            {"type": "D", "name": "D1", "nodes": ["a", "0"], "is": 1e-14}
        ]})json");
    const Circuit& ckt = *nl.circuit;
    DcResult info;
    const Vector x = solveOperatingPoint(*nl.circuit, nl.options, nullptr, &info);
    const double vd = probe(ckt, x, "v(a)");
    const double iR = (5.0 - vd) / 1e3;
    const double iD = 1e-14 * (std::exp(vd / ckt.options.vt()) - 1.0) + ckt.options.gmin * vd;
    checkTrue(vd > 0.6 && vd < 0.8, "diode forward voltage plausible");
    checkNear(iD / iR, 1.0, 1e-3, "diode current equals resistor current");
    checkTrue(info.strategy == "newton", "diode operating point with plain Newton");

    // Zener: 5.1 V breakdown clamps the node.
    Netlist nz = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 12},
            {"type": "R", "name": "R1", "nodes": ["in", "k"], "value": "1k"},
            {"type": "D", "name": "DZ", "nodes": ["0", "k"], "bv": 5.1, "ibv": "5m"}
        ]})json");
    const Vector xz = solveOperatingPoint(*nz.circuit, nz.options);
    checkNear(probe(*nz.circuit, xz, "v(k)"), 5.1, 0.05, "zener clamp voltage");
}

void testBjt() {
    // Fixed base bias, no Early effect: Ic = bf * Ib in the active region.
    Netlist nl = parseNetlistText(R"json({
        "models": {"n1": {"type": "npn", "is": 1e-14, "bf": 100}},
        "components": [
            {"type": "V", "name": "VCC", "nodes": ["vcc", "0"], "dc": 10},
            {"type": "R", "name": "RB", "nodes": ["vcc", "b"], "value": "1meg"},
            {"type": "R", "name": "RC", "nodes": ["vcc", "c"], "value": "1k"},
            {"type": "Q", "name": "Q1", "nodes": ["c", "b", "0"], "model": "n1"}
        ]})json");
    const Circuit& ckt = *nl.circuit;
    const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
    const double ib = (10.0 - probe(ckt, x, "v(b)")) / 1e6;
    const double ic = (10.0 - probe(ckt, x, "v(c)")) / 1e3;
    checkNear(ic / ib, 100.0, 0.1, "npn current gain");
    checkTrue(probe(ckt, x, "v(b)") > 0.55 && probe(ckt, x, "v(b)") < 0.75, "npn vbe plausible");

    // The mirrored pnp circuit must give the mirrored solution.
    Netlist pl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "VCC", "nodes": ["vcc", "0"], "dc": -10},
            {"type": "R", "name": "RB", "nodes": ["vcc", "b"], "value": "1meg"},
            {"type": "R", "name": "RC", "nodes": ["vcc", "c"], "value": "1k"},
            {"type": "pnp", "name": "Q1", "nodes": ["c", "b", "0"], "is": 1e-14, "bf": 100}
        ]})json");
    const Vector xp = solveOperatingPoint(*pl.circuit, pl.options);
    checkNear(probe(*pl.circuit, xp, "v(c)"), -probe(ckt, x, "v(c)"), 1e-6, "pnp mirrors npn (vc)");
    checkNear(probe(*pl.circuit, xp, "v(b)"), -probe(ckt, x, "v(b)"), 1e-6, "pnp mirrors npn (vb)");
}

void testCmosInverter() {
    const char* text = R"json({
        "models": {
            "n": {"type": "nmos", "vto": 1.0, "kp": 1e-4, "lambda": 0.02},
            "p": {"type": "pmos", "vto": -1.0, "kp": 1e-4, "lambda": 0.02}
        },
        "components": [
            {"type": "V", "name": "VDD", "nodes": ["vdd", "0"], "dc": 5},
            {"type": "V", "name": "VIN", "nodes": ["in", "0"], "dc": 0},
            {"type": "M", "name": "MP", "nodes": ["out", "in", "vdd", "vdd"], "model": "p"},
            {"type": "M", "name": "MN", "nodes": ["out", "in", "0", "0"], "model": "n"}
        ]})json";
    const double vin[] = {0.0, 2.5, 5.0};
    const double vout[] = {5.0, 2.5, 0.0};
    for (int k = 0; k < 3; ++k) {
        nlohmann::json j = nlohmann::json::parse(text);
        j["components"][1]["dc"] = vin[k];
        Netlist nl = parseNetlist(j);
        const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
        checkNear(probe(*nl.circuit, x, "v(out)"), vout[k], 1e-3,
                  "cmos inverter vout at vin = " + std::to_string(vin[k]));
    }

    // Saturation current of the n-channel alone: id = kp/2 (vgs - vto)^2 (1 + lambda vds)
    Netlist nl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": 5},
            {"type": "V", "name": "VG", "nodes": ["g", "0"], "dc": 3},
            {"type": "nmos", "name": "M1", "nodes": ["d", "g", "0"], "vto": 1, "kp": 1e-4,
             "lambda": 0.02, "w": "10u", "l": "1u"}
        ]})json");
    const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
    checkNear(-probe(*nl.circuit, x, "i(VD)"), 0.5 * 1e-3 * 4.0 * 1.1, 1e-8, "nmos saturation current");
}

void testAc() {
    // RC low-pass: |H| = 1/sqrt(2) and -45 deg at fc = 1 / (2 pi R C).
    Netlist nl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 0, "ac": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "1u"}
        ]})json");
    Circuit& ckt = *nl.circuit;
    const Vector xop = solveOperatingPoint(ckt, nl.options);
    const double fc = 1.0 / (2.0 * kPi * 1e3 * 1e-6);
    const Probe out = parseProbe(ckt, "v(out)");
    runAc(ckt, xop, {fc, 100.0 * fc}, [&](double f, const Eigen::VectorXcd& x) {
        const std::complex<double> h = out.phasor(x);
        const std::complex<double> ref = 1.0 / std::complex<double>(1.0, f / fc);
        checkNear(std::abs(h - ref), 0.0, 1e-9, "rc low-pass transfer function");
    });

    // Series RLC: at resonance the full source voltage appears across R.
    Netlist rl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "ac": 1},
            {"type": "L", "name": "L1", "nodes": ["in", "a"], "value": "10m"},
            {"type": "C", "name": "C1", "nodes": ["a", "b"], "value": "100n"},
            {"type": "R", "name": "R1", "nodes": ["b", "0"], "value": "100"}
        ]})json");
    const Vector xr = solveOperatingPoint(*rl.circuit, rl.options);
    const double f0 = 1.0 / (2.0 * kPi * std::sqrt(10e-3 * 100e-9));
    const Probe vb = parseProbe(*rl.circuit, "v(b)");
    runAc(*rl.circuit, xr, {f0}, [&](double, const Eigen::VectorXcd& x) {
        checkNear(std::abs(vb.phasor(x)), 1.0, 1e-9, "rlc resonance magnitude");
    });

    const auto f = frequencySweep("dec", 10, 1.0, 1000.0);
    checkTrue(f.size() == 31, "decade sweep has 31 points");
    checkNear(f.back(), 1000.0, 1e-9, "decade sweep ends at fstop");
}

void testRobertson() {
    // Stiff index-1 DAE without any circuit, reference values from the literature.
    Robertson sys;
    Vector y0(3);
    y0 << 1.0, 0.0, 0.0;
    for (IntegrationMethod m : {IntegrationMethod::Bdf2, IntegrationMethod::Trapezoidal,
                                IntegrationMethod::Bdf}) {
        TransientOptions opt;
        opt.tstop = 40.0;
        opt.hinit = 1e-6;
        opt.method = m;
        opt.reltol = 1e-5;
        opt.newton.reltol = 1e-5;
        Vector y;
        double maxConstraint = 0.0;
        const TransientStats s = runTransient(sys, y0, nullptr, opt, [&](double, const Vector& v) {
            y = v;
            maxConstraint = std::max(maxConstraint, std::abs(v.sum() - 1.0));
        });
        const std::string tag = std::string("robertson ") + methodName(m);
        checkNear(y[0], 0.7158270687, 2e-4, tag + " y1(40)");
        checkNear(y[1], 9.185534765e-6, 5e-9, tag + " y2(40)");
        checkNear(y[2], 0.2841637457, 2e-4, tag + " y3(40)");
        checkNear(maxConstraint, 0.0, 1e-9, tag + " algebraic constraint");
        std::cout << "    " << tag << ": " << s.accepted << " steps, " << s.rejected
                  << " rejected, order up to " << s.maxOrder << "\n";
        if (m == IntegrationMethod::Bdf)
            checkTrue(s.maxOrder >= 4, "variable-order BDF reaches a high order on a smooth problem");
    }
}

// --- expressions and equations -------------------------------------------------

// Two unknowns x (index 0) and y (index 1) and one constant.
struct XySymbols final : expr::Symbols {
    bool constant(const std::string& name, double& value) const override {
        if (name != "two") return false;
        value = 2.0;
        return true;
    }
    int variable(const std::string& name) const override {
        return name == "x" ? 0 : name == "y" ? 1 : -1;
    }
    int nodeVoltage(const std::string&) const override { throw std::runtime_error("no nodes"); }
    int branchCurrent(const std::string&) const override { throw std::runtime_error("no branches"); }
};

bool throws(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void testExpressions() {
    const std::map<std::string, double> par = {{"R", 1e3}, {"C", 1e-6}};
    const std::pair<const char*, double> cases[] = {
        {"2 + 3*4", 14.0},
        {"-2^2", -4.0},
        {"2^3^2", 512.0},
        {"2**-1", 0.5},
        {"(1 + 2) * (3 - 5) / 4", -1.5},
        {"4.7k / 1k", 4.7},
        {"1e-3 * 1meg", 1000.0},
        {"10m + 5u", 0.010005},
        {"if(1 < 2, 10, 20) + if(2 <= 1 || !(3 == 3), 1, 2)", 12.0},
        {"min(3, max(1, 2)) + sign(-4)", 1.0},
        {"sqrt(16) + abs(-3) + pow(2, 3)", 15.0},
        {"sin(pi/2) + cos(0) + exp(0) + ln(1) + log10(100)", 5.0},
        {"1 / (2*pi*R*C)", 1.0 / (2.0 * kPi * 1e-3)},
        {"3 > 2 && 2 >= 2 && 1 != 2", 1.0},
    };
    for (const auto& [text, expected] : cases)
        checkNear(expr::evalConstant(text, par), expected, 1e-12 * std::max(1.0, std::abs(expected)),
                  std::string("expression ") + text);

    for (const char* bad : {"2pi", "1 +", "foo(1)", "(1 + 2", "1 2", "unknown_name", "sin()",
                            "sin(1, 2)", "2 * t", "1 = 2", ""})
        checkTrue(throws([&] { expr::evalConstant(bad, par); }),
                  std::string("expression \"") + bad + "\" is rejected");

    // Gradient from automatic differentiation against central differences.
    const XySymbols sym;
    const char* text =
        "x^2*sin(y) + exp(x*y)/(1 + y^2) + tanh(x - y)*sqrt(x + 3) + atan2(x, y) + pow(y, x)"
        " + if(x > y, x*y, x/y) + log(1 + x*x) + abs(x - two*y) + min(x, y)*max(x, 0.1) - cos(x)/y";
    const expr::Program prog = expr::Program::compile(expr::parseExpression(text), sym);
    for (const auto& pt : {std::pair<double, double>{0.7, 1.3}, {2.1, 0.4}, {1.5, 2.5}}) {
        Vector x(2);
        x << pt.first, pt.second;
        std::vector<double> grad;
        prog.evalGrad(x, 0.0, grad);
        checkTrue(prog.vars().size() == 2, "expression depends on two unknowns");
        for (size_t k = 0; k < prog.vars().size(); ++k) {
            const int j = prog.vars()[k];
            const double h = 1e-6;
            Vector xp = x, xm = x;
            xp[j] += h;
            xm[j] -= h;
            const double fd = (prog.eval(xp, 0.0) - prog.eval(xm, 0.0)) / (2.0 * h);
            checkNear(grad[k], fd, 1e-6 * std::max(1.0, std::abs(fd)), "gradient of expression");
        }
    }

    // Equations are split into d/dt q + f = 0.
    Vector x(2);
    x << 3.0, 8.0;
    expr::DaeEquation eq = expr::compileEquation("two*der(x) + der(y)/4 = -x + t", sym);
    checkTrue(eq.differential, "equation with der() is differential");
    checkNear(eq.q.eval(x, 1.0), 2.0 * 3.0 + 8.0 / 4.0, 1e-12, "q of equation");
    checkNear(eq.f.eval(x, 1.0), 3.0 - 1.0, 1e-12, "f of equation");
    eq = expr::compileEquation("-(der(x*y) - y) = 0", sym);
    checkNear(eq.q.eval(x, 0.0), -24.0, 1e-12, "q with negated nonlinear der()");
    checkNear(eq.f.eval(x, 0.0), 8.0, 1e-12, "f with negated der()");
    eq = expr::compileEquation("0 = x + y - 1", sym);
    checkTrue(!eq.differential, "equation without der() is algebraic");
    checkNear(eq.f.eval(x, 0.0), -10.0, 1e-12, "f of algebraic equation");

    for (const char* bad : {"x*der(y) = 1", "der(der(x)) = 0", "der(x)^2 = 1", "x + y",
                            "der(t*x) = 0", "sin(der(x)) = 0", "der(x) = der(y)/y"})
        checkTrue(throws([&] { expr::compileEquation(bad, sym); }),
                  std::string("equation \"") + bad + "\" is rejected");
}

void testEquationSystem() {
    // Robertson from the input file. y3 is algebraic: its (wrong) start value
    // must be corrected by the consistent initialization.
    Netlist nl = parseNetlistText(R"json({
        "parameters": {"k1": 0.04, "k2": 1e4, "k3": 3e7},
        "variables": {"y1": 1, "y2": {"init": 0, "abstol": 1e-10}, "y3": {"guess": 0.7}},
        "equations": [
            "der(y1) = -k1*y1 + k2*y2*y3",
            "der(y2) =  k1*y1 - k2*y2*y3 - k3*y2^2",
            "0 = y1 + y2 + y3 - 1"
        ],
        "options": {"reltol": 1e-5}
    })json");
    const Circuit& ckt = *nl.circuit;
    const Trace tr = transient(nl, 40.0, IntegrationMethod::Bdf2, false);
    checkNear(probe(ckt, tr.x.front(), "y1"), 1.0, 1e-12, "robertson json: y1(0) held");
    checkNear(probe(ckt, tr.x.front(), "y3"), 0.0, 1e-9, "robertson json: y3(0) made consistent");
    checkNear(probe(ckt, tr.x.back(), "y1"), 0.7158270687, 2e-4, "robertson json y1(40)");
    checkNear(probe(ckt, tr.x.back(), "y2"), 9.185534765e-6, 5e-9, "robertson json y2(40)");
    checkNear(probe(ckt, tr.x.back(), "y3"), 0.2841637457, 2e-4, "robertson json y3(40)");
    checkNear(probe(ckt, tr.x.back(), "y1 + y2 + y3 - 1"), 0.0, 1e-9, "robertson json constraint");

    // Harmonic oscillator as a first-order system with a mass in front of der().
    Netlist osc = parseNetlistText(R"json({
        "parameters": {"m": 2, "k": 8},
        "variables": {"x": 1, "v": 0},
        "equations": ["der(x) = v", "m*der(v) = -k*x"],
        "options": {"reltol": 1e-6}
    })json");
    const Trace to = transient(osc, 3.0, IntegrationMethod::Trapezoidal, false);
    double maxErr = 0.0;
    for (size_t k = 0; k < to.t.size(); ++k)
        maxErr = std::max(maxErr, std::abs(probe(*osc.circuit, to.x[k], "x") - std::cos(2.0 * to.t[k])));
    checkNear(maxErr, 0.0, 2e-3, "harmonic oscillator x(t) = cos(2 t)");

    // States without a prescribed value start in steady state.
    Netlist ss = parseNetlistText(R"json({
        "variables": {"x": {"guess": 0}},
        "equations": ["der(x) = 3 - x"]
    })json");
    const Trace ts = transient(ss, 1.0, IntegrationMethod::Bdf2, false);
    checkNear(probe(*ss.circuit, ts.x.front(), "x"), 3.0, 1e-9, "free state starts in steady state");

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    checkTrue(fails(R"json({"variables": {"x": 1, "y": 1}, "equations": ["der(x) = -x"]})json"),
              "number of equations must match the variables");
    checkTrue(fails(R"json({"variables": {"x": 1, "y": 1},
                        "equations": ["der(x + y) = -x", "0 = x - y"]})json"),
              "more initial values than differential equations is rejected");
    checkTrue(fails(R"json({"variables": {"x": 1}, "equations": ["der(x) = -z"]})json"),
              "unknown symbol in an equation is rejected");
    checkTrue(fails(R"json({"parameters": {"a": "b + 1", "b": "a + 1"},
                        "variables": {"x": 1}, "equations": ["der(x) = -x"]})json"),
              "circular parameters are rejected");
    checkTrue(fails(R"json({"variables": {"t": 1}, "equations": ["der(t) = 1"]})json"),
              "reserved variable name is rejected");
}

void testCoupledSystem() {
    // An equation follows a node voltage (first-order lag), an algebraic
    // equation scales it, and a behavioral source feeds it back into the circuit.
    const char* text = R"json({
        "parameters": {"tau": "1m", "Rdiv": "1k"},
        "variables": {"x": 0, "y": {"guess": 0}},
        "equations": ["tau*der(x) = v(out) - x", "y = 2*x + 1"],
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 4, "ac": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "Rdiv"},
            {"type": "R", "name": "R2", "nodes": ["out", "0"], "value": "2*Rdiv/2"},
            {"type": "B", "name": "B1", "nodes": ["b", "0"], "v": "y"},
            {"type": "R", "name": "R3", "nodes": ["b", "0"], "value": "1k"}
        ],
        "options": {"reltol": 1e-5}
    })json";
    Netlist nl = parseNetlistText(text);
    Circuit& ckt = *nl.circuit;

    const Trace tr = transient(nl, 5e-3, IntegrationMethod::Bdf2, false);
    checkNear(probe(ckt, tr.x.front(), "v(out)"), 2.0, 1e-9, "coupled: circuit starts at its operating point");
    checkNear(probe(ckt, tr.x.front(), "x"), 0.0, 1e-12, "coupled: state held at its initial value");
    checkNear(probe(ckt, tr.x.front(), "v(b)"), 1.0, 1e-9, "coupled: algebraic variable consistent at t = 0");
    double maxErr = 0.0;
    for (size_t k = 0; k < tr.t.size(); ++k) {
        const double ref = 2.0 * (1.0 - std::exp(-tr.t[k] / 1e-3));
        maxErr = std::max(maxErr, std::abs(probe(ckt, tr.x[k], "x") - ref));
        maxErr = std::max(maxErr, std::abs(probe(ckt, tr.x[k], "v(b)") - (2.0 * ref + 1.0)));
    }
    checkNear(maxErr, 0.0, 1e-3, "coupled: lag follows the node voltage");
    checkNear(probe(ckt, tr.x.back(), "v(b) * i(B1)"), -std::pow(probe(ckt, tr.x.back(), "y"), 2) / 1e3,
              1e-9, "expression output v(b)*i(B1)");

    // Operating point: steady state of everything, nothing is held.
    const Vector xop = solveOperatingPoint(ckt, nl.options);
    checkNear(probe(ckt, xop, "x"), 2.0, 1e-9, "coupled op: x");
    checkNear(probe(ckt, xop, "y"), 5.0, 1e-9, "coupled op: y");

    // AC: the equation is a low-pass with corner 1 / (2 pi tau) behind the divider.
    const double fc = 1.0 / (2.0 * kPi * 1e-3);
    const Probe px = parseProbe(ckt, "x"), pb = parseProbe(ckt, "v(b)");
    runAc(ckt, xop, {fc}, [&](double, const Eigen::VectorXcd& X) {
        const std::complex<double> ref = 0.5 / std::complex<double>(1.0, 1.0);
        checkNear(std::abs(px.phasor(X) - ref), 0.0, 1e-9, "coupled ac: transfer function to x");
        checkNear(std::abs(pb.phasor(X) - 2.0 * ref), 0.0, 1e-9, "coupled ac: transfer function to v(b)");
    });
    checkTrue(throws([&] { parseProbe(ckt, "x*y").phasor(Eigen::VectorXcd::Zero(ckt.size())); }),
              "ac rejects nonlinear output expressions");
}

void testBehavioralSources() {
    // Nonlinear resistor i = v^3 behind 1 Ohm from 2 V: v = 1.
    Netlist nl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 2},
            {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": 1},
            {"type": "B", "name": "B1", "nodes": ["a", "0"], "i": "v(a)^3"}
        ]})json");
    const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
    checkNear(probe(*nl.circuit, x, "v(a)"), 1.0, 1e-6, "behavioral current source");

    // A charge expression q = C0 v + C1 v^2 is a voltage-dependent capacitor:
    // (C0 + 2 C1 v) dv/dt = (1 - v) / R. Compare with the implicit solution
    // t / R = -(C0 + 2 C1) ln(1 - v) - 2 C1 v.
    Netlist nc = parseNetlistText(R"json({
        "parameters": {"C0": "1u", "C1": "0.5u"},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k"},
            {"type": "B", "name": "B1", "nodes": ["out", "0"], "q": "C0*v(out) + C1*v(out)^2"}
        ],
        "options": {"reltol": 1e-6}
    })json");
    const Trace tr = transient(nc, 5e-3, IntegrationMethod::Bdf2, true);
    double maxErr = 0.0;
    for (size_t k = 0; k < tr.t.size(); ++k) {
        const double v = probe(*nc.circuit, tr.x[k], "v(out)");
        const double tRef = 1e3 * (-(1e-6 + 2.0 * 0.5e-6) * std::log(1.0 - v) - 2.0 * 0.5e-6 * v);
        maxErr = std::max(maxErr, std::abs(tRef - tr.t[k]));
    }
    checkNear(maxErr, 0.0, 5e-6, "nonlinear capacitor from a charge expression");

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    checkTrue(fails(R"json({"components": [{"type": "B", "nodes": ["a", "0"], "i": "v(nope)"}]})json"),
              "behavioral source with unknown node is rejected");
    checkTrue(fails(R"json({"components": [{"type": "B", "nodes": ["a", "0"], "i": "1", "v": "2"}]})json"),
              "behavioral source with two definitions is rejected");
}

// --- events ---------------------------------------------------------------------

// Times at which a probe jumps between two consecutive output points.
std::vector<double> jumpTimes(const Circuit& ckt, const Trace& tr, const std::string& expr,
                              double minJump) {
    const Probe p = parseProbe(ckt, expr);
    std::vector<double> times;
    for (size_t k = 1; k < tr.t.size(); ++k)
        if (std::abs(p.value(tr.x[k], tr.t[k]) - p.value(tr.x[k - 1], tr.t[k - 1])) > minJump)
            times.push_back(tr.t[k - 1]);
    return times;
}

void testEvents() {
    // Bouncing ball: the velocity is reversed whenever the height becomes negative.
    Netlist ball = parseNetlistText(R"json({
        "parameters": {"g": 9.81, "e": 0.8},
        "variables": {"h": 1, "v": 0},
        "equations": ["der(h) = v", "der(v) = -g"],
        "events": [{"when": "h < 0", "set": {"v": "-e*v"}}],
        "options": {"reltol": 1e-6}
    })json");
    TransientStats stats;
    const Trace tb = transient(ball, 2.0, IntegrationMethod::Bdf2, false, &stats);
    const double t1 = std::sqrt(2.0 / 9.81);
    const std::vector<double> impacts = jumpTimes(*ball.circuit, tb, "v", 1.0);
    checkTrue(impacts.size() >= 3, "bouncing ball: at least three impacts");
    if (impacts.size() >= 3) {
        checkNear(impacts[0], t1, 1e-6, "bouncing ball: first impact");
        checkNear(impacts[1], t1 * (1.0 + 2.0 * 0.8), 1e-5, "bouncing ball: second impact");
        checkNear(impacts[2], t1 * (1.0 + 2.0 * 0.8 + 2.0 * 0.64), 1e-5, "bouncing ball: third impact");
    }
    double apex = 0.0;
    for (size_t k = 0; k < tb.t.size(); ++k)
        if (tb.t[k] > t1 && tb.t[k] < t1 * 2.6) apex = std::max(apex, probe(*ball.circuit, tb.x[k], "h"));
    checkNear(apex, 0.64, 1e-4, "bouncing ball: height after the first bounce");
    std::cout << "    bouncing ball: " << stats.accepted << " steps, " << stats.events << " events\n";

    // Two-point controller with hysteresis: a discrete variable switches the heater.
    Netlist th = parseNetlistText(R"json({
        "variables": {"T": 20, "heat": {"discrete": 1}},
        "equations": ["der(T) = 30*heat - T"],
        "events": [
            {"when": "T > 22", "set": {"heat": 0}},
            {"when": "T < 18", "set": {"heat": 1}}
        ],
        "options": {"reltol": 1e-9}
    })json");
    const Trace tt = transient(th, 2.0, IntegrationMethod::Bdf, false);
    const std::vector<double> sw = jumpTimes(*th.circuit, tt, "heat", 0.5);
    const double tOff = std::log(10.0 / 8.0), cool = std::log(22.0 / 18.0), warm = std::log(12.0 / 8.0);
    checkTrue(sw.size() >= 4, "thermostat: at least four switchings");
    if (sw.size() >= 4) {
        checkNear(sw[0], tOff, 1e-6, "thermostat: heater off");
        checkNear(sw[1], tOff + cool, 1e-6, "thermostat: heater on");
        checkNear(sw[2], tOff + cool + warm, 1e-6, "thermostat: heater off again");
        checkNear(sw[3], tOff + 2.0 * cool + warm, 1e-6, "thermostat: heater on again");
    }
    // In the DC analyses a discrete variable keeps its value.
    const Vector xop = solveOperatingPoint(*th.circuit, th.options);
    checkNear(probe(*th.circuit, xop, "T"), 30.0, 1e-9, "thermostat op: T with the heater on");

    // Kinks and jumps of if(), max() and abs() are located without a when-clause.
    Netlist kink = parseNetlistText(R"json({
        "variables": {"z": 0, "w": 0, "a": 0},
        "equations": ["der(z) = if(t < 1, 1, -2)", "der(w) = max(0, 1 - t)", "der(a) = abs(t - 0.5)"],
        "options": {"reltol": 1e-4}
    })json");
    const Trace tk = transient(kink, 2.0, IntegrationMethod::Bdf2, false, &stats);
    checkNear(probe(*kink.circuit, tk.x.back(), "z"), -1.0, 1e-7, "jump of if() located: z(2)");
    checkNear(probe(*kink.circuit, tk.x.back(), "w"), 0.5, 1e-5, "kink of max() located: w(2)");
    checkNear(probe(*kink.circuit, tk.x.back(), "a"), 1.25, 1e-5, "kink of abs() located: a(2)");
    bool at1 = false, atHalf = false;
    for (double t : tk.t) {
        at1 = at1 || std::abs(t - 1.0) < 1e-7;
        atHalf = atHalf || std::abs(t - 0.5) < 1e-7;
    }
    checkTrue(at1 && atHalf, "time points at the kinks t = 0.5 and t = 1");

    // Circuit with events: RC relaxation oscillator with an ideal Schmitt trigger.
    Netlist osc = parseNetlistText(R"json({
        "parameters": {"Vcc": 5},
        "variables": {"s": {"discrete": 1}},
        "equations": [],
        "events": [
            {"when": "v(c) > 2*Vcc/3", "set": {"s": 0}},
            {"when": "v(c) < Vcc/3", "set": {"s": 1}}
        ],
        "components": [
            {"type": "B", "name": "Bout", "nodes": ["out", "0"], "v": "Vcc*s"},
            {"type": "R", "name": "R1", "nodes": ["out", "c"], "value": "10k"},
            {"type": "C", "name": "C1", "nodes": ["c", "0"], "value": "100n", "ic": 0}
        ],
        "options": {"reltol": 1e-9, "vntol": 1e-10}
    })json");
    const Trace to = transient(osc, 5e-3, IntegrationMethod::Bdf, true);
    const std::vector<double> flips = jumpTimes(*osc.circuit, to, "s", 0.5);
    checkTrue(flips.size() >= 4, "relaxation oscillator: at least four switchings");
    if (flips.size() >= 4) {
        checkNear(flips[0], 1e-3 * std::log(3.0), 1e-9, "relaxation oscillator: first switching");
        checkNear(flips[2] - flips[0], 2e-3 * std::log(2.0), 1e-9, "relaxation oscillator: period");
        checkNear(flips[3] - flips[1], 2e-3 * std::log(2.0), 1e-9, "relaxation oscillator: period (2)");
    }

    // Endless switching is detected instead of running forever.
    Netlist chatter = parseNetlistText(R"json({
        "variables": {"x": 0},
        "equations": ["der(x) = if(x < 1, 1, -1)"]
    })json");
    checkTrue(throws([&] {
                  Circuit& ckt = *chatter.circuit;
                  ckt.setTransientMode(true);
                  TransientOptions o;
                  o.tstop = 5.0;
                  o.maxEvents = 200;
                  runTransient(ckt, ckt.initialGuess(), nullptr, o, nullptr);
              }),
              "chattering is reported");

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    checkTrue(fails(R"json({"variables": {"x": 1, "y": {"guess": 0}},
                        "equations": ["der(x) = -x", "y = 2*x"],
                        "events": [{"when": "x < 0.5", "set": {"y": 1}}]})json"),
              "assigning an algebraic variable is rejected");
    checkTrue(fails(R"json({"variables": {"x": 1}, "equations": ["der(x) = -x"],
                        "events": [{"when": "x < 0.5", "set": {"q": 1}}]})json"),
              "assigning an unknown variable is rejected");
    checkTrue(fails(R"json({"variables": {"x": 1}, "equations": ["der(x) = -x"],
                        "events": [{"when": "x < 0.5"}]})json"),
              "event without assignments is rejected");
}

void testSwitchingElements() {
    // Voltage-controlled switch in the operating point (SPICE node form).
    for (double vc : {0.0, 5.0}) {
        nlohmann::json j = nlohmann::json::parse(R"json({
            "components": [
                {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 10},
                {"type": "V", "name": "VC", "nodes": ["c", "0"], "dc": 0},
                {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": "1k"},
                {"type": "S", "name": "S1", "nodes": ["a", "0", "c", "0"], "vt": 2.5, "ron": 1, "roff": "1meg"}
            ]})json");
        j["components"][1]["dc"] = vc;
        Netlist nl = parseNetlist(j);
        const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
        const double expected = vc > 2.5 ? 10.0 * 1.0 / 1001.0 : 10.0 * 1e6 / (1e6 + 1e3);
        checkNear(probe(*nl.circuit, x, "v(a)"), expected, 1e-9,
                  vc > 2.5 ? "switch closed in the operating point" : "switch open in the operating point");
    }

    // Hysteresis: a triangle 0 -> 5 -> 0 closes the switch at 3 V and opens it at 2 V.
    Netlist hy = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 10},
            {"type": "V", "name": "VC", "nodes": ["c", "0"],
             "wave": {"type": "pwl", "points": [[0, 0], [1, 5], [2, 0]]}},
            {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": "1k"},
            {"type": "S", "name": "S1", "nodes": ["a", "0", "c", "0"], "vt": 2.5, "vh": 0.5}
        ]})json");
    const Trace th = transient(hy, 2.0, IntegrationMethod::Bdf2, false);
    const std::vector<double> sw = jumpTimes(*hy.circuit, th, "v(a)", 5.0);
    checkTrue(sw.size() == 2, "switch with hysteresis: two switchings");
    if (sw.size() == 2) {
        checkNear(sw[0], 0.6, 1e-7, "switch closes at threshold + hysteresis");
        checkNear(sw[1], 1.6, 1e-7, "switch opens at threshold - hysteresis");
    }

    // Controlled by an expression of time.
    Netlist ex = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 10},
            {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": "1k"},
            {"type": "S", "name": "S1", "nodes": ["a", "0"], "control": "sin(2*pi*t)", "initial": "on"}
        ]})json");
    const Trace te = transient(ex, 1.2, IntegrationMethod::Bdf2, false);
    const std::vector<double> se = jumpTimes(*ex.circuit, te, "v(a)", 5.0);
    checkTrue(se.size() == 2, "expression-controlled switch: two switchings");
    if (se.size() == 2) {
        checkNear(se[0], 0.5, 1e-7, "expression-controlled switch opens");
        checkNear(se[1], 1.0, 1e-7, "expression-controlled switch closes");
    }

    // Ideal diode: forward and reverse operating point.
    for (double v : {5.0, -5.0}) {
        nlohmann::json j = nlohmann::json::parse(R"json({
            "components": [
                {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 5},
                {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": "1k"},
                {"type": "ideal_diode", "name": "D1", "nodes": ["a", "0"], "vf": 0.7, "ron": 1, "roff": "1meg"}
            ]})json");
        j["components"][0]["dc"] = v;
        Netlist nl = parseNetlist(j);
        const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
        // forward: slopes 1/roff below and 1/ron above vf; reverse: roff only
        const double g1 = 1e-6, g2 = 1.0, gr = 1e-3;
        const double expected = v > 0 ? (gr * v + (g2 - g1) * 0.7) / (gr + g2) : v * gr / (gr + g1);
        checkNear(probe(*nl.circuit, x, "v(a)"), expected, 1e-9,
                  v > 0 ? "ideal diode conducting" : "ideal diode blocking");
    }

    // Switching off an inductor with a freewheeling diode: the current commutates
    // to the diode at the instant the switch opens, without a voltage spike.
    Netlist kick = parseNetlistText(R"json({
        "options": {"reltol": 1e-6},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 10},
            {"type": "V", "name": "VG", "nodes": ["g", "0"],
             "wave": {"type": "pulse", "v1": 1, "v2": 0, "delay": "1m", "rise": "1u"}},
            {"type": "S", "name": "S1", "nodes": ["in", "sw", "g", "0"], "vt": 0.5},
            {"type": "ideal_diode", "name": "D1", "nodes": ["0", "sw"]},
            {"type": "L", "name": "L1", "nodes": ["sw", "out"], "value": "1m"},
            {"type": "R", "name": "R1", "nodes": ["out", "0"], "value": 10}
        ]})json");
    const Trace tk = transient(kick, 1.5e-3, IntegrationMethod::Bdf2, false);
    const double tOpen = 1.0005e-3, tau = 1e-4;
    double minSw = 0.0, maxErr = 0.0;
    for (size_t k = 0; k < tk.t.size(); ++k) {
        minSw = std::min(minSw, probe(*kick.circuit, tk.x[k], "v(sw)"));
        const double ref = tk.t[k] <= tOpen ? 10.0 / 10.001 : std::exp(-(tk.t[k] - tOpen) / tau);
        maxErr = std::max(maxErr, std::abs(probe(*kick.circuit, tk.x[k], "i(L1)") - ref));
    }
    checkTrue(minSw > -0.01, "freewheeling diode takes over without a voltage spike");
    checkNear(maxErr, 0.0, 2e-3, "inductor current decays through the diode");

    // Buck converter in steady state: the mean output voltage is duty cycle * input.
    Netlist buck = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "VIN", "nodes": ["in", "0"], "dc": 12},
            {"type": "V", "name": "PWM", "nodes": ["g", "0"],
             "wave": {"type": "pulse", "v1": 0, "v2": 1, "rise": "10n", "fall": "10n",
                      "width": "4.99u", "period": "10u"}},
            {"type": "S", "name": "S1", "nodes": ["in", "sw", "g", "0"], "vt": 0.5},
            {"type": "ideal_diode", "name": "D1", "nodes": ["0", "sw"]},
            {"type": "L", "name": "L1", "nodes": ["sw", "out"], "value": "100u", "ic": 1.2},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "100u", "ic": 6},
            {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 5}
        ]})json");
    TransientStats stats;
    const Trace tb = transient(buck, 1e-3, IntegrationMethod::Bdf2, true, &stats);
    double integral = 0.0;
    for (size_t k = 1; k < tb.t.size(); ++k)
        if (tb.t[k - 1] >= 0.5e-3)
            integral += 0.5 * (probe(*buck.circuit, tb.x[k], "v(out)") + probe(*buck.circuit, tb.x[k - 1], "v(out)")) *
                        (tb.t[k] - tb.t[k - 1]);
    checkNear(integral / 0.5e-3, 6.0, 0.06, "buck converter: mean output voltage");
    std::cout << "    buck converter: " << stats.accepted << " steps, " << stats.events
              << " events, mean output " << integral / 0.5e-3 << " V\n";

    // A rectifier whose DC side floats against ground (tied down by 10 MOhm
    // only). The potential of that side is poorly determined; neither the step
    // size control nor Newton may get stuck on its rounding noise.
    Netlist fl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "Vs", "nodes": ["s1", "s2"],
             "wave": {"type": "sin", "amplitude": 17, "freq": 50}},
            {"type": "R", "name": "Rref", "nodes": ["s2", "0"], "value": "10meg"},
            {"type": "ideal_diode", "name": "D1", "nodes": ["s1", "plus"], "vf": 0.7, "ron": "50m"},
            {"type": "ideal_diode", "name": "D2", "nodes": ["s2", "plus"], "vf": 0.7, "ron": "50m"},
            {"type": "ideal_diode", "name": "D3", "nodes": ["minus", "s1"], "vf": 0.7, "ron": "50m"},
            {"type": "ideal_diode", "name": "D4", "nodes": ["minus", "s2"], "vf": 0.7, "ron": "50m"},
            {"type": "C", "name": "C1", "nodes": ["plus", "minus"], "value": "2200u"},
            {"type": "R", "name": "RL", "nodes": ["plus", "minus"], "value": 33},
            {"type": "R", "name": "Rgnd", "nodes": ["minus", "0"], "value": "10meg"}
        ]})json");
    const Trace tf = transient(fl, 0.1, IntegrationMethod::Bdf2, false, &stats);
    double vmin = 1e9, vmax = -1e9;
    for (size_t k = 0; k < tf.t.size(); ++k) {
        if (tf.t[k] < 0.06) continue;
        const double v = probe(*fl.circuit, tf.x[k], "v(plus,minus)");
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    }
    checkNear(vmax, 17.0 - 1.4, 0.15, "floating bridge rectifier: peak of the output voltage");
    checkTrue(vmin > 12.5 && vmin < 14.5, "floating bridge rectifier: ripple is plausible");
    // (A few failed settling steps after diode events are tolerated; they fall back.)
    checkTrue(stats.newtonFailures < 10 && stats.accepted < 5000,
              "floating bridge rectifier: no stalling on rounding noise");

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    checkTrue(fails(R"json({"components": [{"type": "S", "nodes": ["a", "0"]}]})json"),
              "switch without control is rejected");
    checkTrue(fails(R"json({"components": [{"type": "S", "nodes": ["a", "0", "c", "0"], "control": "t"}]})json"),
              "switch with two kinds of control is rejected");
}

void testSubcircuits() {
    // A divider used twice with different parameters, and nested once more.
    Netlist nl = parseNetlistText(R"json({
        "parameters": {"Rtop": "1k"},
        "subcircuits": {
            "divider": {
                "ports": ["in", "out"],
                "parameters": {"ratio": 0.5, "rtotal": "2*Rtop", "rlow": "ratio*rtotal"},
                "components": [
                    {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "rtotal - rlow"},
                    {"type": "R", "name": "R2", "nodes": ["out", "0"], "value": "rlow"}
                ]
            },
            "buffered": {
                "ports": ["in", "out"],
                "components": [
                    {"type": "X", "name": "D", "subcircuit": "divider", "nodes": ["in", "mid"],
                     "parameters": {"ratio": 0.25}},
                    {"type": "E", "name": "E1", "nodes": ["out", "0", "mid", "0"], "gain": 1}
                ]
            }
        },
        "components": [
            {"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 8},
            {"type": "X", "name": "X1", "subcircuit": "divider", "nodes": ["a", "b"]},
            {"type": "X", "name": "X2", "subcircuit": "divider", "nodes": ["a", "c"],
             "parameters": {"ratio": "3/4"}},
            {"type": "X", "name": "X3", "subcircuit": "buffered", "nodes": ["a", "d"]},
            {"type": "R", "name": "RL", "nodes": ["d", "0"], "value": "1k"}
        ]})json");
    const Circuit& ckt = *nl.circuit;
    const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
    checkNear(probe(ckt, x, "v(b)"), 4.0, 1e-9, "subcircuit with default parameters");
    checkNear(probe(ckt, x, "v(c)"), 6.0, 1e-9, "subcircuit with overridden parameter");
    checkNear(probe(ckt, x, "v(d)"), 2.0, 1e-9, "nested subcircuit");
    checkNear(probe(ckt, x, "v(X3.mid)"), 2.0, 1e-9, "node inside an instance: v(X3.mid)");
    checkNear(probe(ckt, x, "i(X3.E1)"), -2e-3, 1e-12, "component inside an instance: i(X3.E1)");
    checkTrue(ckt.findDevice("X3.D.R1") != nullptr, "component of a nested instance is named X3.D.R1");
    int visible = 0;
    for (const Probe& pr : makeProbes(ckt, {})) visible += pr.label.find('.') != std::string::npos;
    checkTrue(visible == 0, "default outputs do not list the inside of instances");

    // Expressions, controlled sources and equations inside a subcircuit: a
    // self-heating resistor with its own temperature state, used twice.
    Netlist th = parseNetlistText(R"json({
        "parameters": {"Tamb": 300},
        "subcircuits": {
            "hot_resistor": {
                "ports": ["p", "n"],
                "parameters": {"R0": 100, "alpha": 0.01, "Rth": 50, "Cth": 0.02},
                "variables": {"T": "Tamb"},
                "equations": ["Cth*der(T) = v(p,n)*i(Vs) - (T - Tamb)/Rth"],
                "components": [
                    {"type": "V", "name": "Vs", "nodes": ["p", "x"], "dc": 0},
                    {"type": "B", "name": "B1", "nodes": ["x", "n"],
                     "i": "v(x,n)/(R0*(1 + alpha*(T - Tamb)))"}
                ]
            }
        },
        "components": [
            {"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 10},
            {"type": "X", "name": "RA", "subcircuit": "hot_resistor", "nodes": ["a", "0"]},
            {"type": "X", "name": "RB", "subcircuit": "hot_resistor", "nodes": ["a", "0"],
             "parameters": {"R0": 200}}
        ],
        "options": {"reltol": 1e-6}
    })json");
    const Circuit& tc = *th.circuit;
    const Trace tr = transient(th, 10.0, IntegrationMethod::Bdf2, false);
    checkNear(probe(tc, tr.x.front(), "RA.T"), 300.0, 1e-9, "state inside an instance starts at its initial value");
    // Steady state: T - Tamb = Rth * V^2 / (R0 (1 + alpha (T - Tamb)))
    for (const auto& [inst, r0] : {std::pair<const char*, double>{"RA", 100.0}, {"RB", 200.0}}) {
        const double dT = probe(tc, tr.x.back(), std::string(inst) + ".T") - 300.0;
        checkNear(dT * (1.0 + 0.01 * dT), 50.0 * 100.0 / r0, 1e-3,
                  std::string("self-heating resistor ") + inst + " reaches its steady state");
    }

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    const char* sub = R"json("subcircuits": {"s": {"ports": ["a", "b"], "parameters": {"r": 1},
        "components": [{"type": "R", "nodes": ["a", "b"], "value": "r"}]}})json";
    const auto with = [&](const std::string& instance) {
        return "{" + std::string(sub) + R"json(, "components": [{"type": "R", "nodes": ["n", "0"], "value": 1}, )json" +
               instance + "]}";
    };
    checkTrue(!fails(with(R"json({"type": "X", "subcircuit": "s", "nodes": ["n", "0"]})json").c_str()),
              "valid subcircuit instance is accepted");
    checkTrue(fails(with(R"json({"type": "X", "subcircuit": "nope", "nodes": ["n", "0"]})json").c_str()),
              "undefined subcircuit is rejected");
    checkTrue(fails(with(R"json({"type": "X", "subcircuit": "s", "nodes": ["n"]})json").c_str()),
              "wrong number of nodes is rejected");
    checkTrue(fails(with(R"json({"type": "X", "subcircuit": "s", "nodes": ["n", "0"], "parameters": {"q": 1}})json").c_str()),
              "unknown subcircuit parameter is rejected");
    checkTrue(fails(R"json({"subcircuits": {"loop": {"ports": ["a"], "components": [
                          {"type": "X", "subcircuit": "loop", "nodes": ["a"]}]}},
                        "components": [{"type": "R", "nodes": ["n", "0"], "value": 1},
                                       {"type": "X", "subcircuit": "loop", "nodes": ["n"]}]})json"),
              "recursive subcircuit is rejected");
}

void testCoupledInductors() {
    // Transformer 1 : 2 (L2 = 4 L1): the open-circuit voltage ratio is k * 2.
    Netlist nl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "ac": 1},
            {"type": "R", "name": "Rs", "nodes": ["in", "p"], "value": "1m"},
            {"type": "L", "name": "L1", "nodes": ["p", "0"], "value": 1},
            {"type": "L", "name": "L2", "nodes": ["s", "0"], "value": 4},
            {"type": "K", "name": "K1", "inductors": ["L1", "L2"], "k": 0.99},
            {"type": "R", "name": "RL", "nodes": ["s", "0"], "value": "1G"}
        ]})json");
    Circuit& ckt = *nl.circuit;
    const Vector xop = solveOperatingPoint(ckt, nl.options);
    const Probe ps = parseProbe(ckt, "v(s)");
    runAc(ckt, xop, {1000.0}, [&](double, const Eigen::VectorXcd& x) {
        checkNear(std::abs(ps.phasor(x)), 0.99 * 2.0, 1e-6, "coupled inductors: voltage ratio");
        checkNear(std::arg(ps.phasor(x)), 0.0, 1e-3, "coupled inductors: phase follows the dots");
    });

    // With a load the secondary current is the primary current divided by the ratio.
    Netlist ld = parseNetlistText(R"json({
        "options": {"reltol": 1e-6},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"],
             "wave": {"type": "sin", "amplitude": 10, "freq": 1000}},
            {"type": "R", "name": "Rs", "nodes": ["in", "p"], "value": "1m"},
            {"type": "L", "name": "L1", "nodes": ["p", "0"], "value": 1},
            {"type": "L", "name": "L2", "nodes": ["s", "0"], "value": 4},
            {"type": "K", "name": "K1", "inductors": ["L1", "L2"], "k": 0.999999},
            {"type": "R", "name": "RL", "nodes": ["s", "0"], "value": 100}
        ]})json");
    const Trace tr = transient(ld, 2e-3, IntegrationMethod::Trapezoidal, false);
    double maxErr = 0.0;
    for (size_t k = 0; k < tr.t.size(); ++k)
        maxErr = std::max(maxErr, std::abs(probe(*ld.circuit, tr.x[k], "v(s)") -
                                           20.0 * std::sin(2.0 * kPi * 1e3 * tr.t[k])));
    checkNear(maxErr, 0.0, 2e-2, "loaded transformer: secondary voltage");

    checkTrue(throws([&] {
                  parseNetlistText(R"json({"components": [
                      {"type": "L", "name": "L1", "nodes": ["a", "0"], "value": 1},
                      {"type": "R", "name": "R1", "nodes": ["a", "0"], "value": 1},
                      {"type": "K", "inductors": ["L1", "R1"], "k": 0.5}]})json");
              }),
              "coupling with something that is not an inductor is rejected");
}

void testTables() {
    using expr::Table;
    double slope = 0.0;
    const Table lin({0.0, 1.0, 3.0}, {0.0, 2.0, 2.0}, Table::Interp::Linear, Table::Extrap::Hold);
    checkNear(lin.eval(0.5, slope), 1.0, 1e-12, "linear table: value");
    checkNear(slope, 2.0, 1e-12, "linear table: slope");
    checkNear(lin.eval(2.0, slope), 2.0, 1e-12, "linear table: second interval");
    checkNear(lin.eval(-1.0, slope), 0.0, 1e-12, "table: held below the data");
    checkNear(slope, 0.0, 1e-12, "table: slope of the held part");
    checkNear(lin.eval(7.0, slope), 2.0, 1e-12, "table: held above the data");
    const Table ext({0.0, 1.0, 3.0}, {0.0, 2.0, 2.0}, Table::Interp::Linear, Table::Extrap::Linear);
    checkNear(ext.eval(-1.0, slope), -2.0, 1e-12, "table: linear extrapolation");

    // Cubic interpolation of a smooth function, with a consistent derivative.
    std::vector<double> xs, ys;
    for (int k = 0; k <= 10; ++k) {
        xs.push_back(kPi * k / 10.0);
        ys.push_back(std::sin(xs.back()));
    }
    const Table cub(xs, ys, Table::Interp::Cubic, Table::Extrap::Hold);
    double valueErr = 0.0, slopeErr = 0.0, fdErr = 0.0;
    for (double x = 0.05; x < 3.1; x += 0.0137) {
        const double v = cub.eval(x, slope);
        valueErr = std::max(valueErr, std::abs(v - std::sin(x)));
        slopeErr = std::max(slopeErr, std::abs(slope - std::cos(x)));
        double dummy;
        const double fd = (cub.eval(x + 1e-6, dummy) - cub.eval(x - 1e-6, dummy)) / 2e-6;
        fdErr = std::max(fdErr, std::abs(fd - slope));
    }
    // (The monotone interpolation trades some accuracy for the absence of overshoot.)
    checkNear(valueErr, 0.0, 5e-3, "cubic table: value");
    checkNear(slopeErr, 0.0, 0.1, "cubic table: slope");
    checkNear(fdErr, 0.0, 1e-6, "cubic table: slope is the derivative of the value");
    // Monotone data must not overshoot.
    const Table mono({0.0, 1.0, 2.0, 3.0}, {0.0, 0.0, 1.0, 1.0}, Table::Interp::Cubic, Table::Extrap::Hold);
    bool inside = true;
    for (double x = 0.0; x <= 3.0; x += 0.01) {
        const double v = mono.eval(x, slope);
        inside = inside && v >= -1e-12 && v <= 1.0 + 1e-12;
    }
    checkTrue(inside, "cubic table: no overshoot for monotone data");
    checkTrue(throws([&] { Table({0.0, 0.0}, {1.0, 2.0}, Table::Interp::Linear, Table::Extrap::Hold); }),
              "table with repeated x is rejected");

    // A characteristic from a table in a circuit: v + 500 Ohm * i(v) = 1.5 V  ->  v = 1 V.
    Netlist nl = parseNetlistText(R"json({
        "tables": {"iv": {"x": [-1, 0, 1, 2], "y": [0, 0, "1m", "3m"]}},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1.5},
            {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": 500},
            {"type": "B", "name": "B1", "nodes": ["a", "0"], "i": "iv(v(a))"}
        ]})json");
    const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
    checkNear(probe(*nl.circuit, x, "v(a)"), 1.0, 1e-6, "table as characteristic of a behavioral source");
    checkNear(probe(*nl.circuit, x, "iv(v(a)) * 1000"), 1.0, 1e-6, "table in an output expression");

    // Data from a file next to the input (header line and semicolons).
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "esim_tests";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "curve.csv");
        f << "x;unused;y\n0;9;10\n1;9;20\n2;9;40\n";
    }
    Netlist nf = parseNetlist(nlohmann::json::parse(R"json({
        "tables": {"curve": {"file": "curve.csv", "xcol": 0, "ycol": 2}},
        "variables": {"y": {"guess": 0}},
        "equations": ["y = curve(1.5)"]
    })json"), true, dir.string());
    const Vector xf = solveOperatingPoint(*nf.circuit, nf.options);
    checkNear(probe(*nf.circuit, xf, "y"), 30.0, 1e-9, "table read from a file");

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    checkTrue(fails(R"json({"tables": {"t": {"x": [0, 1], "y": [1]}},
                        "variables": {"y": 0}, "equations": ["der(y) = t(y)"]})json"),
              "table with different lengths is rejected");
    checkTrue(fails(R"json({"tables": {"t": {"x": [0, 1], "y": [1, 2]}},
                        "variables": {"y": 0}, "equations": ["der(y) = t(y, 1)"]})json"),
              "table called with two arguments is rejected");
}

void testTimedEvents() {
    // A counter that is incremented every 0.1 s, and a state that is reset at given times.
    Netlist nl = parseNetlistText(R"json({
        "variables": {"n": {"discrete": 0}, "x": 1},
        "equations": ["der(x) = -x"],
        "events": [
            {"every": 0.1, "set": {"n": "n + 1"}},
            {"at": [0.3, 0.7], "set": {"x": 5}}
        ],
        "options": {"reltol": 1e-7}
    })json");
    const Circuit& ckt = *nl.circuit;
    const Trace tr = transient(nl, 1.05, IntegrationMethod::Bdf, false);
    checkNear(probe(ckt, tr.x.back(), "n"), 10.0, 1e-12, "periodic event: ten firings in 1.05 s");
    const std::vector<double> ticks = jumpTimes(ckt, tr, "n", 0.5);
    checkTrue(ticks.size() == 10, "periodic event: number of jumps");
    double tickErr = 0.0;
    for (size_t k = 0; k < ticks.size(); ++k) tickErr = std::max(tickErr, std::abs(ticks[k] - 0.1 * (k + 1)));
    checkNear(tickErr, 0.0, 1e-12, "periodic event: fires exactly on the grid");
    checkNear(probe(ckt, tr.x.back(), "x"), 5.0 * std::exp(-0.35), 1e-5, "events at given times reset the state");

    // Assigning a node voltage: the capacitor is recharged to 2 V at t = 1 ms.
    Netlist rc = parseNetlistText(R"json({
        "components": [
            {"type": "C", "name": "C1", "nodes": ["c", "0"], "value": "1u", "ic": 1},
            {"type": "R", "name": "R1", "nodes": ["c", "0"], "value": "1k"}
        ],
        "events": [{"at": "1m", "set": {"v(c)": 2}}],
        "options": {"reltol": 1e-7}
    })json");
    const Trace t2 = transient(rc, 3e-3, IntegrationMethod::Bdf, true);
    double maxErr = 0.0;
    for (size_t k = 0; k < t2.t.size(); ++k) {
        const double t = t2.t[k];
        if (std::abs(t - 1e-3) < 1e-9) continue;  // the instant of the jump
        const double ref = t < 1e-3 ? std::exp(-t / 1e-3) : 2.0 * std::exp(-(t - 1e-3) / 1e-3);
        maxErr = std::max(maxErr, std::abs(probe(*rc.circuit, t2.x[k], "v(c)") - ref));
    }
    checkNear(maxErr, 0.0, 1e-5, "event assigns a capacitor voltage");

    // Sampled PI controller: drives the output of an RC plant to its set point.
    Netlist pi = parseNetlistText(R"json({
        "parameters": {"Ts": "1m", "Kp": 0.5, "Ki": 200, "ref": 2},
        "variables": {"u": {"discrete": 0}, "integ": {"discrete": 0}},
        "equations": [],
        "events": [
            {"every": "Ts", "set": {"integ": "integ + Ki*Ts*(ref - v(y))",
                                    "u": "Kp*(ref - v(y)) + integ + Ki*Ts*(ref - v(y))"}}
        ],
        "components": [
            {"type": "B", "name": "Bu", "nodes": ["drive", "0"], "v": "u"},
            {"type": "R", "name": "R1", "nodes": ["drive", "y"], "value": "1k"},
            {"type": "C", "name": "C1", "nodes": ["y", "0"], "value": "10u"}
        ]})json");
    const Trace t3 = transient(pi, 0.2, IntegrationMethod::Bdf, false);
    checkNear(probe(*pi.circuit, t3.x.back(), "v(y)"), 2.0, 1e-3, "sampled PI controller reaches the set point");
    const std::vector<double> updates = jumpTimes(*pi.circuit, t3, "u", 1e-6);
    bool onGrid = !updates.empty();
    for (double tu : updates) onGrid = onGrid && std::abs(tu / 1e-3 - std::round(tu / 1e-3)) < 1e-6;
    checkTrue(onGrid, "controller output changes only at the sampling instants");

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    checkTrue(fails(R"json({"variables": {"x": 1}, "equations": ["der(x) = -x"],
                        "events": [{"when": "x < 0.5", "every": 1, "set": {"x": 1}}]})json"),
              "event with two triggers is rejected");
    checkTrue(fails(R"json({"components": [{"type": "R", "nodes": ["a", "0"], "value": 1}],
                        "events": [{"at": 1, "set": {"v(nope)": 1}}]})json"),
              "assignment to an unknown node is rejected");
}

void testConstrainedMechanics() {
    // Pendulum with position and velocity constraint (index 2).
    Netlist nl = parseNetlistText(R"json({
        "parameters": {"g": 9.81, "L": 1, "theta0": 0.5},
        "variables": {"x": "L*sin(theta0)", "y": "-L*cos(theta0)", "u": 0, "v": 0,
                      "lam": {"guess": 0}, "mu": {"guess": 0}},
        "equations": [
            "der(x) = u - 2*x*mu", "der(y) = v - 2*y*mu",
            "der(u) = -2*x*lam", "der(v) = -g - 2*y*lam",
            "0 = x^2 + y^2 - L^2", "0 = x*u + y*v"
        ],
        "options": {"reltol": 1e-6}
    })json");
    const Circuit& ckt = *nl.circuit;
    TransientStats stats;
    const Trace tr = transient(nl, 10.0, IntegrationMethod::Bdf, true, &stats);
    const Probe px = parseProbe(ckt, "x"), length = parseProbe(ckt, "x^2 + y^2 - 1"),
                energy = parseProbe(ckt, "0.5*(u^2 + v^2) + 9.81*y");
    double lengthErr = 0.0, eMin = 1e9, eMax = -1e9;
    std::vector<double> up;  // upward zero crossings of x
    for (size_t k = 0; k < tr.t.size(); ++k) {
        lengthErr = std::max(lengthErr, std::abs(length.value(tr.x[k])));
        const double e = energy.value(tr.x[k]);
        eMin = std::min(eMin, e);
        eMax = std::max(eMax, e);
        if (k > 0) {
            const double a = px.value(tr.x[k - 1]), b = px.value(tr.x[k]);
            if (a < 0.0 && b >= 0.0) up.push_back(tr.t[k - 1] + (tr.t[k] - tr.t[k - 1]) * a / (a - b));
        }
    }
    checkNear(lengthErr, 0.0, 1e-9, "pendulum: the rod keeps its length");
    checkNear(eMax - eMin, 0.0, 1e-3, "pendulum: the energy is conserved");
    checkTrue(up.size() >= 4, "pendulum: several periods");
    if (up.size() >= 4)  // exact period for 0.5 rad amplitude (complete elliptic integral)
        checkNear((up.back() - up.front()) / static_cast<double>(up.size() - 1), 2.037867915, 2e-4,
                  "pendulum: period");
    std::cout << "    pendulum: " << stats.accepted << " steps, order up to " << stats.maxOrder
              << ", length error " << lengthErr << '\n';
}

// Compare the analytic Jacobians G = df/dx and C = dq/dx of a circuit with
// central differences at the point x.
void checkJacobian(Circuit& ckt, const Vector& x, const std::string& what) {
    const int n = ckt.size();
    Evaluation ev, ep, em;
    ckt.evaluate(0.0, x, LimitMode::Off, ev);
    Eigen::MatrixXd G = Eigen::MatrixXd::Zero(n, n), C = Eigen::MatrixXd::Zero(n, n);
    for (const Triplet& t : ev.G) G(t.row(), t.col()) += t.value();
    for (const Triplet& t : ev.C) C(t.row(), t.col()) += t.value();
    double worstG = 0.0, worstC = 0.0;
    for (int j = 0; j < n; ++j) {
        const double h = 1e-6;
        Vector xp = x, xm = x;
        xp[j] += h;
        xm[j] -= h;
        ckt.evaluate(0.0, xp, LimitMode::Off, ep);
        ckt.evaluate(0.0, xm, LimitMode::Off, em);
        const Vector df = (ep.f - em.f) / (2.0 * h), dq = (ep.q - em.q) / (2.0 * h);
        const double scaleG = G.col(j).cwiseAbs().maxCoeff(), scaleC = C.col(j).cwiseAbs().maxCoeff();
        // Rounding of the difference quotient itself.
        const double roundF = 1e-13 * ep.f.cwiseAbs().maxCoeff() / h, roundQ = 1e-13 * ep.q.cwiseAbs().maxCoeff() / h;
        for (int i = 0; i < n; ++i) {
            const double eg = std::abs(df[i] - G(i, j)) / (1e-5 * std::abs(G(i, j)) + 1e-6 * scaleG + roundF + 1e-30);
            const double ec = std::abs(dq[i] - C(i, j)) / (1e-5 * std::abs(C(i, j)) + 1e-6 * scaleC + roundQ + 1e-30);
            if (eg > 1.0 && eg > worstG)
                std::cout << "    df(" << ckt.unknownName(i) << ")/d" << ckt.unknownName(j) << ": analytic "
                          << G(i, j) << ", finite difference " << df[i] << '\n';
            if (ec > 1.0 && ec > worstC)
                std::cout << "    dq(" << ckt.unknownName(i) << ")/d" << ckt.unknownName(j) << ": analytic "
                          << C(i, j) << ", finite difference " << dq[i] << '\n';
            worstG = std::max(worstG, eg);
            worstC = std::max(worstC, ec);
        }
    }
    if (worstG > 1.0 || worstC > 1.0) {
        std::cout << "    at x =";
        for (int i = 0; i < n; ++i) std::cout << ' ' << ckt.unknownName(i) << '=' << x[i];
        std::cout << '\n';
    }
    checkTrue(worstG <= 1.0, what + ": df/dx agrees with finite differences");
    checkTrue(worstC <= 1.0, what + ": dq/dx agrees with finite differences");
}

void testDeviceModels() {
    // --- derivatives of all semiconductor models at scattered bias points ---
    const char* devices[] = {
        R"json({"type": "D", "name": "X", "nodes": ["a", "b"], "is": 1e-14, "n": 1.2, "rs": 2,
                "cj0": "5p", "tt": "10n", "bv": 6, "ibv": "1m"})json",
        R"json({"type": "npn", "name": "X", "nodes": ["a", "b", "c"], "is": 1e-15, "bf": 150, "br": 3,
                "vaf": 60, "var": 20, "ikf": "20m", "ikr": "5m", "ise": 1e-13, "ne": 1.6, "isc": 1e-12,
                "nc": 1.9, "rb": 30, "rc": 2, "re": 0.5, "cje": "10p", "cjc": "4p", "tf": "0.4n", "tr": "20n"})json",
        R"json({"type": "pnp", "name": "X", "nodes": ["a", "b", "c"], "is": 1e-15, "bf": 80, "vaf": 40,
                "ikf": "10m", "ise": 1e-13, "rb": 10, "cje": "8p", "cjc": "5p", "tf": "1n"})json",
        R"json({"type": "nmos", "name": "X", "nodes": ["a", "b", "c", "d"], "vto": 0.8, "kp": 1e-4,
                "gamma": 0.5, "lambda": 0.03, "w": "20u", "l": "1u", "tox": "10n", "cgso": "0.2n",
                "cgdo": "0.2n", "cbd": "10f", "cbs": "10f"})json",
        R"json({"type": "pmos", "name": "X", "nodes": ["a", "b", "c", "d"], "vto": -0.8, "kp": 4e-5,
                "gamma": 0.4, "lambda": 0.05, "w": "40u", "l": "1u", "tox": "10n", "cbd": "10f"})json",
        R"json({"type": "nmos", "name": "X", "nodes": ["a", "b", "c", "d"], "level": "ekv", "vto": 0.6,
                "kp": 2e-4, "gamma": 0.6, "lambda": 0.04, "theta": 0.2, "ucrit": 4e6, "w": "10u",
                "l": "0.5u", "tox": "8n", "cgso": "0.2n", "cbd": "10f"})json",
        R"json({"type": "pmos", "name": "X", "nodes": ["a", "b", "c", "d"], "level": "ekv", "vto": -0.7,
                "kp": 8e-5, "gamma": 0.5, "lambda": 0.05, "theta": 0.1, "ucrit": 8e6, "w": "20u",
                "l": "0.5u", "tox": "8n"})json",
        R"json({"type": "njf", "name": "X", "nodes": ["a", "b", "c"], "vto": -0.4, "beta": 1e-3,
                "lambda": 0.02, "cgs": "3p", "cgd": "2p"})json",
        R"json({"type": "pjf", "name": "X", "nodes": ["a", "b", "c"], "vto": -0.3, "beta": 5e-4,
                "lambda": 0.01, "cgs": "3p"})json",
    };
    for (const char* dev : devices) {
        const std::string text = std::string(R"json({"components": [
            {"type": "R", "nodes": ["a", "0"], "value": 1000}, {"type": "R", "nodes": ["b", "0"], "value": 1000},
            {"type": "R", "nodes": ["c", "0"], "value": 1000}, {"type": "R", "nodes": ["d", "0"], "value": 1000}, )json") +
                                 dev + "]}";
        Netlist nl = parseNetlistText(text);
        Circuit& ckt = *nl.circuit;
        const std::string kind = nlohmann::json::parse(dev)["type"].get<std::string>();
        const bool mos = kind == "nmos" || kind == "pmos";
        unsigned seed = 12345;
        for (int trial = 0; trial < 12; ++trial) {
            Vector x(ckt.size());
            for (int i = 0; i < ckt.size(); ++i) {
                seed = seed * 1664525u + 1013904223u;
                const double r = (seed >> 8) / 16777216.0;  // 0 .. 1
                // Junction voltages stay below about 0.75 V; MOS terminals cover all regions.
                x[i] = mos ? 4.0 * r : 0.75 * r;
            }
            // The bulk diodes of a MOSFET stay reverse biased (bulk below all
            // terminals of an n-channel, above those of a p-channel).
            if (mos) x[ckt.findNode("d")] = kind == "nmos" ? -2.0 * x[ckt.findNode("d")] / 4.0
                                                          : 4.0 + 2.0 * x[ckt.findNode("d")] / 4.0;
            checkJacobian(ckt, x, kind + " model, bias point " + std::to_string(trial));
        }
    }

    // --- Gummel-Poon: currents at a prescribed bias ---
    const auto bjtCurrents = [](const std::string& params, double vbe, double vce, double temp,
                                double& ic, double& ib) {
        Netlist nl = parseNetlistText(R"json({"options": {"temp": )json" + std::to_string(temp) +
                                      R"json(, "gmin": 1e-30}, "components": [
            {"type": "V", "name": "VB", "nodes": ["b", "0"], "dc": )json" + std::to_string(vbe) + R"json(},
            {"type": "V", "name": "VC", "nodes": ["c", "0"], "dc": )json" + std::to_string(vce) + R"json(},
            {"type": "npn", "name": "Q1", "nodes": ["c", "b", "0"], )json" + params + "}]}");
        const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
        ic = -probe(*nl.circuit, x, "i(VC)");
        ib = -probe(*nl.circuit, x, "i(VB)");
    };
    const double vt = 8.617333262e-5 * 300.15, is = 1e-15;
    double ic, ib;
    // Leakage: ib = if/bf + ise (exp(vbe/(ne vt)) - 1), ic = if
    bjtCurrents(R"json("is": 1e-15, "bf": 100, "ise": 1e-13, "ne": 2)json", 0.6, 2.0, 27.0, ic, ib);
    const double ifw = is * (std::exp(0.6 / vt) - 1.0);
    checkNear(ic / ifw, 1.0, 1e-6, "bjt: collector current without Early effect");
    checkNear(ib / (ifw / 100.0 + 1e-13 * (std::exp(0.6 / (2.0 * vt)) - 1.0)), 1.0, 1e-6,
              "bjt: base current with leakage");
    // High injection: ic = if / qb with qb = (1 + sqrt(1 + 4 if/ikf)) / 2
    bjtCurrents(R"json("is": 1e-15, "bf": 100, "ikf": 1e-3)json", 0.75, 2.0, 27.0, ic, ib);
    const double ifh = is * (std::exp(0.75 / vt) - 1.0);
    checkNear(ic / (ifh / (0.5 * (1.0 + std::sqrt(1.0 + 4.0 * ifh / 1e-3)))), 1.0, 1e-6,
              "bjt: high-injection roll-off of the collector current");
    checkTrue(ic / ib < 50.0, "bjt: current gain drops at high injection");
    // Early effect: ic = if * (1 + vcb/vaf) = if * (1 - vbc/vaf)
    bjtCurrents(R"json("is": 1e-15, "bf": 100, "vaf": 50)json", 0.6, 5.6, 27.0, ic, ib);
    checkNear(ic / (ifw * (1.0 + 5.0 / 50.0)), 1.0, 1e-6, "bjt: Early effect");
    // Temperature: is(T) = is r^xti exp((r - 1) eg / vt(T)); the current rises steeply
    bjtCurrents(R"json("is": 1e-15, "bf": 100)json", 0.6, 2.0, 77.0, ic, ib);
    const double r = 350.15 / 300.15, vtHot = 8.617333262e-5 * 350.15;
    const double isHot = is * std::pow(r, 3.0) * std::exp((r - 1.0) * 1.11 / vtHot);
    checkNear(ic / (isHot * (std::exp(0.6 / vtHot) - 1.0)), 1.0, 1e-6, "bjt: temperature dependence");

    // Emitter resistance inside the model acts like an external resistor.
    const auto vcWithRe = [](const char* model, const char* emitter) {
        Netlist nl = parseNetlistText(std::string(R"json({"components": [
            {"type": "V", "name": "VCC", "nodes": ["vcc", "0"], "dc": 10},
            {"type": "V", "name": "VB", "nodes": ["b", "0"], "dc": 1.5},
            {"type": "R", "name": "RC", "nodes": ["vcc", "c"], "value": "1k"},)json") +
                                      emitter + R"json({"type": "npn", "name": "Q1", "nodes": ["c", "b", "e"], )json" +
                                      model + "}]}");
        const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
        return probe(*nl.circuit, x, "v(c)");
    };
    const double vInternal = vcWithRe(R"json("is": 1e-15, "re": 200)json",
                                      R"json({"type": "R", "name": "RE", "nodes": ["e", "0"], "value": 1e-6},)json");
    const double vExternal = vcWithRe(R"json("is": 1e-15)json",
                                      R"json({"type": "R", "name": "RE", "nodes": ["e", "0"], "value": 200},)json");
    checkNear(vInternal, vExternal, 1e-6, "bjt: internal emitter resistance");
    checkTrue(vExternal > 5.0 && vExternal < 7.0, "bjt: degenerated stage is in the active region");

    // --- diode: forward voltage falls by about 2 mV/K at constant current ---
    const auto diodeVoltage = [](double temp) {
        Netlist nl = parseNetlistText(R"json({"options": {"temp": )json" + std::to_string(temp) +
                                      R"json(}, "components": [
            {"type": "I", "name": "I1", "nodes": ["0", "a"], "dc": "1m"},
            {"type": "D", "name": "D1", "nodes": ["a", "0"], "is": 1e-14}]})json");
        const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
        return probe(*nl.circuit, x, "v(a)");
    };
    const double tc = (diodeVoltage(37.0) - diodeVoltage(27.0)) / 10.0;
    checkTrue(tc < -1.5e-3 && tc > -2.5e-3, "diode: temperature coefficient of the forward voltage");
    checkNear(diodeVoltage(27.0), vt * std::log(1e-3 / 1e-14 + 1.0), 1e-6, "diode: nominal temperature");

    // --- MOSFET: gate capacitance in the three regions (cox W L = 34.53 fF) ---
    const double K = 3.9 * 8.8541878128e-12 / 10e-9 * 10e-6 * 1e-6;
    const auto gateCap = [&](double vg, double vd) {
        Netlist nl = parseNetlistText(R"json({"components": [
            {"type": "V", "name": "VG", "nodes": ["g", "0"], "ac": 1, "dc": )json" + std::to_string(vg) + R"json(},
            {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": )json" + std::to_string(vd) + R"json(},
            {"type": "nmos", "name": "M1", "nodes": ["d", "g", "0", "0"], "vto": 1, "kp": 1e-4,
             "w": "10u", "l": "1u", "tox": "10n"}]})json");
        Circuit& ckt = *nl.circuit;
        const Vector xop = solveOperatingPoint(ckt, nl.options);
        const Probe ig = parseProbe(ckt, "i(VG)");
        double c = 0.0;
        runAc(ckt, xop, {1e6}, [&](double f, const Eigen::VectorXcd& X) {
            c = std::abs(ig.phasor(X)) / (2.0 * kPi * f);
        });
        return c;
    };
    checkNear(gateCap(3.0, 5.0) / K, 2.0 / 3.0, 1e-6, "mosfet: gate capacitance in saturation is 2/3 cox");
    checkNear(gateCap(3.0, 0.0) / K, 1.0, 1e-6, "mosfet: gate capacitance at vds = 0 is cox");
    checkNear(gateCap(0.0, 5.0) / K, 1.0, 1e-6, "mosfet: gate capacitance below threshold is cox (to bulk)");

    // Mobility: the saturation current scales with T^-1.5.
    const auto drainCurrent = [](double temp) {
        Netlist nl = parseNetlistText(R"json({"options": {"temp": )json" + std::to_string(temp) +
                                      R"json(}, "components": [
            {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": 5},
            {"type": "V", "name": "VG", "nodes": ["g", "0"], "dc": 3},
            {"type": "nmos", "name": "M1", "nodes": ["d", "g", "0"], "vto": 1, "kp": 1e-4}]})json");
        const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
        return -probe(*nl.circuit, x, "i(VD)");
    };
    checkNear(drainCurrent(127.0) / drainCurrent(27.0), std::pow(400.15 / 300.15, -1.5), 1e-4,
              "mosfet: mobility falls with temperature");

    // --- operating point of a long chain of high-gain stages ---
    {
        std::string comps = R"json({"type": "V", "name": "VDD", "nodes": ["vdd", "0"], "dc": 3.3},
            {"type": "V", "name": "VIN", "nodes": ["s0", "0"], "dc": 0})json";
        const int stages = 60;
        for (int k = 0; k < stages; ++k) {
            const std::string in = "s" + std::to_string(k), out = "s" + std::to_string(k + 1);
            comps += R"json(, {"type": "pmos", "nodes": [")json" + out + "\", \"" + in +
                     R"json(", "vdd", "vdd"], "vto": -0.7, "kp": "40u", "lambda": 0.05, "w": "3u", "l": "0.35u"})json";
            comps += R"json(, {"type": "nmos", "nodes": [")json" + out + "\", \"" + in +
                     R"json(", "0", "0"], "vto": 0.7, "kp": "120u", "lambda": 0.05, "w": "1u", "l": "0.35u"})json";
        }
        Netlist chain = parseNetlistText("{\"components\": [" + comps + "]}");
        DcResult info;
        const Vector xc = solveOperatingPoint(*chain.circuit, chain.options, nullptr, &info);
        bool alternating = true;
        for (int k = 1; k <= stages; ++k) {
            const double v = probe(*chain.circuit, xc, "v(s" + std::to_string(k) + ")");
            alternating = alternating && std::abs(v - (k % 2 ? 3.3 : 0.0)) < 1e-3;
        }
        checkTrue(alternating, "operating point of a chain of 60 inverters");
        std::cout << "    inverter chain: operating point by " << info.strategy << " in "
                  << info.iterations << " iterations\n";
    }

    // --- models in SPICE syntax ---
    Netlist sp = parseNetlistText(R"json({
        "models": {
            "Q2N2222": ".model Q2N2222 NPN(Is=14.34f Xti=3 Eg=1.11 Vaf=74.03 Bf=255.9 Ne=1.307\n+ Ise=14.34f Ikf=.2847 Xtb=1.5 Br=6.092 Nc=2 Isc=0 Ikr=0 Rc=1 Cjc=7.306p Mjc=.3416\n+ Vjc=.75 Fc=.5 Cje=22.01p Mje=.377 Vje=.75 Tr=46.91n Tf=411.1p Itf=.6 Vtf=1.7 Xtf=3 Rb=10)",
            "D1N4148": ".MODEL D1N4148 D (IS=2.52n RS=.568 N=1.752 CJO=4p M=.4 TT=20n BV=100 IBV=100u)"
        },
        "components": [
            {"type": "V", "name": "VCC", "nodes": ["vcc", "0"], "dc": 12},
            {"type": "R", "name": "R1", "nodes": ["vcc", "b"], "value": "47k"},
            {"type": "R", "name": "R2", "nodes": ["b", "0"], "value": "10k"},
            {"type": "R", "name": "RC", "nodes": ["vcc", "c"], "value": "4.7k"},
            {"type": "R", "name": "RE", "nodes": ["e", "0"], "value": "1k"},
            {"type": "Q", "name": "Q1", "nodes": ["c", "b", "e"], "model": "Q2N2222"},
            {"type": "R", "name": "RD", "nodes": ["vcc", "k"], "value": "10k"},
            {"type": "D", "name": "D1", "nodes": ["k", "0"], "model": "D1N4148"}
        ]})json");
    const Vector xs = solveOperatingPoint(*sp.circuit, sp.options);
    const double ve = probe(*sp.circuit, xs, "v(e)");
    checkTrue(ve > 1.3 && ve < 1.6, "SPICE model: bias point of the 2N2222 stage");
    checkNear(probe(*sp.circuit, xs, "v(b)") - ve, 0.65, 0.05, "SPICE model: base-emitter voltage");
    checkNear(probe(*sp.circuit, xs, "v(k)"), 0.62, 0.05, "SPICE model: forward voltage of the 1N4148");
    checkTrue(throws([&] {
                  parseNetlistText(R"json({"models": {"m": ".model m JFET(beta=1)"},
                      "components": [{"type": "R", "nodes": ["a", "0"], "value": 1}]})json");
              }),
              "unsupported SPICE model type is rejected");
}

void testNoise() {
    const double kT = 1.380649e-23 * 300.15, q = 1.602176634e-19, vt = 8.617333262e-5 * 300.15;
    // Output noise density (squared) of a circuit at the given frequencies.
    const auto noise = [](Netlist& nl, const std::string& output, const char* input,
                          const std::vector<double>& freqs, std::vector<double>* gains = nullptr) {
        Circuit& ckt = *nl.circuit;
        const Vector xop = solveOperatingPoint(ckt, nl.options);
        const Probe out = parseProbe(ckt, output);
        const auto* src = input ? dynamic_cast<const IndependentSource*>(ckt.findDevice(input)) : nullptr;
        std::vector<NoiseSource> sources;
        std::vector<double> result;
        runNoise(ckt, xop, freqs, out.pos, out.neg, src, sources,
                 [&](double, double onoise2, double gain, const std::vector<double>&) {
                     result.push_back(onoise2);
                     if (gains) gains->push_back(gain);
                 });
        return result;
    };

    // Thermal noise of a resistor: 4 k T R; of two in parallel: 4 k T (R1 || R2).
    Netlist r1 = parseNetlistText(R"json({"components": [
        {"type": "R", "name": "R1", "nodes": ["a", "0"], "value": "1k"}]})json");
    checkNear(noise(r1, "v(a)", nullptr, {1e3})[0] / (4.0 * kT * 1e3), 1.0, 1e-9, "thermal noise of a resistor");
    Netlist r2 = parseNetlistText(R"json({"components": [
        {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1},
        {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": "1k"},
        {"type": "R", "name": "R2", "nodes": ["a", "0"], "value": "3k"}]})json");
    std::vector<double> gains;
    checkNear(noise(r2, "v(a)", "V1", {1e3}, &gains)[0] / (4.0 * kT * 750.0), 1.0, 1e-9,
              "thermal noise of a divider");
    checkNear(gains[0], 0.75, 1e-12, "noise analysis: gain from the input source");

    // RC low-pass: the noise integrated over all frequencies is k T / C.
    Netlist rc = parseNetlistText(R"json({"components": [
        {"type": "R", "name": "R1", "nodes": ["a", "0"], "value": "1k"},
        {"type": "C", "name": "C1", "nodes": ["a", "0"], "value": "1n"}]})json");
    const std::vector<double> freqs = frequencySweep("dec", 200, 1.0, 1e10);
    const std::vector<double> dens = noise(rc, "v(a)", nullptr, freqs);
    double integral = 0.0;
    for (size_t k = 1; k < freqs.size(); ++k)
        integral += 0.5 * (freqs[k] - freqs[k - 1]) * (dens[k] + dens[k - 1]);
    checkNear(integral / (kT / 1e-9), 1.0, 2e-3, "integrated noise of an RC low-pass is kT/C");

    // Shot noise of a diode: 2 q I into its small-signal resistance.
    Netlist dn = parseNetlistText(R"json({"options": {"gmin": 1e-30}, "components": [
        {"type": "I", "name": "I1", "nodes": ["0", "a"], "dc": "1m"},
        {"type": "D", "name": "D1", "nodes": ["a", "0"], "is": 1e-14}]})json");
    const double gd = (1e-3 + 1e-14) / vt;
    checkNear(noise(dn, "v(a)", nullptr, {1e3})[0] / (2.0 * q * 1e-3 / (gd * gd)), 1.0, 1e-6,
              "shot noise of a diode");

    // Common-emitter stage with base resistance: sum of the four contributions.
    Netlist ce = parseNetlistText(R"json({"options": {"gmin": 1e-30}, "components": [
        {"type": "V", "name": "VCC", "nodes": ["vcc", "0"], "dc": 10},
        {"type": "V", "name": "VB", "nodes": ["b", "0"], "dc": 0.65},
        {"type": "R", "name": "RC", "nodes": ["vcc", "c"], "value": "1k"},
        {"type": "npn", "name": "Q1", "nodes": ["c", "b", "0"], "is": 1e-15, "bf": 100, "rb": 100}]})json");
    const Vector xce = solveOperatingPoint(*ce.circuit, ce.options);
    const double ic = (10.0 - probe(*ce.circuit, xce, "v(c)")) / 1e3, ib = -probe(*ce.circuit, xce, "i(VB)");
    const double gm = ic / vt, rpi = vt / ib, rb = 100.0, RC = 1e3;
    const double av = gm * RC;                 // from the internal base to the collector
    const double div = rpi / (rpi + rb);       // base resistance against r_pi
    const double par = rb * rpi / (rb + rpi);
    const double expected = 2.0 * q * ic * RC * RC + 2.0 * q * ib * par * par * av * av +
                            4.0 * kT * rb * div * div * av * av + 4.0 * kT * RC;
    gains.clear();
    const double on = noise(ce, "v(c)", "VB", {1e3}, &gains)[0];
    checkNear(on / expected, 1.0, 1e-4, "noise of a common-emitter stage");
    checkNear(gains[0] / (av * div), 1.0, 1e-4, "gain of the common-emitter stage");
    std::cout << "    common-emitter stage: input-referred noise " << std::sqrt(on) / gains[0] * 1e9
              << " nV/sqrt(Hz)\n";
}

void testFourier() {
    // 0.5 + 2 cos(w t + 30 deg) + 0.2 cos(3 w t - 45 deg), f = 50 Hz, two periods.
    std::vector<double> t, y;
    const double w = 2.0 * kPi * 50.0;
    for (int k = 0; k <= 8000; ++k) {
        t.push_back(5e-6 * k);
        y.push_back(0.5 + 2.0 * std::cos(w * t.back() + kPi / 6.0) + 0.2 * std::cos(3.0 * w * t.back() - kPi / 4.0));
    }
    std::vector<std::complex<double>> amp;
    checkTrue(fourier(t, y, 50.0, 5, 0, 0.0, amp), "fourier analysis over whole periods");
    checkNear(amp[0].real(), 0.5, 1e-6, "fourier: mean value");
    checkNear(std::abs(amp[1]), 2.0, 1e-5, "fourier: fundamental amplitude");
    checkNear(std::arg(amp[1]), kPi / 6.0, 1e-5, "fourier: fundamental phase");
    checkNear(std::abs(amp[2]), 0.0, 1e-5, "fourier: absent harmonic");
    checkNear(std::abs(amp[3]), 0.2, 1e-5, "fourier: third harmonic amplitude");
    checkNear(std::arg(amp[3]), -kPi / 4.0, 1e-4, "fourier: third harmonic phase");
    checkNear(totalHarmonicDistortion(amp), 0.1, 1e-5, "total harmonic distortion");
    checkTrue(!fourier(t, y, 10.0, 3, 0, 0.0, amp), "fourier: fewer samples than one period");

    // Square wave: odd harmonics 4/(pi k); thd of the first 15 harmonics is 0.45.
    Netlist nl = parseNetlistText(R"json({
        "components": [
            {"type": "V", "name": "V1", "nodes": ["a", "0"],
             "wave": {"type": "pulse", "v1": -1, "v2": 1, "rise": "1u", "fall": "1u",
                      "width": "499u", "period": "1m"}},
            {"type": "R", "name": "R1", "nodes": ["a", "0"], "value": "1k"}
        ],
        "analyses": [
            {"type": "tran", "tstop": "4m", "tstep": "5u",
             "fourier": {"freq": "1k", "harmonics": 5},
             "measure": {"thd": {"thd": "v(a)", "fundamental": "1k"},
                         "h3": {"harmonic": "v(a)", "fundamental": "1k", "n": 3}}}
        ],
        "outputs": ["v(a)"]
    })json");
    RunConfig cfg;
    cfg.outDir = (std::filesystem::temp_directory_path() / "esim_tests").string();
    cfg.stem = "square";
    nlohmann::json summary = nlohmann::json::array();
    cfg.summary = &summary;
    std::ostringstream log;
    runAnalyses(nl, cfg, log);
    checkNear(summary[0]["measures"]["h3"].get<double>(), 4.0 / (3.0 * kPi), 2e-3, "square wave: third harmonic");
    checkNear(summary[0]["measures"]["thd"].get<double>(), 0.45, 5e-3, "square wave: distortion");
    checkNear(summary[0]["fourier"]["fundamental"]["v(a)"].get<double>(), 4.0 / kPi, 2e-3,
              "fourier block: fundamental of a square wave");
    std::ifstream in(std::filesystem::path(cfg.outDir) / "square.tran.fourier.csv");
    std::string header;
    std::getline(in, header);
    checkTrue(header == "harmonic,freq,mag(v(a)),phase(v(a))", "fourier block: table");
}

void testParameterStudies() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "esim_tests";
    const auto runAll = [&](const std::string& text, const std::string& stem) {
        Netlist nl = parseNetlistText(text);
        RunConfig cfg;
        cfg.outDir = dir.string();
        cfg.stem = stem;
        nlohmann::json summary = nlohmann::json::array();
        cfg.summary = &summary;
        std::ostringstream log;
        runAnalyses(nl, cfg, log);
        return summary;
    };
    const auto lines = [&](const std::string& name) {
        std::ifstream in(dir / name);
        std::vector<std::string> out;
        for (std::string line; std::getline(in, line);) out.push_back(line);
        return out;
    };

    // --- two nested sweeps: the first parameter is the outer one ---
    runAll(R"json({
        "parameters": {"R": "1k", "V": 1},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": "V"},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "R"},
            {"type": "R", "name": "R2", "nodes": ["out", "0"], "value": "1k"}
        ],
        "analyses": [{"type": "op", "param_sweep": [{"param": "R", "values": ["1k", "3k"]},
                                                    {"param": "V", "values": [1, 2]}]}],
        "outputs": ["v(out)"]
    })json", "nested");
    const auto nested = lines("nested.op.csv");
    checkTrue(nested.size() == 5 && nested[0] == "R,V,v(out)", "nested sweep: table with two parameter columns");
    if (nested.size() == 5) {
        checkTrue(nested[1] == "1000,1,0.5" && nested[2] == "1000,2,1", "nested sweep: inner parameter runs fastest");
        checkTrue(nested[3] == "3000,1,0.25" && nested[4] == "3000,2,0.5", "nested sweep: outer parameter");
    }

    // --- Monte Carlo: divider with a resistor of +-10 % (uniform) ---
    const char* mc = R"json({
        "parameters": {"R": "1k"},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "R"},
            {"type": "R", "name": "R2", "nodes": ["out", "0"], "value": "1k"}
        ],
        "analyses": [{"type": "dc", "source": "V1", "start": 1, "stop": 1, "step": 1,
                      "monte_carlo": {"runs": 2000, "seed": 7, "vary": {"R": {"tol": 0.1}}},
                      "measure": {"vout": {"final": "v(out)"}}}]
    })json";
    const nlohmann::json s1 = runAll(mc, "mc");
    const nlohmann::json stat = s1[0]["statistics"]["vout"];
    // v = 1/(1 + R/1k): sensitivity -0.25 per unit of R/1k, standard deviation of R/1k is 0.1/sqrt(3)
    checkNear(stat["mean"].get<double>(), 0.5, 3e-3, "monte carlo: mean");
    checkNear(stat["std"].get<double>() / (0.25 * 0.1 / std::sqrt(3.0)), 1.0, 0.05, "monte carlo: standard deviation");
    checkTrue(stat["min"].get<double>() > 1.0 / 2.1 - 1e-9 && stat["max"].get<double>() < 1.0 / 1.9 + 1e-9,
              "monte carlo: values stay inside the tolerance");
    checkTrue(stat["count"].get<int>() == 2000 && lines("mc.dc.measure.csv").size() == 2001,
              "monte carlo: one row per run");
    checkTrue(lines("mc.dc.measure.csv")[0] == "run,R,vout", "monte carlo: table with the drawn parameters");
    checkTrue(runAll(mc, "mc")[0]["statistics"] == s1[0]["statistics"], "monte carlo: a seed reproduces the result");

    // --- fit: recover the parameters of a diode from its characteristic ---
    const double vt = 8.617333262e-5 * 300.15, isTrue = 2e-12, nTrue = 1.7;
    std::string xs, ys;
    for (double v = 0.40; v < 0.701; v += 0.05) {
        xs += (xs.empty() ? "" : ", ") + std::to_string(v);
        std::ostringstream os;
        os << std::setprecision(12) << isTrue * (std::exp(v / (nTrue * vt)) - 1.0);
        ys += (ys.empty() ? "" : ", ") + os.str();
    }
    const nlohmann::json fit = runAll(R"json({
        "parameters": {"is0": 1e-13, "n0": 1.2},
        "options": {"gmin": 1e-30},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 0.5},
            {"type": "D", "name": "D1", "nodes": ["a", "0"], "is": "is0", "n": "n0"}
        ],
        "analyses": [{"type": "fit",
            "analysis": {"type": "dc", "source": "V1", "start": 0.35, "stop": 0.75, "step": 0.01},
            "output": "-i(V1)",
            "data": {"x": [)json" + xs + R"json(], "y": [)json" + ys + R"json(]},
            "parameters": {"is0": {"start": 1e-13, "min": 1e-16, "max": 1e-9, "log": true},
                           "n0": {"start": 1.2, "min": 1, "max": 3}},
            "residual": "log"}]
    })json", "fit");
    checkNear(fit[0]["parameters"]["is0"].get<double>() / isTrue, 1.0, 1e-3, "fit: saturation current recovered");
    checkNear(fit[0]["parameters"]["n0"].get<double>(), nTrue, 1e-3, "fit: emission coefficient recovered");
    checkTrue(fit[0]["rms_residual"].get<double>() < 1e-4, "fit: residual is small");
    checkTrue(lines("fit.fit.csv").size() == 8 && lines("fit.fit.csv")[0] == "x,data,model", "fit: table of data and model");

    // --- fit of a transient: time constant of an RC step response ---
    std::string pts;
    for (int k = 1; k <= 8; ++k) {
        const double t = 0.5e-3 * k;
        std::ostringstream os;
        os << std::setprecision(12) << "[" << t << ", " << 1.0 - std::exp(-t / 2e-3) << "]";
        pts += (pts.empty() ? "" : ", ") + os.str();
    }
    const nlohmann::json fr = runAll(R"json({
        "parameters": {"C": "0.5u"},
        "options": {"reltol": 1e-7},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "C", "ic": 0}
        ],
        "analyses": [{"type": "fit",
            "analysis": {"type": "tran", "tstop": "5m", "uic": true},
            "output": "v(out)",
            "data": {"points": [)json" + pts + R"json(]},
            "parameters": {"C": {"min": "10n", "max": "100u", "log": true}}}]
    })json", "fitrc");
    checkNear(fr[0]["parameters"]["C"].get<double>() / 2e-6, 1.0, 1e-3, "fit of a transient: capacitance recovered");

    checkTrue(throws([&] {
                  runAll(R"json({"parameters": {"R": 1}, "components": [{"type": "R", "nodes": ["a", "0"], "value": "R"}],
                      "analyses": [{"type": "fit", "analysis": {"type": "op"}, "output": "v(a)",
                                    "data": {"x": [0, 1], "y": [0, 1]}, "parameters": {"R": {}}}]})json", "bad");
              }),
              "fit with an unsuitable analysis is rejected");
}

void testMoreDevices() {
    // --- JFET: saturation current idss = beta vto^2 at vgs = 0, pinch-off below vto ---
    const auto drain = [](double vgs, double vds, const char* type) {
        Netlist nl = parseNetlistText(R"json({"options": {"gmin": 1e-30}, "components": [
            {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": )json" + std::to_string(vds) + R"json(},
            {"type": "V", "name": "VG", "nodes": ["g", "0"], "dc": )json" + std::to_string(vgs) + R"json(},
            {"type": ")json" + type + R"json(", "name": "J1", "nodes": ["d", "g", "0"], "vto": -2, "beta": 1e-3,
             "lambda": 0.01}]})json");
        const Vector x = solveOperatingPoint(*nl.circuit, nl.options);
        return -probe(*nl.circuit, x, "i(VD)");
    };
    checkNear(drain(0.0, 5.0, "njf") / (1e-3 * 4.0 * 1.05), 1.0, 1e-6, "jfet: saturation current at vgs = 0");
    checkNear(drain(-1.0, 5.0, "njf") / (1e-3 * 1.0 * 1.05), 1.0, 1e-6, "jfet: square law");
    checkNear(drain(-1.0, 0.5, "njf") / (1e-3 * 0.5 * 1.5 * 1.005), 1.0, 1e-6, "jfet: linear region");
    checkTrue(std::abs(drain(-3.0, 5.0, "njf")) < 1e-10, "jfet: pinched off below vto");
    checkNear(drain(1.0, -5.0, "pjf"), -drain(-1.0, 5.0, "njf"), 1e-12, "p-channel jfet mirrors the n-channel");

    // --- MOSFET, EKV model ---
    // Drain current for terminal voltages against ground (bulk at ground).
    const auto mos = [](const std::string& params, double vg, double vd, double vs, Circuit** keep = nullptr,
                        Vector* xk = nullptr) {
        static std::unique_ptr<Netlist> held;
        held = std::make_unique<Netlist>(parseNetlistText(R"json({"options": {"gmin": 1e-30}, "components": [
            {"type": "V", "name": "VG", "nodes": ["g", "0"], "dc": )json" + std::to_string(vg) + R"json(},
            {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": )json" + std::to_string(vd) + R"json(},
            {"type": "V", "name": "VS", "nodes": ["s", "0"], "dc": )json" + std::to_string(vs) + R"json(},
            {"type": "nmos", "name": "M1", "nodes": ["d", "g", "s", "0"], "is": 1e-30, )json" + params + "}]}"));
        const Vector x = solveOperatingPoint(*held->circuit, held->options);
        if (keep) *keep = held->circuit.get();
        if (xk) *xk = x;
        return -probe(*held->circuit, x, "i(VD)");
    };
    const double vt = 8.617333262e-5 * 300.15;
    const std::string ekv = R"json("level": "ekv", "vto": 0.7, "kp": 1e-4, "w": "10u", "l": "1u")json";
    const std::string lvl1 = R"json("vto": 0.7, "kp": 1e-4, "w": "10u", "l": "1u")json";
    // Far above threshold the square law of level 1, exactly.
    checkNear(mos(ekv, 2.7, 5.0, 0.0) / mos(lvl1, 2.7, 5.0, 0.0), 1.0, 1e-4, "ekv: square law in strong inversion");
    checkNear(mos(ekv, 2.7, 0.5, 0.0) / mos(lvl1, 2.7, 0.5, 0.0), 1.0, 1e-4, "ekv: linear region");
    // Below threshold an exponential with vt ln(10) per decade (n = 1 without body effect).
    checkNear(mos(ekv, 0.4, 2.0, 0.0) / mos(ekv, 0.3, 2.0, 0.0), std::exp(0.1 / vt), 1e-2 * std::exp(0.1 / vt),
              "ekv: subthreshold slope");
    checkTrue(mos(lvl1, 0.4, 2.0, 0.0) < 1e-20 && mos(ekv, 0.4, 2.0, 0.0) > 1e-11, "ekv: current below threshold");
    // With body effect the slope factor n = 1 + gamma / (2 sqrt(vp + phi + 4 vt)), vp ~ -phi/... near 0.
    const std::string body = ekv + R"json(, "gamma": 0.6, "phi": 0.7)json";
    const double slope = std::log(mos(body, 0.35, 2.0, 0.0) / mos(body, 0.25, 2.0, 0.0)) / 0.1;  // 1/(n vt)
    checkTrue(1.0 / (slope * vt) > 1.2 && 1.0 / (slope * vt) < 1.5, "ekv: body effect flattens the subthreshold slope");
    // Symmetric in drain and source, also for a lifted source.
    checkNear(mos(ekv + R"json(, "lambda": 0.05, "theta": 0.1, "ucrit": 3e6)json", 2.0, 0.3, 1.1),
              -mos(ekv + R"json(, "lambda": 0.05, "theta": 0.1, "ucrit": 3e6)json", 2.0, 1.1, 0.3), 1e-12,
              "ekv: drain and source are interchangeable");
    // Velocity saturation and mobility reduction in strong-inversion saturation.
    const double id0 = mos(ekv, 2.7, 5.0, 0.0);
    checkNear(mos(ekv + R"json(, "ucrit": 4e6)json", 2.7, 5.0, 0.0) / id0, 1.0 / (1.0 + 2.0 / 4.0), 1e-3,
              "ekv: velocity saturation");
    checkNear(mos(ekv + R"json(, "theta": 0.2)json", 2.7, 5.0, 0.0) / id0, 1.0 / (1.0 + 0.2 * 1.0), 1e-4,
              "ekv: mobility reduction");
    // Charges: they add up to zero, and the gate sees the whole oxide in strong inversion.
    Circuit* mc = nullptr;
    Vector xm;
    mos(ekv + R"json(, "gamma": 0.5, "tox": "10n")json", 3.0, 0.0, 0.0, &mc, &xm);
    Evaluation em;
    mc->evaluate(0.0, xm, LimitMode::Off, em);
    double qsum = 0.0, qabs = 0.0, cgg = 0.0;
    for (const char* node : {"g", "d", "s"}) {
        qsum += em.q[mc->findNode(node)];
        qabs += std::abs(em.q[mc->findNode(node)]);
    }
    for (const Triplet& t : em.C)
        if (t.row() == mc->findNode("g") && t.col() == mc->findNode("g")) cgg += t.value();
    const double cox = 3.9 * 8.8541878128e-12 / 10e-9 * 10e-6 * 1e-6;
    // The bulk is the ground node here: its charge is what is missing from the sum.
    checkTrue(qabs > 1e-14 && std::abs(qsum) < qabs, "ekv: charges of gate, drain and source nearly cancel");
    checkNear(cgg / cox, 1.0, 0.03, "ekv: gate capacitance in strong inversion is the oxide capacitance");
    mos(ekv + R"json(, "gamma": 0.5, "tox": "10n")json", 0.2, 0.0, 0.0, &mc, &xm);
    mc->evaluate(0.0, xm, LimitMode::Off, em);
    cgg = 0.0;
    for (const Triplet& t : em.C)
        if (t.row() == mc->findNode("g") && t.col() == mc->findNode("g")) cgg += t.value();
    checkTrue(cgg / cox > 0.2 && cgg / cox < 0.6, "ekv: gate capacitance dips in depletion");

    // --- nonlinear inductor from a flux expression ---
    // Linear flux: identical to an inductor. RL step response i = (1 - exp(-t R/L)) V/R.
    Netlist lin = parseNetlistText(R"json({
        "options": {"reltol": 1e-7},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "a"], "value": 10},
            {"type": "B", "name": "LB", "nodes": ["a", "0"], "phi": "1m * i(LB)"}
        ]})json");
    const Trace tl = transient(lin, 5e-4, IntegrationMethod::Bdf, true);
    double maxErr = 0.0;
    for (size_t k = 0; k < tl.t.size(); ++k)
        maxErr = std::max(maxErr, std::abs(probe(*lin.circuit, tl.x[k], "i(LB)") - 0.1 * (1.0 - std::exp(-tl.t[k] * 1e4))));
    checkNear(maxErr, 0.0, 1e-6, "flux element with linear flux behaves like an inductor");

    // Saturating core at a constant voltage: flux = V t, so i = Isat atanh(V t / (L Isat)).
    Netlist sat = parseNetlistText(R"json({
        "parameters": {"L0": "1m", "Isat": 2},
        "options": {"reltol": 1e-7},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 1},
            {"type": "B", "name": "LB", "nodes": ["a", "0"], "phi": "L0*Isat*tanh(i(LB)/Isat)"}
        ]})json");
    const Trace ts = transient(sat, 1.6e-3, IntegrationMethod::Bdf, true);
    maxErr = 0.0;
    for (size_t k = 0; k < ts.t.size(); ++k)
        maxErr = std::max(maxErr, std::abs(probe(*sat.circuit, ts.x[k], "i(LB)") - 2.0 * std::atanh(ts.t[k] / 2e-3)));
    checkNear(maxErr, 0.0, 1e-4, "saturating inductor: current follows the flux");
    checkTrue(probe(*sat.circuit, ts.x.back(), "i(LB)") > 2.0, "saturating inductor: current exceeds Isat");

    // --- user-defined functions ---
    Netlist fn = parseNetlistText(R"json({
        "parameters": {"lim": 2},
        "functions": {
            "clip(x, a)": "max(-a, min(a, x))",
            "clip2(x)": "clip(x, lim) * 2",
            "hyp(a, b)": "sqrt(a^2 + b^2)"
        },
        "variables": {"y1": {"guess": 0}, "y2": {"guess": 0}, "y3": {"guess": 0}},
        "equations": ["y1 = clip(5, 1.5)", "y2 = clip2(-7)", "y3 = hyp(3, y1 + 2.5)"]
    })json");
    const Vector xf = solveOperatingPoint(*fn.circuit, fn.options);
    checkNear(probe(*fn.circuit, xf, "y1"), 1.5, 1e-12, "user function");
    checkNear(probe(*fn.circuit, xf, "y2"), -4.0, 1e-12, "user function calling another one, with a parameter");
    checkNear(probe(*fn.circuit, xf, "y3"), 5.0, 1e-9, "user function of an unknown");
    checkNear(probe(*fn.circuit, xf, "clip(y3, 1) + hyp(y1, 2)"), 1.0 + 2.5, 1e-9, "user function in an output");

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    checkTrue(fails(R"json({"functions": {"f(x)": "f(x) + 1"}, "variables": {"y": {"guess": 0}},
                        "equations": ["y = f(1)"]})json"),
              "endless recursion of a function is rejected");
    checkTrue(fails(R"json({"functions": {"f(x)": "x"}, "variables": {"y": {"guess": 0}},
                        "equations": ["y = f(1, 2)"]})json"),
              "wrong number of arguments is rejected");
    checkTrue(fails(R"json({"functions": {"sin(x)": "x"}, "variables": {"y": {"guess": 0}},
                        "equations": ["y = 1"]})json"),
              "redefining a built-in function is rejected");
}

void testSteadyStateAndSensitivity() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "esim_tests";
    const auto runAll = [&](const std::string& text, const std::string& stem) {
        Netlist nl = parseNetlistText(text);
        RunConfig cfg;
        cfg.outDir = dir.string();
        cfg.stem = stem;
        nlohmann::json summary = nlohmann::json::array();
        cfg.summary = &summary;
        std::ostringstream log;
        runAnalyses(nl, cfg, log);
        return summary;
    };

    // --- periodic steady state ---
    // RC low-pass with a time constant of 100 periods, started far away. A linear
    // circuit with one state is solved by a single extrapolation.
    nlohmann::json s = runAll(R"json({
        "options": {"reltol": 1e-6},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"],
             "wave": {"type": "sin", "offset": 2, "amplitude": 1, "freq": "1k"}},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "100u", "ic": 0}
        ],
        "analyses": [{"type": "pss", "period": "1m", "uic": true, "periods": 2,
                      "measure": {"mean": {"avg": "v(out)"}, "pp": {"pp": "v(out)"},
                                  "end": {"final": "time"}}}],
        "outputs": ["v(out)", "time"]})json", "pss_rc");
    checkNear(s[0]["measures"]["mean"].get<double>(), 2.0, 5e-4, "pss: mean of the RC low-pass");
    checkNear(s[0]["measures"]["pp"].get<double>(), 2.0 / std::sqrt(1.0 + std::pow(2.0 * kPi * 100.0, 2)),
              1e-5, "pss: ripple of the RC low-pass");
    checkNear(s[0]["measures"]["end"].get<double>(), 2e-3, 1e-12, "pss: the requested number of periods is written");
    checkTrue(s[0]["periods_simulated"].get<int>() <= 5, "pss: a linear circuit needs a handful of periods");

    // Rectifier that charges its capacitor slowly: compared with a long transient run.
    s = runAll(R"json({
        "options": {"reltol": 1e-6},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "wave": {"type": "sin", "amplitude": 10, "freq": 50}},
            {"type": "R", "name": "RS", "nodes": ["in", "a"], "value": 100},
            {"type": "D", "name": "D1", "nodes": ["a", "out"], "is": 1e-12},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "1000u"},
            {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": "1k"}
        ],
        "analyses": [
            {"type": "pss", "period": "20m", "measure": {"mean": {"avg": "v(out)"}, "pp": {"pp": "v(out)"}}},
            {"type": "tran", "tstop": 6, "save": false,
             "measure": {"mean": {"avg": "v(out)", "from": 5.98}, "pp": {"pp": "v(out)", "from": 5.98}}}
        ],
        "outputs": ["v(out)"]})json", "pss_rectifier");
    checkNear(s[0]["measures"]["mean"].get<double>() / s[1]["measures"]["mean"].get<double>(), 1.0, 5e-4,
              "pss: rectifier, mean as after 300 periods");
    checkNear(s[0]["measures"]["pp"].get<double>() / s[1]["measures"]["pp"].get<double>(), 1.0, 5e-3,
              "pss: rectifier, ripple as after 300 periods");
    checkTrue(s[0]["periods_simulated"].get<int>() <= 12, "pss: rectifier needs few periods");

    // Switched converter: two states, events in every period.
    s = runAll(R"json({
        "parameters": {"T": "10u"},
        "components": [
            {"type": "V", "name": "VIN", "nodes": ["in", "0"], "dc": 12},
            {"type": "V", "name": "PWM", "nodes": ["gate", "0"],
             "wave": {"type": "pulse", "v1": 0, "v2": 1, "rise": "T/1000", "fall": "T/1000",
                      "width": "0.5*T", "period": "T"}},
            {"type": "S", "name": "S1", "nodes": ["in", "sw", "gate", "0"], "vt": 0.5, "ron": "10m"},
            {"type": "ideal_diode", "name": "D1", "nodes": ["0", "sw"], "vf": 0.4, "ron": "10m"},
            {"type": "L", "name": "L1", "nodes": ["sw", "out"], "value": "100u"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "100u"},
            {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 5}
        ],
        "analyses": [
            {"type": "pss", "period": "T",
             "measure": {"vout": {"avg": "v(out)"}, "ripple": {"pp": "i(L1)"}}},
            {"type": "tran", "tstop": "20m", "save": false,
             "measure": {"vout": {"avg": "v(out)", "from": "20m - T"}, "ripple": {"pp": "i(L1)", "from": "20m - T"}}}
        ],
        "outputs": ["v(out)", "i(L1)"]})json", "pss_buck");
    checkNear(s[0]["measures"]["vout"].get<double>(), s[1]["measures"]["vout"].get<double>(), 1e-3,
              "pss: converter output as after 2000 periods");
    checkNear(s[0]["measures"]["ripple"].get<double>() / s[1]["measures"]["ripple"].get<double>(), 1.0, 1e-2,
              "pss: converter current ripple");
    checkTrue(s[0]["periods_simulated"].get<int>() <= 12, "pss: converter needs few periods");

    checkTrue(throws([&] {
                  runAll(R"json({"components": [
                      {"type": "V", "name": "V1", "nodes": ["a", "0"], "wave": {"type": "sin", "amplitude": 1, "freq": 1370}},
                      {"type": "R", "name": "R1", "nodes": ["a", "b"], "value": "1k"},
                      {"type": "C", "name": "C1", "nodes": ["b", "0"], "value": "1u"}],
                      "analyses": [{"type": "pss", "period": "1m"}]})json", "pss_bad");
              }),
              "pss: a wrong period is reported");

    // --- sensitivities ---
    // Divider: v = V R2/(R1+R2), i(V1) = -V/(R1+R2); corner frequency 1/(2 pi (R1||R2) C).
    s = runAll(R"json({
        "parameters": {"R1": "1k", "R2": "3k", "V": 10, "C": "100n"},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": "V", "ac": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "R1"},
            {"type": "R", "name": "R2", "nodes": ["out", "0"], "value": "R2"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "C"}
        ],
        "analyses": [
            {"type": "sens", "analysis": {"type": "op"}},
            {"type": "sens", "parameters": ["R1", "R2", "C"], "delta": 0.02,
             "analysis": {"type": "ac", "sweep": "dec", "points": 200, "fstart": 1, "fstop": "100k",
                          "measure": {"f3db": {"f3db": "v(out)"}}}}
        ],
        "outputs": ["v(out)", "i(V1)"]})json", "sens");
    const nlohmann::json& n0 = s[0]["normalized"];
    checkNear(n0["R1"]["v(out)"].get<double>(), -0.25, 1e-6, "sens: divider, R1");
    checkNear(n0["R2"]["v(out)"].get<double>(), 0.25, 1e-6, "sens: divider, R2");
    checkNear(n0["V"]["v(out)"].get<double>(), 1.0, 1e-9, "sens: divider, source");
    checkNear(n0["C"]["v(out)"].get<double>(), 0.0, 1e-12, "sens: divider, capacitor has no influence at DC");
    checkNear(n0["R2"]["i(V1)"].get<double>(), -0.75, 1e-6, "sens: source current");
    checkNear(s[0]["absolute"]["R2"]["v(out)"].get<double>(), 10.0 * 1e3 / 16e6, 1e-9, "sens: absolute value");
    checkNear(s[0]["nominal"]["v(out)"].get<double>(), 7.5, 1e-9, "sens: nominal value");
    const nlohmann::json& n1 = s[1]["normalized"];
    checkNear(n1["R1"]["f3db"].get<double>(), -0.75, 3e-3, "sens: corner frequency, R1");
    checkNear(n1["R2"]["f3db"].get<double>(), -0.25, 3e-3, "sens: corner frequency, R2");
    checkNear(n1["C"]["f3db"].get<double>(), -1.0, 3e-3, "sens: corner frequency, C");
    checkTrue(!n1.contains("V"), "sens: only the listed parameters");
    checkTrue(throws([&] {
                  runAll(R"json({"parameters": {"R": 1}, "components": [
                      {"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 1},
                      {"type": "R", "name": "R1", "nodes": ["a", "0"], "value": "R"}],
                      "analyses": [{"type": "sens", "analysis": {"type": "tran", "tstop": 1}}]})json", "sens_bad");
              }),
              "sens: an analysis without measurements is rejected");
}

void testDelaysAndLines() {
    // --- transmission line ---
    // Step into a matched line: half the source voltage enters at once and
    // arrives at the end after the delay. Nothing is reflected.
    const auto line = [](const std::string& load) {
        return parseNetlistText(R"json({
            "options": {"reltol": 1e-5},
            "components": [
                {"type": "V", "name": "V1", "nodes": ["src", "0"],
                 "wave": {"type": "pulse", "v1": 0, "v2": 1, "delay": "1n", "rise": "0.1n"}},
                {"type": "R", "name": "RS", "nodes": ["src", "in"], "value": 50},
                {"type": "T", "name": "T1", "nodes": ["in", "0", "out", "0"], "z0": 50, "td": "5n"},
                {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": )json" + load + "}]}");
    };
    const auto at = [](Netlist& nl, const Trace& tr, const std::string& what, double time) {
        for (size_t k = 1; k < tr.t.size(); ++k)
            if (tr.t[k] >= time) {
                const double a = probe(*nl.circuit, tr.x[k - 1], what), b = probe(*nl.circuit, tr.x[k], what);
                return a + (b - a) * (time - tr.t[k - 1]) / (tr.t[k] - tr.t[k - 1]);
            }
        return probe(*nl.circuit, tr.x.back(), what);
    };
    Netlist matched = line("50");
    const Trace tm = transient(matched, 30e-9, IntegrationMethod::Bdf, false);
    checkNear(at(matched, tm, "v(in)", 3e-9), 0.5, 1e-6, "line: half the voltage enters a matched line");
    checkNear(at(matched, tm, "v(out)", 5e-9), 0.0, 1e-9, "line: nothing arrives before the delay");
    checkNear(at(matched, tm, "v(out)", 8e-9), 0.5, 1e-6, "line: the step arrives after the delay");
    checkNear(at(matched, tm, "v(in)", 25e-9), 0.5, 1e-6, "line: no reflection from a matched load");
    checkNear(at(matched, tm, "i(T1)", 25e-9), 0.01, 1e-8, "line: current into the line");

    // Open end: the wave is reflected with the same sign and doubles the
    // voltage at the end; back at the (matched) source it completes the step.
    Netlist open = line("1e12");
    const Trace to = transient(open, 30e-9, IntegrationMethod::Bdf, false);
    checkNear(at(open, to, "v(out)", 8e-9), 1.0, 1e-6, "line: voltage doubles at an open end");
    checkNear(at(open, to, "v(in)", 8e-9), 0.5, 1e-6, "line: the reflection has not come back yet");
    checkNear(at(open, to, "v(in)", 14e-9), 1.0, 1e-6, "line: reflection back at the source after 2 td");

    // Mismatched at both ends: the end voltage goes through the bounce diagram.
    // Source 10 ohm, load 200 ohm on 50 ohm: v = a (1 + gl) sum (gs gl)^k.
    Netlist bounce = parseNetlistText(R"json({
        "options": {"reltol": 1e-5},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["src", "0"],
             "wave": {"type": "pulse", "v1": 0, "v2": 1, "delay": "1n", "rise": "0.1n"}},
            {"type": "R", "name": "RS", "nodes": ["src", "in"], "value": 10},
            {"type": "T", "name": "T1", "nodes": ["in", "0", "out", "0"], "z0": 50, "td": "5n"},
            {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 200}]})json");
    const Trace tb = transient(bounce, 40e-9, IntegrationMethod::Bdf, false);
    const double a0 = 50.0 / 60.0, gl = 150.0 / 250.0, gs = -40.0 / 60.0;
    checkNear(at(bounce, tb, "v(out)", 9e-9), a0 * (1 + gl), 1e-6, "line: first arrival at a mismatched end");
    checkNear(at(bounce, tb, "v(out)", 19e-9), a0 * (1 + gl) * (1 + gs * gl), 1e-6, "line: second arrival");
    checkNear(at(bounce, tb, "v(out)", 29e-9), a0 * (1 + gl) * (1 + gs * gl + gs * gs * gl * gl), 1e-6,
              "line: third arrival");
    // At DC the line is a wire.
    Netlist dc = parseNetlistText(R"json({"components": [
        {"type": "V", "name": "V1", "nodes": ["src", "0"], "dc": 2.1},
        {"type": "R", "name": "RS", "nodes": ["src", "in"], "value": 10},
        {"type": "T", "name": "T1", "nodes": ["in", "0", "out", "0"], "z0": 50, "td": "5n"},
        {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 200}]})json");
    const Vector xdc = solveOperatingPoint(*dc.circuit, dc.options);
    checkNear(probe(*dc.circuit, xdc, "v(out)"), 2.0, 1e-9, "line: a wire at DC");
    checkNear(probe(*dc.circuit, xdc, "v(in)"), 2.0, 1e-9, "line: a wire at DC, input");

    // AC: a quarter-wave line transforms the load to z0^2 / RL.
    Netlist qw = parseNetlistText(R"json({"components": [
        {"type": "V", "name": "V1", "nodes": ["src", "0"], "ac": 1},
        {"type": "R", "name": "RS", "nodes": ["src", "in"], "value": 25},
        {"type": "T", "name": "T1", "nodes": ["in", "0", "out", "0"], "z0": 50, "f": "100meg", "nl": 0.25},
        {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 100}]})json");
    const Vector xq = solveOperatingPoint(*qw.circuit, qw.options);
    const Probe pin = parseProbe(*qw.circuit, "v(in)"), pout = parseProbe(*qw.circuit, "v(out)");
    runAc(*qw.circuit, xq, {100e6, 200e6, 50e6}, [&](double f, const Eigen::VectorXcd& x) {
        const std::complex<double> vin = pin.phasor(x), vout = pout.phasor(x);
        if (f == 100e6) {
            checkNear(std::abs(vin), 0.5, 1e-9, "line: quarter-wave transformer, 100 ohm appear as 25 ohm");
            // The power is passed on: |vout|^2 / 100 = |vin|^2 / 25.
            checkNear(std::abs(vout), 1.0, 1e-9, "line: quarter-wave transformer, output voltage");
            checkNear(std::arg(vout), -kPi / 2.0, 1e-9, "line: quarter-wave transformer, phase");
        } else if (f == 200e6) {
            checkNear(std::abs(vin), 0.8, 1e-9, "line: a half-wave line repeats its load");
        } else {
            // 1/8 wave: zin = z0 (zl + j z0) / (z0 + j zl)
            const std::complex<double> j(0, 1), zin = 50.0 * (100.0 + j * 50.0) / (50.0 + j * 100.0);
            checkNear(std::abs(vin - zin / (25.0 + zin)), 0.0, 1e-9, "line: input impedance of a 1/8-wave line");
        }
    });

    // --- lossy line ---
    // DC: the series resistance and the shunt conductance. With g = 0 the line is a resistor.
    Netlist lossyDc = parseNetlistText(R"json({"components": [
        {"type": "V", "name": "V1", "nodes": ["src", "0"], "dc": 1},
        {"type": "R", "name": "RS", "nodes": ["src", "in"], "value": 50},
        {"type": "T", "name": "T1", "nodes": ["in", "0", "out", "0"], "z0": 50, "td": "5n", "r": 20},
        {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 50}]})json");
    const Vector xl = solveOperatingPoint(*lossyDc.circuit, lossyDc.options);
    checkNear(probe(*lossyDc.circuit, xl, "v(out)"), 50.0 / 120.0, 1e-9, "lossy line: series resistance at DC");
    checkNear(probe(*lossyDc.circuit, xl, "i(T1)"), 1.0 / 120.0, 1e-9, "lossy line: current into the line");

    // AC against the exact solution of the telegrapher's equations:
    // gamma = sqrt((R + jwL)(G + jwC)), zc = sqrt((R + jwL)/(G + jwC)),
    // vout/vin = 1 / (cosh(gamma l) + zc/RL sinh(gamma l)).
    const auto lossyAc = [&](const std::string& extra, double tol, const std::string& what) {
        Netlist nl = parseNetlistText(R"json({"components": [
            {"type": "V", "name": "V1", "nodes": ["src", "0"], "ac": 1},
            {"type": "R", "name": "RS", "nodes": ["src", "in"], "value": 30},
            {"type": "T", "name": "T1", "nodes": ["in", "0", "out", "0"], "l": 250e-9, "c": 100e-12,
             "r": 15, "g": 2e-3, "len": 1.5)json" + extra + R"json(},
            {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 120}]})json");
        const Vector x0 = solveOperatingPoint(*nl.circuit, nl.options);
        const Probe pi = parseProbe(*nl.circuit, "v(in)"), po = parseProbe(*nl.circuit, "v(out)");
        double worst = 0.0;
        runAc(*nl.circuit, x0, {1e6, 30e6, 100e6, 400e6}, [&](double f, const Eigen::VectorXcd& x) {
            const std::complex<double> j(0, 1), zs = 15.0 + j * 2.0 * kPi * f * 250e-9,
                                       yp = 2e-3 + j * 2.0 * kPi * f * 100e-12;
            const std::complex<double> gl = std::sqrt(zs * yp) * 1.5, zc = std::sqrt(zs / yp);
            const std::complex<double> zin = zc * (120.0 + zc * std::tanh(gl)) / (zc + 120.0 * std::tanh(gl));
            const std::complex<double> vin = zin / (30.0 + zin);
            const std::complex<double> vout = vin / (std::cosh(gl) + zc / 120.0 * std::sinh(gl));
            worst = std::max({worst, std::abs(pi.phasor(x) - vin), std::abs(po.phasor(x) - vout)});
        });
        checkNear(worst, 0.0, tol, what);
    };
    lossyAc("", 5e-3, "lossy line: AC agrees with the telegrapher's equations (default sections)");
    lossyAc(R"json(, "segments": 60)json", 1e-4, "lossy line: AC converges with the number of sections");

    // Step response: the first arrival at a matched end is attenuated by exp(-r / (2 z0)).
    Netlist lossyStep = parseNetlistText(R"json({
        "options": {"reltol": 1e-5},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["src", "0"],
             "wave": {"type": "pulse", "v1": 0, "v2": 1, "delay": "1n", "rise": "0.1n"}},
            {"type": "R", "name": "RS", "nodes": ["src", "in"], "value": 50},
            {"type": "T", "name": "T1", "nodes": ["in", "0", "out", "0"], "z0": 50, "td": "5n", "r": 10, "segments": 20},
            {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 50}]})json");
    const Trace tls = transient(lossyStep, 7e-9, IntegrationMethod::Bdf, false);
    checkNear(at(lossyStep, tls, "v(out)", 6.5e-9), 0.5 * std::exp(-10.0 / 100.0), 2e-3,
              "lossy line: attenuation of the first arrival");

    // --- delay() in expressions ---
    // A delayed copy of a signal.
    Netlist copy = parseNetlistText(R"json({
        "options": {"reltol": 1e-6},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "wave": {"type": "sin", "amplitude": 1, "freq": "1k"}},
            {"type": "B", "name": "B1", "nodes": ["out", "0"], "v": "2 * delay(v(in), 0.3m)"},
            {"type": "R", "name": "R1", "nodes": ["out", "0"], "value": "1k"}]})json");
    const Trace tc = transient(copy, 3e-3, IntegrationMethod::Bdf, false);
    double worst = 0.0;
    for (size_t k = 0; k < tc.t.size(); ++k) {
        const double expected = tc.t[k] < 0.3e-3 ? 0.0 : 2.0 * std::sin(2.0 * kPi * 1e3 * (tc.t[k] - 0.3e-3));
        worst = std::max(worst, std::abs(probe(*copy.circuit, tc.x[k], "v(out)") - expected));
    }
    checkNear(worst, 0.0, 2e-3, "delay: a source follows its input after the delay");

    // Delay differential equation x' = -x(t - 1) with x = 1 up to t = 0:
    // x = 1 - t on [0, 1], then 1 - t + (t - 1)^2 / 2 on [1, 2], ...
    Netlist dde = parseNetlistText(R"json({
        "options": {"reltol": 1e-6},
        "variables": {"x": {"init": 1}},
        "equations": ["der(x) = -delay(x, 1)"]})json");
    const Trace td = transient(dde, 3.0, IntegrationMethod::Bdf, true);
    worst = 0.0;
    for (size_t k = 0; k < td.t.size(); ++k) {
        const double t = td.t[k];
        const double expected = t <= 1.0 ? 1.0 - t
                                : t <= 2.0 ? 1.0 - t + (t - 1.0) * (t - 1.0) / 2.0
                                           : 1.0 - t + (t - 1.0) * (t - 1.0) / 2.0 - std::pow(t - 2.0, 3) / 6.0;
        worst = std::max(worst, std::abs(probe(*dde.circuit, td.x[k], "x") - expected));
    }
    checkNear(worst, 0.0, 1e-3, "delay: delay differential equation");
    checkTrue(td.t.size() > 10, "delay: the step size is limited by the delay");

    // Outside of a time integration the delay is transparent.
    Netlist op = parseNetlistText(R"json({
        "variables": {"y": {"guess": 0}},
        "components": [{"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 3},
                       {"type": "R", "name": "R1", "nodes": ["a", "0"], "value": 1}],
        "equations": ["y = 2 * delay(v(a), 1m)"]})json");
    const Vector xo = solveOperatingPoint(*op.circuit, op.options);
    checkNear(probe(*op.circuit, xo, "y"), 6.0, 1e-9, "delay: no effect at the operating point");
    checkTrue(throws([] {
                  parseNetlistText(R"json({"variables": {"y": {"guess": 0}},
                                           "equations": ["y = delay(time, y)"]})json");
              }),
              "delay: a variable delay time is rejected");
}

void testVectors() {
    // Heat conduction along a rod, discretized into n points (method of lines).
    // Ends held at 1 and 0: the steady state is a straight line.
    Netlist rod = parseNetlistText(R"json({
        "parameters": {"n": 11, "a": 0.5},
        "variables": {"T[n]": {"guess": 0}, "total": {"guess": 0}},
        "equations": [
            "T[1] = 1",
            {"for": "i", "from": 2, "to": "n-1", "equation": "der(T[i]) = a * (T[i-1] - 2*T[i] + T[i+1])"},
            "T[n] = 0",
            "total = sum(k, 1, n, T[k])"
        ]})json");
    const Vector xr = solveOperatingPoint(*rod.circuit, rod.options);
    checkNear(probe(*rod.circuit, xr, "T[4]"), 0.7, 1e-9, "vector: steady temperature profile");
    checkNear(probe(*rod.circuit, xr, "T[10]"), 0.1, 1e-9, "vector: steady temperature profile, last inner point");
    checkNear(probe(*rod.circuit, xr, "total"), 5.5, 1e-9, "vector: sum over the elements");
    checkNear(probe(*rod.circuit, xr, "T[2*3] + T[n-1]"), 0.6, 1e-9, "vector: index expressions in an output");
    checkTrue(makeProbes(*rod.circuit, {"T[*]", "total"}).size() == 12, "vector: T[*] stands for all elements");
    checkTrue(rod.circuit->size() == 12, "vector: one unknown per element");

    // Decay of the fundamental mode with both ends at 0: every point falls as
    // exp(-lambda t) with lambda = 4 a sin^2(pi / (2 (n - 1))), exactly.
    const int n = 21;
    std::string init;
    for (int i = 0; i < n; ++i) init += (i ? ", " : "") + std::to_string(std::sin(kPi * i / (n - 1)));
    Netlist mode = parseNetlistText(R"json({
        "options": {"reltol": 1e-7},
        "parameters": {"n": )json" + std::to_string(n) + R"json(, "a": 30},
        "variables": {"u[n]": {"init": [)json" + init + R"json(]}},
        "equations": [
            "u[1] = 0",
            {"for": "i", "from": 2, "to": "n-1", "equation": "der(u[i]) = a * (u[i-1] - 2*u[i] + u[i+1])"},
            "u[n] = 0"
        ]})json");
    const Trace tm = transient(mode, 1.0, IntegrationMethod::Bdf, true);
    const double lambda = 4.0 * 30.0 * std::pow(std::sin(kPi / (2.0 * (n - 1))), 2);
    checkNear(probe(*mode.circuit, tm.x.back(), "u[11]"), std::exp(-lambda), 1e-5, "vector: decay of a diffusion mode");
    checkNear(probe(*mode.circuit, tm.x.back(), "u[6]"), std::exp(-lambda) * std::sin(kPi * 5 / 20), 1e-5,
              "vector: the mode keeps its shape");

    // Nested loops, several equations per index, vectors inside a subcircuit.
    Netlist nested = parseNetlistText(R"json({
        "subcircuits": {"cell": {"ports": [], "variables": {"w[2]": {"guess": 0}},
                                 "equations": ["w[1] = 3", "w[2] = 2 * w[1]"]}},
        "components": [{"type": "X", "name": "A", "subcircuit": "cell", "nodes": []},
                       {"type": "X", "name": "B", "subcircuit": "cell", "nodes": []}],
        "variables": {"p[6]": {"guess": 0}, "q[3]": {"guess": 0}},
        "equations": [
            {"for": "i", "from": 1, "to": 2, "equations": [
                {"for": "j", "from": 1, "to": 3, "equation": "p[3*(i-1) + j] = 10*i + j"}
            ]},
            {"for": "i", "from": 1, "to": 3, "equation": "q[i] = p[i] + p[i+3]"}
        ]})json");
    const Vector xn = solveOperatingPoint(*nested.circuit, nested.options);
    checkNear(probe(*nested.circuit, xn, "p[5]"), 22.0, 1e-12, "vector: nested loops");
    checkNear(probe(*nested.circuit, xn, "q[3]"), 13.0 + 23.0, 1e-12, "vector: element-wise equation");
    checkNear(probe(*nested.circuit, xn, "B.w[2]"), 6.0, 1e-12, "vector: inside a subcircuit");

    const auto fails = [](const char* text) { return throws([&] { parseNetlistText(text); }); };
    checkTrue(fails(R"json({"variables": {"x[3]": {"guess": 0}},
                        "equations": [{"for": "i", "from": 1, "to": 3, "equation": "x[i+1] = 1"}]})json"),
              "vector: an index outside the vector is rejected");
    checkTrue(fails(R"json({"variables": {"x[2]": {"guess": 0}}, "equations": ["x[1] = 1", "x[x[1]] = 2"]})json"),
              "vector: a variable index is rejected");
    checkTrue(fails(R"json({"variables": {"x[3]": {"guess": 0}}, "equations": ["x[1] = 1", "x[2] = 2"]})json"),
              "vector: the number of equations has to match the number of elements");
    checkTrue(fails(R"json({"variables": {"x[2]": {"init": [1, 2, 3]}}, "equations": ["x[1] = 1", "x[2] = 2"]})json"),
              "vector: a list of start values of the wrong length is rejected");
}

void testStructuralIndex() {
    const auto analyze = [](const char* text, Circuit** keep = nullptr) {
        static std::unique_ptr<Netlist> held;
        held = std::make_unique<Netlist>(parseNetlistText(text));
        held->circuit->setTransientMode(true);
        if (keep) *keep = held->circuit.get();
        return structuralIndex(*held->circuit, Vector::Zero(held->circuit->size()));
    };
    // An ordinary differential equation: index 0.
    IndexAnalysis a = analyze(R"json({"variables": {"x": 1, "y": 0},
                                      "equations": ["der(x) = y", "der(y) = -x"]})json");
    checkTrue(a.index == 0 && !a.singular, "index: ordinary differential equation");
    // With an algebraic equation that determines its unknown: index 1.
    a = analyze(R"json({"variables": {"x": 1, "y": {"guess": 0}},
                        "equations": ["der(x) = -y", "y = x^3"]})json");
    checkTrue(a.index == 1, "index: algebraic equation for its own unknown");
    // Pendulum with the position constraint only: index 3, the constraint twice.
    Circuit* ckt = nullptr;
    a = analyze(R"json({"options": {"index_reduction": false},
                        "variables": {"x": 0.5, "y": -0.8, "u": 0, "v": 0, "lam": {"guess": 0}},
                        "equations": ["der(x) = u", "der(y) = v", "der(u) = -x*lam",
                                      "der(v) = -9.81 - y*lam", "0 = x^2 + y^2 - 1"]})json", &ckt);
    checkTrue(a.index == 3, "index: pendulum with position constraint has index 3");
    int twice = -1;
    for (int i = 0; i < ckt->size(); ++i)
        if (a.differentiations[i] == 2) twice = twice < 0 ? i : -2;
    checkTrue(twice >= 0 && ckt->equationText(twice) == "\"0 = x^2 + y^2 - 1\"",
              "index: the constraint is named as the equation to differentiate twice");
    // With the velocity constraint instead: index 2.
    a = analyze(R"json({"variables": {"x": 0.5, "y": -0.8, "u": 0, "v": 0, "lam": {"guess": 0}},
                        "equations": ["der(x) = u", "der(y) = v", "der(u) = -x*lam",
                                      "der(v) = -9.81 - y*lam", "0 = x*u + y*v"]})json");
    checkTrue(a.index == 2, "index: pendulum with velocity constraint has index 2");
    // Circuits: RC is index 1, a capacitor across a voltage source index 2.
    a = analyze(R"json({"components": [
        {"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 1},
        {"type": "R", "name": "R1", "nodes": ["a", "b"], "value": 1},
        {"type": "C", "name": "C1", "nodes": ["b", "0"], "value": 1}]})json");
    checkTrue(a.index == 1, "index: RC circuit");
    a = analyze(R"json({"components": [
        {"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 1},
        {"type": "C", "name": "C1", "nodes": ["a", "0"], "value": 1}]})json");
    checkTrue(a.index == 2, "index: capacitor across a voltage source");
    // Two equations for one unknown, none for the other.
    a = analyze(R"json({"variables": {"x": {"guess": 0}, "y": {"guess": 0}},
                        "equations": ["x = 1", "2*x = 2"]})json");
    checkTrue(a.singular, "index: structurally singular system is recognized");

    // The transient analysis warns about the index 3 formulation.
    Netlist nl = parseNetlistText(R"json({
        "options": {"index_reduction": false},
        "variables": {"x": 0.5, "y": -0.8, "u": 0, "v": 0, "lam": {"guess": 0}},
        "equations": ["der(x) = u", "der(y) = v", "der(u) = -x*lam", "der(v) = -9.81 - y*lam",
                      "0 = x^2 + y^2 - 1"],
        "analyses": [{"type": "tran", "tstop": 1e-3, "uic": true, "save": false}]})json");
    RunConfig cfg;
    cfg.outDir = (std::filesystem::temp_directory_path() / "esim_tests").string();
    cfg.stem = "index3";
    std::ostringstream log;
    try {
        runAnalyses(nl, cfg, log);
    } catch (const std::exception&) {  // the integration itself may fail; the warning is what counts
    }
    checkTrue(log.str().find("index 3") != std::string::npos &&
                  log.str().find("\"0 = x^2 + y^2 - 1\" would have to be differentiated 2 times") != std::string::npos,
              "index: warning names the constraint");
}

void testSpeedFeatures() {
    // --- reuse of the factorized Jacobian: same result, about half the factorizations ---
    const char* rectifier = R"json({
        "options": {"reltol": 1e-5, "reuse_jacobian": %s},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "wave": {"type": "sin", "amplitude": 10, "freq": 50}},
            {"type": "D", "name": "D1", "nodes": ["in", "out"], "is": 1e-12, "rs": 0.5, "cj0": "20p"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "100u"},
            {"type": "R", "name": "RL", "nodes": ["out", "0"], "value": 200}
        ]})json";
    double vout[2];
    TransientStats st[2];
    for (int k = 0; k < 2; ++k) {
        char text[1024];
        std::snprintf(text, sizeof text, rectifier, k == 0 ? "true" : "false");
        Netlist nl = parseNetlistText(text);
        TransientOptions to;
        to.tstop = 0.06;
        to.reltol = nl.options.reltol;
        to.newton.reltol = nl.options.reltol;
        to.reuseJacobian = nl.options.reuseJacobian;
        nl.circuit->setTransientMode(true);
        nl.circuit->setInitialPointMode(true);
        const Vector x0 = solveOperatingPoint(*nl.circuit, nl.options);
        nl.circuit->setInitialPointMode(false);
        Vector last = x0;
        st[k] = runTransient(*nl.circuit, x0, nullptr, to, [&](double, const Vector& x) { last = x; });
        vout[k] = probe(*nl.circuit, last, "v(out)");
    }
    checkNear(vout[0], vout[1], 1e-4, "jacobian reuse: same result");
    checkTrue(st[0].factorizations < 0.75 * st[0].newtonIterations, "jacobian reuse: fewer factorizations than iterations");
    checkTrue(st[1].factorizations >= st[1].newtonIterations - st[1].accepted, "without reuse every iteration factorizes");
    checkTrue(st[0].newtonIterations < 1.15 * st[1].newtonIterations, "jacobian reuse: no more iterations to speak of");
    std::cout << "    rectifier: " << st[0].newtonIterations << " iterations, " << st[0].factorizations
              << " factorizations (without reuse " << st[1].newtonIterations << ", " << st[1].factorizations << ")\n";

    // --- series of runs on several threads: identical files ---
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "esim_tests";
    const auto run = [&](const char* text, const std::string& stem, int threads) {
        Netlist nl = parseNetlistText(text);
        RunConfig cfg;
        cfg.outDir = dir.string();
        cfg.stem = stem;
        cfg.threads = threads;
        std::ostringstream log;
        runAnalyses(nl, cfg, log);
    };
    const auto content = [&](const std::string& name) {
        std::ifstream in(dir / name);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const char* study = R"json({
        "parameters": {"R": "1k", "C": "1u"},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 0, "ac": 1,
             "wave": {"type": "pulse", "v1": 0, "v2": 1, "rise": "1u"}},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "R"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "C"}
        ],
        "analyses": [
            {"type": "tran", "tstop": "5m", "name": "sweep",
             "param_sweep": [{"param": "R", "values": [500, 1000, 2000]}, {"param": "C", "values": ["0.5u", "1u", "2u"]}],
             "measure": {"t50": {"cross": "v(out)", "level": 0.5}}},
            {"type": "ac", "sweep": "dec", "points": 20, "fstart": 1, "fstop": "100k", "name": "mc",
             "monte_carlo": {"runs": 40, "seed": 7, "vary": {"R": {"tol": 0.05}, "C": {"tol": 0.1}}},
             "measure": {"fc": {"f3db": "v(out)"}}},
            {"type": "sens", "name": "sens", "analysis": {"type": "tran", "tstop": "5m",
             "measure": {"t50": {"cross": "v(out)", "level": 0.5}}}}
        ],
        "outputs": ["v(out)"]})json";
    run(study, "par1", 1);
    run(study, "par4", 4);
    for (const char* file : {"sweep.csv", "sweep.measure.csv", "mc.measure.csv", "sens.csv"}) {
        const std::string a = content(std::string("par1.") + file), b = content(std::string("par4.") + file);
        checkTrue(!a.empty() && a == b, std::string("parallel runs: ") + file + " does not depend on the number of threads");
    }
    // A failing run is reported with its parameters, whichever thread it ran on.
    std::string message;
    try {
        run(R"json({"parameters": {"R": 1},
                    "components": [{"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 1},
                                   {"type": "R", "name": "R1", "nodes": ["a", "0"], "value": "R"}],
                    "analyses": [{"type": "op", "param_sweep": {"param": "R", "values": [1, 2, 3, 0, 5, 6, 7, 8]}}]})json",
            "parfail", 4);
    } catch (const std::exception& e) {
        message = e.what();
    }
    checkTrue(message.find("param_sweep, R = 0") != std::string::npos, "parallel runs: the failing run is named");
}

void testIndexReduction() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "esim_tests";
    const auto runAll = [&](const std::string& text, const std::string& stem, std::string* logText = nullptr) {
        Netlist nl = parseNetlistText(text);
        RunConfig cfg;
        cfg.outDir = dir.string();
        cfg.stem = stem;
        nlohmann::json summary = nlohmann::json::array();
        cfg.summary = &summary;
        std::ostringstream log;
        runAnalyses(nl, cfg, log);
        if (logText) *logText = log.str();
        return summary;
    };

    // Pendulum written with the position constraint only (index 3). The
    // constraint is imposed on its derivative; the length stays what it was.
    // Exact period for an amplitude of 0.5 rad: 2.03787 s.
    const std::string pendulum = R"json({
        "parameters": {"g": 9.81, "L": 1, "theta0": 0.5},
        "options": {"reltol": 1e-7},
        "variables": {"x": "L*sin(theta0)", "y": "-L*cos(theta0)", "u": 0, "v": 0, "lam": {"guess": 0}},
        "equations": ["der(x) = u", "der(y) = v", "der(u) = -2*x*lam", "der(v) = -g - 2*y*lam",
                      "0 = x^2 + y^2 - L^2"],
        "analyses": [{"type": "tran", "tstop": 10, "uic": true, "save": false,
                      "measure": {"period": {"period": "x", "level": 0},
                                  "length_error": {"max": "abs(x^2 + y^2 - L^2)"},
                                  "energy_drift": {"pp": "0.5*(u^2 + v^2) + g*y"}}}]})json";
    std::string log;
    nlohmann::json s = runAll(pendulum, "reduce_pendulum", &log);
    checkTrue(log.find("index 3 reduced to 2") != std::string::npos &&
                  log.find("added  0 = 2*x*u + 2*y*v") != std::string::npos,
              "index reduction: reported with the added equation");
    std::cout << log.substr(0, log.find("[tran]"));
    checkTrue(log.find("warning") == std::string::npos, "index reduction: no warning for consistent start values");
    checkNear(s[0]["measures"]["period"].get<double>(), 2.03787, 2e-5, "index reduction: period of the pendulum");
    checkNear(s[0]["measures"]["length_error"].get<double>(), 0.0, 1e-9, "index reduction: the constraint does not drift");
    checkNear(s[0]["measures"]["energy_drift"].get<double>(), 0.0, 2e-4, "index reduction: energy of the pendulum");
    std::cout << "    reduced pendulum: period " << s[0]["measures"]["period"].get<double>() << ", length error "
              << s[0]["measures"]["length_error"].get<double>() << ", energy drift "
              << s[0]["measures"]["energy_drift"].get<double>() << std::endl;

    // A constraint that depends on time: the pivot of the pendulum is driven.
    // Checked against the same model written in the angle, an ordinary differential equation.
    s = runAll(R"json({
        "parameters": {"g": 9.81, "L": 1, "A": 0.1, "w": 3},
        "options": {"reltol": 1e-7},
        "variables": {"x": 0, "y": -1, "u": "A*w", "v": 0, "lam": {"guess": 0},
                      "th": 0, "om": 0},
        "equations": ["der(x) = u", "der(y) = v", "der(u) = -2*(x - A*sin(w*time))*lam",
                      "der(v) = -g - 2*y*lam", "0 = (x - A*sin(w*time))^2 + y^2 - L^2",
                      "der(th) = om", "der(om) = -g/L*sin(th) + A*w^2/L*sin(w*time)*cos(th)"],
        "analyses": [{"type": "tran", "tstop": 5, "uic": true, "save": false,
                      "measure": {"difference": {"max": "abs(atan2(x - A*sin(w*time), -y) - th)"},
                                  "swing": {"max": "abs(th)"}}}]})json", "reduce_driven");
    checkTrue(s[0]["measures"]["swing"].get<double>() > 0.05, "index reduction: the driven pendulum swings");
    checkNear(s[0]["measures"]["difference"].get<double>(), 0.0, 2e-5, "index reduction: time-dependent constraint");

    // Index 4: three integrators whose first state is prescribed, the input u is
    // the unknown. x1 = sin(t) requires u = x1''' = -cos(t). The constraint is
    // differentiated twice symbolically and imposed on that derivative.
    s = runAll(R"json({
        "options": {"reltol": 1e-7},
        "variables": {"x1": 0, "x2": 1, "x3": 0, "u": {"guess": -1}},
        "equations": ["der(x1) = x2", "der(x2) = x3", "der(x3) = u", "x1 = sin(time)"],
        "analyses": [{"type": "tran", "tstop": 6, "uic": true, "save": false,
                      "measure": {"path": {"max": "abs(x1 - sin(time))"},
                                  "input": {"max": "abs(u + cos(time))", "from": 0.01}}}]})json",
               "reduce_index4", &log);
    checkTrue(log.find("index 4 reduced to 2") != std::string::npos, "index reduction: index 4 is reduced");
    checkNear(s[0]["measures"]["path"].get<double>(), 0.0, 1e-5, "index reduction: index 4, the state follows its path");
    checkNear(s[0]["measures"]["input"].get<double>(), 0.0, 1e-3, "index reduction: index 4, the input is found");

    // Not reducible: the derivative of a node voltage is not available as an expression.
    Netlist no = parseNetlistText(R"json({
        "components": [{"type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 1},
                       {"type": "R", "name": "R1", "nodes": ["a", "b"], "value": 1},
                       {"type": "C", "name": "C1", "nodes": ["b", "0"], "value": 1}],
        "variables": {"x1": 0, "x2": 0, "x3": 0, "u": {"guess": 0}},
        "equations": ["der(x1) = x2", "der(x2) = x3", "der(x3) = u", "x1 = v(b)"]})json");
    checkTrue(no.circuit->notes.size() == 1 &&
                  no.circuit->notes[0].find("automatic reduction not possible") != std::string::npos,
              "index reduction: a constraint that cannot be differentiated is reported");

    // The operating point still uses the original equations.
    Netlist op = parseNetlistText(R"json({
        "variables": {"x": {"guess": 0.6}, "y": {"guess": -0.8}, "u": 0, "v": 0, "lam": {"guess": 5}},
        "equations": ["der(x) = u", "der(y) = v", "der(u) = -2*x*lam", "der(v) = -9.81 - 2*y*lam",
                      "0 = x^2 + y^2 - 1"]})json");
    const Vector xo = solveOperatingPoint(*op.circuit, op.options);
    checkNear(probe(*op.circuit, xo, "y"), -1.0, 1e-9, "index reduction: the operating point is the hanging pendulum");
    checkNear(probe(*op.circuit, xo, "lam"), 9.81 / 2.0, 1e-9, "index reduction: rod force at rest");
}

void testParallelEvaluation() {
    // A rectifier ladder with 1600 components: large enough for the evaluation
    // in blocks. The result must not depend on the number of threads at all,
    // and agree with the plain sequential evaluation up to rounding.
    std::string comps = R"json({"type": "V", "name": "V1", "nodes": ["n0", "0"],
                                "wave": {"type": "sin", "amplitude": 5, "freq": 1000}})json";
    const int stages = 400;
    for (int k = 0; k < stages; ++k) {
        const std::string a = "n" + std::to_string(k), b = "n" + std::to_string(k + 1), m = "m" + std::to_string(k);
        comps += R"json(, {"type": "R", "nodes": [")json" + a + "\", \"" + b + R"json("], "value": 100})json";
        comps += R"json(, {"type": "D", "nodes": [")json" + b + "\", \"" + m + R"json("], "is": 1e-14, "cj0": "2p"})json";
        comps += R"json(, {"type": "R", "nodes": [")json" + m + R"json(", "0"], "value": "10k"})json";
        comps += R"json(, {"type": "C", "nodes": [")json" + m + R"json(", "0"], "value": "100n"})json";
    }
    const std::string text = "{\"components\": [" + comps + "]}";
    const auto simulate = [&](int threads, TransientStats& st) {
        Netlist nl = parseNetlistText(text);
        Circuit& ckt = *nl.circuit;
        if (threads > 0) ckt.setThreads(threads);
        TransientOptions to;
        to.tstop = 1e-3;
        ckt.setTransientMode(true);
        ckt.setInitialPointMode(true);
        const Vector x0 = solveOperatingPoint(ckt, nl.options);  // another mode than the integration
        ckt.setInitialPointMode(false);
        Vector last = x0;
        st = runTransient(ckt, x0, nullptr, to, [&](double, const Vector& x) { last = x; });
        return last;
    };
    TransientStats s0, s1, s2, s5;
    const Vector plain = simulate(0, s0), one = simulate(1, s1), two = simulate(2, s2), five = simulate(5, s5);
    checkTrue(Circuit::kParallelDevices <= 1601, "parallel evaluation: the test circuit is large enough");
    checkTrue((one - two).cwiseAbs().maxCoeff() == 0.0 && (one - five).cwiseAbs().maxCoeff() == 0.0,
              "parallel evaluation: identical results for 1, 2 and 5 threads");
    checkTrue(s1.accepted == s2.accepted && s1.newtonIterations == s5.newtonIterations,
              "parallel evaluation: identical step sequence");
    checkNear((plain - one).cwiseAbs().maxCoeff(), 0.0, 1e-6, "parallel evaluation: agrees with the sequential evaluation");
    checkTrue(s0.accepted > 20, "parallel evaluation: the comparison covers a real simulation");
}

void testBsim3() {
    // The drain current itself is compared with a second implementation in
    // tests/test_bsim3.py; here: the device inside circuits.
    const std::string nmodel = R"json("level": "bsim3", "tox": 4.1e-9, "xj": 1e-7, "nch": 2.3549e17, "vth0": 0.3694,
        "k1": 0.5789, "k2": 1.11e-3, "k3": 1e-3, "dvt0": 1.295, "dvt1": 0.342, "dvt2": 0.0396, "u0": 293.2,
        "ua": -1.219e-9, "ub": 2.326e-18, "uc": 7.06e-11, "vsat": 1.676e5, "a0": 2, "ags": 0.476, "keta": -0.0139,
        "a1": 1.09e-3, "a2": 0.33, "rdsw": 105.6, "prwg": 0.5, "prwb": -0.2, "lint": 1.716e-8, "voff": -0.0948,
        "nfactor": 2.186, "eta0": 2.665e-3, "etab": 6.03e-5, "dsub": 0.0442, "pclm": 1.746, "pdiblc1": 0.3258,
        "pdiblc2": 2.7e-3, "pdiblcb": -0.1, "drout": 0.9787, "pscbe1": 4.49e10, "pscbe2": 3.67e-8,
        "cgdo": 8.58e-10, "cgso": 8.58e-10, "cj": 9.47e-4, "pb": 0.8, "mj": 0.3726, "cjsw": 1.9e-10)json";
    const std::string pmodel = R"json("level": "bsim3", "tox": 4.1e-9, "xj": 1e-7, "nch": 4.1589e17, "vth0": -0.3823,
        "k1": 0.5722, "k2": 0.022, "k3": 0.1577, "dvt0": 0.6235, "dvt1": 0.2479, "dvt2": 0.1, "u0": 109.5,
        "ua": 1.316e-9, "ub": 1e-21, "uc": -1e-10, "vsat": 1.055e5, "a0": 1.58, "ags": 0.3115, "keta": 0.0299,
        "a1": 0.3887, "a2": 0.401, "rdsw": 199.2, "prwg": 0.5, "prwb": -0.4947, "lint": 2.94e-8, "voff": -0.0936,
        "nfactor": 2, "eta0": 3.5e-4, "etab": -4.8e-4, "pclm": 0.964, "pdiblc1": 3.03e-3, "pdiblcb": -1e-3,
        "pscbe1": 8e10, "pscbe2": 8.27e-10, "cgdo": 6.3e-10, "cgso": 6.3e-10, "cj": 1.18e-3, "pb": 0.85,
        "mj": 0.412, "cjsw": 2.4e-10)json";
    const std::string models = "\"models\": {\"n18\": {\"type\": \"nmos\", " + nmodel + "}, \"p18\": {\"type\": \"pmos\", " +
                               pmodel + "}}";

    // Derivatives: conductances and capacitances against finite differences.
    for (const std::string& kind : {std::string("n18"), std::string("p18")}) {
        Netlist nl = parseNetlistText("{" + models + R"json(, "components": [
            {"type": "R", "nodes": ["a", "0"], "value": 1000}, {"type": "R", "nodes": ["b", "0"], "value": 1000},
            {"type": "R", "nodes": ["c", "0"], "value": 1000}, {"type": "R", "nodes": ["d", "0"], "value": 1000},
            {"type": "M", "name": "X", "nodes": ["a", "b", "c", "d"], "model": ")json" + kind +
                                      R"json(", "w": "2u", "l": "0.18u", "ad": "1p", "as": "1p", "pd": "3u", "ps": "3u"}]})json");
        Circuit& ckt = *nl.circuit;
        unsigned seed = 4711;
        for (int trial = 0; trial < 10; ++trial) {
            Vector x(ckt.size());
            for (int i = 0; i < ckt.size(); ++i) {
                seed = seed * 1664525u + 1013904223u;
                x[i] = 1.8 * ((seed >> 8) / 16777216.0);
            }
            // bulk below all terminals of the n-channel, above those of the p-channel
            x[ckt.findNode("d")] = kind == "n18" ? -0.5 * x[ckt.findNode("d")] : 1.8 + 0.5 * x[ckt.findNode("d")];
            checkJacobian(ckt, x, "bsim3 " + kind + ", bias point " + std::to_string(trial));
        }
    }

    // Inverter: transfer curve from rail to rail, switching near half the supply.
    Netlist inv = parseNetlistText("{" + models + R"json(, "components": [
        {"type": "V", "name": "VDD", "nodes": ["vdd", "0"], "dc": 1.8},
        {"type": "V", "name": "VIN", "nodes": ["in", "0"], "dc": 0},
        {"type": "M", "name": "MP", "nodes": ["out", "in", "vdd", "vdd"], "model": "p18", "w": "2u", "l": "0.18u"},
        {"type": "M", "name": "MN", "nodes": ["out", "in", "0", "0"], "model": "n18", "w": "1u", "l": "0.18u"}]})json");
    double vm = 0.0, previous = 0.0;
    auto* vin = dynamic_cast<IndependentSource*>(inv.circuit->findDevice("VIN"));
    Vector xi;
    for (int k = 0; k <= 180; ++k) {
        vin->setDc(0.01 * k);
        xi = solveOperatingPoint(*inv.circuit, inv.options, xi.size() ? &xi : nullptr);
        const double out = probe(*inv.circuit, xi, "v(out)");
        if (k == 0) checkNear(out, 1.8, 1e-4, "bsim3 inverter: output high");
        if (k > 0 && previous > 0.9 && out <= 0.9) vm = 0.01 * k;
        previous = out;
    }
    checkNear(previous, 0.0, 1e-4, "bsim3 inverter: output low");
    checkTrue(vm > 0.6 && vm < 1.1, "bsim3 inverter: switches near half the supply");

    // Ring oscillator of five inverters: it starts and runs at a plausible frequency.
    std::string ring = "{" + models + R"json(, "ic": {"n1": 1.8, "n3": 1.8}, "components": [
        {"type": "V", "name": "VDD", "nodes": ["vdd", "0"], "dc": 1.8})json";
    for (int k = 1; k <= 5; ++k) {
        const std::string in = "n" + std::to_string(k), out = "n" + std::to_string(k % 5 + 1);
        ring += R"json(, {"type": "M", "nodes": [")json" + out + "\", \"" + in +
                R"json(", "vdd", "vdd"], "model": "p18", "w": "2u", "l": "0.18u", "ad": "1p", "pd": "3u"})json";
        ring += R"json(, {"type": "M", "nodes": [")json" + out + "\", \"" + in +
                R"json(", "0", "0"], "model": "n18", "w": "1u", "l": "0.18u", "ad": "0.5p", "pd": "2u"})json";
        ring += R"json(, {"type": "C", "nodes": [")json" + out + R"json(", "0"], "value": "5f"})json";
    }
    ring += "]}";
    Netlist osc = parseNetlistText(ring);
    TransientStats st;
    const Trace tr = transient(osc, 2e-9, IntegrationMethod::Bdf, true, &st);
    int rising = 0;
    double first = 0.0, last = 0.0;
    for (size_t k = 1; k < tr.t.size(); ++k) {
        const double a = probe(*osc.circuit, tr.x[k - 1], "v(n3)"), b = probe(*osc.circuit, tr.x[k], "v(n3)");
        if (a < 0.9 && b >= 0.9) {
            const double tc = tr.t[k - 1] + (0.9 - a) / (b - a) * (tr.t[k] - tr.t[k - 1]);
            if (rising++ == 0) first = tc;
            last = tc;
        }
    }
    const double period = rising > 1 ? (last - first) / (rising - 1) : 0.0;
    checkTrue(rising >= 3, "bsim3 ring oscillator: oscillates");
    checkTrue(period > 50e-12 && period < 600e-12, "bsim3 ring oscillator: period of a 0.18 um ring");
    std::cout << "    bsim3 ring oscillator: period " << period * 1e12 << " ps, " << st.accepted << " steps, "
              << st.newtonFailures << " Newton failures\n";

    // A SPICE model card of level 49, and a parameter the model does not know.
    Netlist card = parseNetlistText(R"json({
        "models": {"nch": ".model nch NMOS (LEVEL=49 VERSION=3.1 TOX=4.1E-9 VTH0=0.37 U0=290 NCH=2.35E17 K1=0.58 K2=1E-3 RDSW=105 CAPMOD=2)"},
        "components": [
            {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": 1.8},
            {"type": "V", "name": "VG", "nodes": ["g", "0"], "dc": 1.8},
            {"type": "M", "name": "M1", "nodes": ["d", "g", "0", "0"], "model": "nch", "w": "1u", "l": "0.18u"}]})json");
    const Vector xc = solveOperatingPoint(*card.circuit, card.options);
    const double ion = -probe(*card.circuit, xc, "i(VD)");
    checkTrue(dynamic_cast<const Bsim3*>(card.circuit->findDevice("M1")) != nullptr, "bsim3: SPICE level 49 selects the model");
    checkTrue(ion > 2e-4 && ion < 1.5e-3, "bsim3: on-current of the imported model");
    Netlist odd = parseNetlistText(R"json({"components": [
        {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": 1},
        {"type": "nmos", "name": "M1", "nodes": ["d", "d", "0", "0"], "level": "bsim3", "w": "1u", "l": "1u", "foo": 1, "capmod": 2}]})json");
    const auto unknown = dynamic_cast<const Bsim3*>(odd.circuit->findDevice("M1"))->unknownParameters();
    checkTrue(unknown.size() == 1 && unknown[0] == "foo", "bsim3: unknown parameters are found, accepted ones are not reported");
}

void testUniformOutput() {
    Netlist nl = parseNetlistText(R"json({
        "options": {"reltol": 1e-6},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"],
             "wave": {"type": "sin", "amplitude": 1, "freq": 1000}},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "1u"}
        ]})json");
    // v' = (sin(w t) - v) / tau, v(0) = 0
    const double w = 2.0 * kPi * 1e3, tau = 1e-3;
    const auto maxError = [&](const Trace& tr) {
        double e = 0.0;
        for (size_t k = 0; k < tr.t.size(); ++k) {
            const double t = tr.t[k], d = 1.0 + w * w * tau * tau;
            const double ref =
                (std::sin(w * t) - w * tau * std::cos(w * t) + w * tau * std::exp(-t / tau)) / d;
            e = std::max(e, std::abs(probe(*nl.circuit, tr.x[k], "v(out)") - ref));
        }
        return e;
    };
    const Trace raw = transient(nl, 4e-3, IntegrationMethod::Trapezoidal, false);
    const Trace tr = transient(nl, 4e-3, IntegrationMethod::Trapezoidal, false, nullptr, 1e-4);
    checkTrue(tr.t.size() == 41, "uniform output: 41 samples");
    double gridErr = 0.0;
    for (size_t k = 0; k < tr.t.size(); ++k)
        gridErr = std::max(gridErr, std::abs(tr.t[k] - 1e-4 * static_cast<double>(k)));
    checkNear(gridErr, 0.0, 1e-15, "uniform output: sample times");
    // Interpolating must not add a noticeable error to the one of the solver's own points.
    checkTrue(maxError(tr) <= 1.2 * maxError(raw) + 1e-9, "uniform output: interpolated values");
    std::cout << "    uniform output: error at solver points " << maxError(raw) << ", on the grid "
              << maxError(tr) << '\n';
}

void testParameterSweep() {
    // The complete run from the input text to the CSV file.
    Netlist nl = parseNetlistText(R"json({
        "parameters": {"R": "1k"},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "R"},
            {"type": "R", "name": "R2", "nodes": ["out", "0"], "value": "1k"}
        ],
        "analyses": [
            {"type": "op", "param_sweep": {"param": "R", "values": ["1k", "3k", "9k"]}},
            {"type": "dc", "source": "V1", "start": 0, "stop": 2, "step": 1,
             "param_sweep": {"param": "R", "start": 1000, "stop": 3000, "points": 2}}
        ],
        "outputs": ["v(out)", "v(out)*i(V1)"]
    })json");
    RunConfig cfg;
    cfg.outDir = (std::filesystem::temp_directory_path() / "esim_tests").string();
    cfg.stem = "sweep";
    std::ostringstream log;
    runAnalyses(nl, cfg, log);

    const auto read = [&](const char* name) {
        std::ifstream in(std::filesystem::path(cfg.outDir) / name);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        return lines;
    };
    const auto op = read("sweep.op.csv");
    checkTrue(op.size() == 4, "op sweep: header and three rows");
    if (op.size() == 4) {
        checkTrue(op[0] == "R,v(out),v(out)*i(V1)", "op sweep: header");
        checkTrue(op[1] == "1000,0.5,-0.00025", "op sweep: R = 1k");
        checkTrue(op[2] == "3000,0.25,-6.25e-05", "op sweep: R = 3k");
        checkTrue(op[3] == "9000,0.1,-1e-05", "op sweep: R = 9k");
    }
    const auto dc = read("sweep.dc.csv");
    checkTrue(dc.size() == 7, "dc sweep inside a parameter sweep: 2 x 3 rows");
    if (dc.size() == 7) {
        checkTrue(dc[0] == "R,V1,v(out),v(out)*i(V1)", "nested sweep: header");
        checkTrue(dc[3] == "1000,2,1,-0.001", "nested sweep: R = 1k, V1 = 2");
        checkTrue(dc[6] == "3000,2,0.5,-0.00025", "nested sweep: R = 3k, V1 = 2");
    }
    checkTrue(throws([&] {
                  Netlist bad = parseNetlistText(R"json({
                      "parameters": {"R": 1},
                      "components": [{"type": "R", "nodes": ["a", "0"], "value": "R"}],
                      "analyses": [{"type": "op", "param_sweep": {"param": "Rx", "values": [1]}}]
                  })json");
                  std::ostringstream sink;
                  runAnalyses(bad, cfg, sink);
              }),
              "sweep over an undefined parameter is rejected");
}

void testMeasurements() {
    // y = 1 + sin(2 pi 5 x) on 0 .. 2, finely sampled.
    std::vector<double> x, y;
    for (int k = 0; k <= 20000; ++k) {
        x.push_back(1e-4 * k);
        y.push_back(1.0 + std::sin(2.0 * kPi * 5.0 * x.back()));
    }
    const std::map<std::string, double> none;
    const auto measure = [&](const char* text, const std::vector<double>& xs,
                             const std::vector<double>& ys) {
        const auto specs = parseMeasures(nlohmann::json::parse(std::string("{\"m\": ") + text + "}"), none);
        return evaluateMeasure(specs.at(0), xs, ys);
    };
    const std::pair<const char*, double> cases[] = {
        {R"json({"min": "y"})json", 0.0},
        {R"json({"max": "y"})json", 2.0},
        {R"json({"pp": "y"})json", 2.0},
        {R"json({"avg": "y"})json", 1.0},
        {R"json({"rms": "y"})json", std::sqrt(1.5)},
        {R"json({"integ": "y", "from": 0, "to": 0.05})json", 0.05 + 1.0 / (10.0 * kPi)},
        {R"json({"final": "y", "to": 0.05})json", 2.0},
        {R"json({"argmax": "y", "to": 0.1})json", 0.05},
        {R"json({"at": "y", "x": 0.025})json", 1.0 + std::sqrt(0.5)},
        {R"json({"cross": "y", "level": 1, "n": 2})json", 0.4},  // starting on the level is no crossing
        {R"json({"cross": "y", "level": 1, "edge": "fall"})json", 0.1},
        {R"json({"cross": "y", "level": 1, "edge": "both", "n": 3, "from": 0.01})json", 0.3},
        {R"json({"period": "y"})json", 0.2},
        {R"json({"freq": "y", "from": 0.5})json", 5.0},
        {R"json({"duty": "y"})json", 0.5},
        {R"json({"duty": "y", "level": 1.5})json", 1.0 / 3.0},
        {R"json({"rise": "y", "from": 0.1})json", 2.0 * std::asin(0.8) / (10.0 * kPi)},
        {R"json({"fall": "y", "low": 0.5, "high": 1.5})json", 2.0 * std::asin(0.5) / (10.0 * kPi)},
    };
    for (const auto& [text, expected] : cases)
        checkNear(measure(text, x, y), expected, 2e-6, std::string("measure ") + text);
    checkTrue(std::isnan(measure(R"json({"cross": "y", "level": 5})json", x, y)),
              "measurement without a result is NaN");
    checkTrue(throws([&] { measure(R"json({"avg": "y", "rms": "y"})json", x, y); }),
              "two functions in one measurement are rejected");
    checkTrue(throws([&] { measure(R"json({"cross": "y"})json", x, y); }), "cross without level is rejected");
    checkTrue(throws([&] { measure(R"json({"average": "y"})json", x, y); }), "unknown function is rejected");

    // Band-pass magnitude over a logarithmic frequency axis: both -3 dB points.
    std::vector<double> f, mag;
    for (int k = 0; k <= 4000; ++k) {
        f.push_back(std::pow(10.0, k / 1000.0));  // 1 .. 1e4
        const double u = f.back() / 100.0 - 100.0 / f.back();  // Q = 1, f0 = 100
        mag.push_back(1.0 / std::sqrt(1.0 + u * u));
    }
    const double lower = 100.0 * (std::sqrt(5.0) - 1.0) / 2.0, upper = 100.0 * (std::sqrt(5.0) + 1.0) / 2.0;
    checkNear(measure(R"json({"f3db_low": "y"})json", f, mag), lower, 1e-3, "lower -3 dB frequency");
    checkNear(measure(R"json({"f3db": "y"})json", f, mag), upper, 1e-3, "upper -3 dB frequency");
    checkNear(measure(R"json({"argmax": "y"})json", f, mag), 100.0, 1e-9, "frequency of the maximum");

    // Through the complete run: corner frequency of an RC low-pass over a parameter sweep.
    Netlist nl = parseNetlistText(R"json({
        "parameters": {"C": "1u"},
        "components": [
            {"type": "V", "name": "V1", "nodes": ["in", "0"], "ac": 1},
            {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k"},
            {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "C"}
        ],
        "analyses": [
            {"type": "ac", "sweep": "dec", "points": 100, "fstart": 1, "fstop": "100k", "save": false,
             "param_sweep": {"param": "C", "values": ["1u", "100n"]},
             "measure": {"fc": {"f3db": "v(out)"}, "gain": {"at": "v(out)", "x": 1}}}
        ]
    })json");
    RunConfig cfg;
    cfg.outDir = (std::filesystem::temp_directory_path() / "esim_tests").string();
    cfg.stem = "meas";
    std::ostringstream log;
    runAnalyses(nl, cfg, log);
    std::ifstream in(std::filesystem::path(cfg.outDir) / "meas.ac.measure.csv");
    std::string header, row1, row2;
    std::getline(in, header);
    std::getline(in, row1);
    std::getline(in, row2);
    checkTrue(header == "C,fc,gain", "measurement table of a sweep: header");
    double c1 = 0, fc1 = 0, g1 = 0, c2 = 0, fc2 = 0, g2 = 0;
    checkTrue(std::sscanf(row1.c_str(), "%lf,%lf,%lf", &c1, &fc1, &g1) == 3 &&
                  std::sscanf(row2.c_str(), "%lf,%lf,%lf", &c2, &fc2, &g2) == 3,
              "measurement table of a sweep: two rows");
    checkNear(fc1, 1.0 / (2.0 * kPi * 1e-3), 0.05, "measured corner frequency, C = 1u");
    checkNear(fc2, 1.0 / (2.0 * kPi * 1e-4), 0.5, "measured corner frequency, C = 100n");
    checkNear(g1, 1.0, 1e-4, "measured low-frequency gain");
    checkTrue(!std::filesystem::exists(std::filesystem::path(cfg.outDir) / "meas.ac.csv"),
              "\"save\": false writes no curves");
}

void testInputErrors() {
    const auto fails = [](const char* text) {
        try {
            parseNetlistText(text);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    checkTrue(fails(R"json({"components": [{"type": "R", "nodes": ["a", "b"], "value": 1}]})json"),
              "circuit without ground is rejected");
    checkTrue(fails(R"json({"components": [{"type": "X", "nodes": ["a", "0"]}]})json"),
              "unknown component type is rejected");
    checkTrue(fails(R"json({"components": [{"type": "R", "nodes": ["a", "0"]}]})json"),
              "missing value is rejected");
    checkTrue(fails(R"json({"components": [{"type": "D", "nodes": ["a", "0"], "model": "nope"}]})json"),
              "undefined model is rejected");
    checkTrue(fails(R"json({"components": [{"type": "R", "nodes": ["a", "0"], "value": 1},)"),
              "JSON syntax error is rejected");
    checkTrue(fails(R"({"components": [{"type": "R", "name": "R1", "nodes": ["a", "0"], "value": 1},
                                       {"type": "R", "name": "R1", "nodes": ["a", "0"], "value": 1}]})json"),
              "duplicate names are rejected");
}

}  // namespace

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"units", testUnits},
        {"voltage divider", testVoltageDivider},
        {"controlled sources", testControlledSources},
        {"rc transient", testRcTransient},
        {"variable order", testVariableOrder},
        {"pulse breakpoints", testPulseBreakpoints},
        {"lc oscillator", testLcOscillator},
        {"diode", testDiode},
        {"bjt", testBjt},
        {"cmos inverter", testCmosInverter},
        {"ac", testAc},
        {"robertson dae", testRobertson},
        {"expressions", testExpressions},
        {"equation system", testEquationSystem},
        {"coupled circuit and equations", testCoupledSystem},
        {"behavioral sources", testBehavioralSources},
        {"events", testEvents},
        {"switches and ideal diodes", testSwitchingElements},
        {"subcircuits", testSubcircuits},
        {"coupled inductors", testCoupledInductors},
        {"tables", testTables},
        {"timed events", testTimedEvents},
        {"constrained mechanics", testConstrainedMechanics},
        {"device models", testDeviceModels},
        {"noise", testNoise},
        {"fourier", testFourier},
        {"parameter studies", testParameterStudies},
        {"steady state and sensitivity", testSteadyStateAndSensitivity},
        {"jfet, flux element, functions", testMoreDevices},
        {"delays and transmission lines", testDelaysAndLines},
        {"vectors", testVectors},
        {"structural index", testStructuralIndex},
        {"jacobian reuse and parallel runs", testSpeedFeatures},
        {"index reduction", testIndexReduction},
        {"parallel evaluation", testParallelEvaluation},
        {"bsim3", testBsim3},
        {"uniform output", testUniformOutput},
        {"parameter sweep", testParameterSweep},
        {"measurements", testMeasurements},
        {"input errors", testInputErrors},
    };
    for (const auto& [name, fn] : tests) {
        std::cout << "[test] " << name << '\n';
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::cout << "  FAIL exception: " << e.what() << '\n';
        }
    }
    std::cout << '\n' << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
