#include "app/Runner.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <fstream>
#include <cstdint>
#include <Eigen/Dense>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>

#include "app/Measure.hpp"
#include "circuit/Analysis.hpp"
#include "circuit/Device.hpp"
#include "circuit/devices/Sources.hpp"
#include "core/Transient.hpp"
#include "io/Output.hpp"

namespace esim {

using json = nlohmann::json;

namespace {

// A series of runs of one analysis (parameter sweep, Monte Carlo, fit): all
// runs write to shared tables whose leading columns identify the run.
struct Sweep {
    std::vector<std::string> columns;  // names of the leading columns
    std::vector<double> values;        // their values for the current run
    std::string path;                  // curves of all runs
    std::string measurePath;           // one row of measurements per run
    std::unique_ptr<CsvWriter> csv;
    std::unique_ptr<CsvWriter> measureCsv;
    // Measurements of all runs, for statistics.
    std::vector<std::string> measureNames;
    std::vector<std::vector<double>> measureValues;
    bool writeMeasures = true;  // false: only collect them (sensitivities)
    // Fit: the curve of one output is captured here instead of being measured.
    std::string capture;
    std::vector<double> captureX, captureY;
};

struct Context {
    Netlist& nl;
    const RunConfig& cfg;
    std::ostream& log;
    std::map<std::string, int> count;  // analyses run so far, per type
    Sweep* sweep = nullptr;

    Circuit& ckt() { return *nl.circuit; }

    // New entry of the summary for an analysis, or nullptr if none is wanted
    // (also inside a parameter sweep, which reports as a whole).
    json* entry(const json& a, const std::string& type) {
        if (!cfg.summary || sweep) return nullptr;
        cfg.summary->push_back({{"type", type}, {"name", a.value("name", type)}});
        return &cfg.summary->back();
    }
};

// NaN and infinity have no JSON representation: they become null.
json number(double v) { return std::isfinite(v) ? json(v) : json(nullptr); }

double num(Context& c, const json& a, const char* key, const std::string& where) {
    if (!a.contains(key)) throw std::runtime_error(where + ": missing \"" + key + "\"");
    return jsonNumber(a[key], where + ", \"" + key + "\"", &c.ckt().parameters);
}

double num(Context& c, const json& a, const char* key, const std::string& where, double def) {
    return a.contains(key) ? num(c, a, key, where) : def;
}

void checkKeys(const json& a, const std::string& where, std::initializer_list<const char*> keys,
               std::ostream& log) {
    for (const auto& item : a.items()) {
        const std::string& k = item.key();
        if (std::none_of(keys.begin(), keys.end(), [&](const char* key) { return k == key; }))
            log << "warning: " << where << ": unknown key \"" << k << "\" ignored\n";
    }
}

std::vector<Probe> probesFor(Context& c, const json& a) {
    std::vector<std::string> exprs = c.nl.outputs;
    if (a.contains("outputs")) exprs = a["outputs"].get<std::vector<std::string>>();
    return makeProbes(c.ckt(), exprs);
}

std::string outputPath(Context& c, const json& a, const std::string& type) {
    const int k = ++c.count[type];
    std::string tag = a.value("name", std::string());
    if (tag.empty()) tag = k == 1 ? type : type + std::to_string(k);
    std::filesystem::create_directories(c.cfg.outDir);
    return (std::filesystem::path(c.cfg.outDir) / (c.cfg.stem + "." + tag + ".csv")).string();
}

// Result table of one analysis: its own CSV file, or the shared file of a
// parameter sweep (then every row starts with the parameter value).
class Table {
public:
    Table(Context& c, const json& a, const std::string& type, std::vector<std::string> header) {
        if (!a.value("save", true)) {  // only the measurements are wanted
            if (!c.sweep) outputPath(c, a, type);  // keeps the numbering of the files stable
            return;
        }
        if (c.sweep) {
            if (!c.sweep->csv) {
                header.insert(header.begin(), c.sweep->columns.begin(), c.sweep->columns.end());
                c.sweep->csv = std::make_unique<CsvWriter>(c.sweep->path, header);
            }
            csv_ = c.sweep->csv.get();
            path_ = c.sweep->path;
            prefix_ = c.sweep->values;
        } else {
            path_ = outputPath(c, a, type);
            owned_ = std::make_unique<CsvWriter>(path_, header);
            csv_ = owned_.get();
        }
    }

    // Row consisting of a leading value (time, frequency, ...) and the probes.
    void row(double first, const std::vector<double>& rest) {
        if (!csv_) return;
        buffer_ = prefix_;
        buffer_.push_back(first);
        buffer_.insert(buffer_.end(), rest.begin(), rest.end());
        csv_->row(buffer_);
    }
    void row(const std::vector<double>& values) {
        if (!csv_) return;
        buffer_ = prefix_;
        buffer_.insert(buffer_.end(), values.begin(), values.end());
        csv_->row(buffer_);
    }

    // " -> file" for the log, or nothing if the curves are not saved.
    std::string arrow() const { return path_.empty() ? std::string() : " -> " + path_; }
    // For the summary: the file with the curves, or null.
    json file() const { return path_.empty() ? json(nullptr) : json(path_); }
    // Name the curve file has or would have (other results are stored next to it).
    std::string fileName(Context& c, const json& a) const {
        if (!path_.empty()) return path_;
        const std::string tag = a.value("name", a.value("type", std::string("tran")));
        return (std::filesystem::path(c.cfg.outDir) / (c.cfg.stem + "." + tag + ".csv")).string();
    }

private:
    CsvWriter* csv_ = nullptr;
    std::unique_ptr<CsvWriter> owned_;
    std::string path_;
    std::vector<double> prefix_, buffer_;
};

// The measurements of one analysis: collects the curves they need while the
// analysis runs and evaluates them at the end.
class Measures {
public:
    Measures(Context& c, const json& a) {
        if (a.contains("measure")) specs_ = parseMeasures(a["measure"], c.ckt().parameters);
        for (const MeasureSpec& spec : specs_) probes_.push_back(parseProbe(c.ckt(), spec.expression));
        if (c.sweep && !c.sweep->capture.empty()) {  // a fit wants this curve
            probes_.push_back(parseProbe(c.ckt(), c.sweep->capture));
            capturing_ = true;
        }
        y_.resize(probes_.size());
    }

    template <class ValueOf>
    void sample(double x, ValueOf&& valueOf) {
        if (probes_.empty()) return;
        x_.push_back(x);
        for (size_t k = 0; k < probes_.size(); ++k) y_[k].push_back(valueOf(probes_[k]));
    }

    void report(Context& c, json* entry = nullptr) {
        if (capturing_) {
            c.sweep->captureX = x_;
            c.sweep->captureY = y_.back();
        }
        if (specs_.empty()) return;
        std::vector<double> results;
        for (size_t k = 0; k < specs_.size(); ++k)
            results.push_back(evaluateMeasure(specs_[k], x_, y_[k]));
        if (entry)
            for (size_t k = 0; k < specs_.size(); ++k)
                (*entry)["measures"][specs_[k].name] = number(results[k]);

        if (c.sweep) {  // table: one row per run
            Sweep& sw = *c.sweep;
            if (sw.measureNames.empty()) {
                for (const MeasureSpec& spec : specs_) sw.measureNames.push_back(spec.name);
                sw.measureValues.resize(specs_.size());
            }
            for (size_t k = 0; k < results.size(); ++k) sw.measureValues[k].push_back(results[k]);
            if (!sw.writeMeasures) return;
            if (!sw.measureCsv) {
                std::vector<std::string> header = sw.columns;
                header.insert(header.end(), sw.measureNames.begin(), sw.measureNames.end());
                sw.measureCsv = std::make_unique<CsvWriter>(sw.measurePath, header);
            }
            results.insert(results.begin(), sw.values.begin(), sw.values.end());
            sw.measureCsv->row(results);
            return;
        }
        size_t width = 0;
        for (const MeasureSpec& spec : specs_) width = std::max(width, spec.name.size());
        for (size_t k = 0; k < specs_.size(); ++k) {
            c.log << "  " << std::left << std::setw(static_cast<int>(width)) << specs_[k].name
                  << " = ";
            if (std::isnan(results[k])) c.log << "not defined\n";
            else c.log << std::setprecision(6) << results[k] << '\n';
        }
    }

private:
    std::vector<MeasureSpec> specs_;
    std::vector<Probe> probes_;  // one per measurement, plus the captured curve
    bool capturing_ = false;
    std::vector<double> x_;
    std::vector<std::vector<double>> y_;
};

// "fourier": {"freq": "1k", "harmonics": 9, "periods": 1, "outputs": ["v(out)"]}
// Spectrum of the last whole period(s) of a transient analysis.
class FourierBlock {
public:
    FourierBlock(Context& c, const json& a) {
        if (!a.contains("fourier")) return;
        const json& f = a["fourier"];
        if (!f.is_object()) throw std::runtime_error("fourier: expected an object");
        checkKeys(f, "fourier", {"freq", "harmonics", "periods", "outputs", "from"}, c.log);
        freq_ = num(c, f, "freq", "fourier");
        if (!(freq_ > 0.0)) throw std::runtime_error("fourier: \"freq\" must be > 0");
        harmonics_ = static_cast<int>(num(c, f, "harmonics", "fourier", 9));
        periods_ = static_cast<int>(num(c, f, "periods", "fourier", 1));
        from_ = num(c, f, "from", "fourier", 0.0);
        if (harmonics_ < 1 || periods_ < 0)
            throw std::runtime_error("fourier: \"harmonics\" must be >= 1 and \"periods\" >= 0");
        std::vector<std::string> exprs = c.nl.outputs;
        if (a.contains("outputs")) exprs = a["outputs"].get<std::vector<std::string>>();
        if (f.contains("outputs")) exprs = f["outputs"].get<std::vector<std::string>>();
        probes_ = makeProbes(c.ckt(), exprs);
        y_.resize(probes_.size());
        active_ = true;
    }

    void sample(double t, const Vector& x) {
        if (!active_) return;
        t_.push_back(t);
        for (size_t k = 0; k < probes_.size(); ++k) y_[k].push_back(probes_[k].value(x, t));
    }

    void report(Context& c, const std::string& curveFile, json* entry) {
        if (!active_ || c.sweep) return;
        std::vector<std::vector<std::complex<double>>> amp(probes_.size());
        for (size_t k = 0; k < probes_.size(); ++k)
            if (!fourier(t_, y_[k], freq_, harmonics_, periods_, from_, amp[k]))
                throw std::runtime_error("fourier: the analysis is shorter than one period of the "
                                         "fundamental frequency");
        std::vector<std::string> header{"harmonic", "freq"};
        for (const Probe& p : probes_) {
            header.push_back("mag(" + p.label + ")");
            header.push_back("phase(" + p.label + ")");
        }
        // Next to the curves: <stem>.<name>.fourier.csv
        const std::string path = curveFile.substr(0, curveFile.size() - 4) + ".fourier.csv";
        CsvWriter csv(path, header);
        for (int h = 0; h <= harmonics_; ++h) {
            std::vector<double> row{static_cast<double>(h), h * freq_};
            for (const auto& a : amp) {
                row.push_back(std::abs(a[h]));
                row.push_back(std::arg(a[h]) * 180.0 / kPi);
            }
            csv.row(row);
        }
        c.log << "  fourier, fundamental " << freq_ << " Hz -> " << path << '\n';
        size_t width = 0;
        for (const Probe& p : probes_) width = std::max(width, p.label.size());
        for (size_t k = 0; k < probes_.size(); ++k) {
            const double thd = totalHarmonicDistortion(amp[k]);
            c.log << "    " << std::left << std::setw(static_cast<int>(width)) << probes_[k].label
                  << "  fundamental " << std::setprecision(6) << std::abs(amp[k][1]) << ", thd "
                  << 100.0 * thd << " %\n";
            if (entry) {
                (*entry)["fourier"]["thd"][probes_[k].label] = number(thd);
                (*entry)["fourier"]["fundamental"][probes_[k].label] = number(std::abs(amp[k][1]));
            }
        }
        if (entry) (*entry)["fourier"]["file"] = path;
    }

private:
    bool active_ = false;
    double freq_ = 0.0, from_ = 0.0;
    int harmonics_ = 9, periods_ = 1;
    std::vector<Probe> probes_;
    std::vector<double> t_;
    std::vector<std::vector<double>> y_;
};

std::vector<std::string> labels(const std::vector<Probe>& probes) {
    std::vector<std::string> out;
    for (const Probe& p : probes) out.push_back(p.label);
    return out;
}

void runOp(Context& c, const json& a) {
    checkKeys(a, "op", {"type", "name", "outputs"}, c.log);
    c.ckt().setTransientMode(false);
    DcResult info;
    const Vector x = solveOperatingPoint(c.ckt(), c.nl.options, nullptr, &info);
    const auto probes = probesFor(c, a);

    if (c.sweep) {  // one row per parameter value instead of the console table
        Table table(c, a, "op", labels(probes));
        std::vector<double> row;
        for (const Probe& p : probes) row.push_back(p.value(x));
        table.row(row);
        // The outputs are the quantities a study of the operating point looks at.
        Sweep& sw = *c.sweep;
        if (sw.measureNames.empty()) {
            sw.measureNames = labels(probes);
            sw.measureValues.resize(probes.size());
        }
        for (size_t k = 0; k < row.size(); ++k) sw.measureValues[k].push_back(row[k]);
        return;
    }
    c.log << "[op] converged after " << info.iterations << " iterations (" << info.strategy
          << ")\n";
    if (json* e = c.entry(a, "op")) {
        (*e)["iterations"] = info.iterations;
        (*e)["strategy"] = info.strategy;
        for (const Probe& p : probes) (*e)["values"][p.label] = number(p.value(x));
    }
    size_t width = 0;
    for (const Probe& p : probes) width = std::max(width, p.label.size());
    for (const Probe& p : probes)
        c.log << "  " << std::left << std::setw(static_cast<int>(width)) << p.label << " = "
              << std::setprecision(6) << p.value(x) << '\n';
}

void runDcSweep(Context& c, const json& a) {
    checkKeys(a, "dc",
              {"type", "name", "outputs", "source", "start", "stop", "step", "measure", "save"},
              c.log);
    if (!a.contains("source") || !a["source"].is_string())
        throw std::runtime_error("dc: missing \"source\" (name of a V or I source)");
    const std::string srcName = a["source"].get<std::string>();
    auto* src = dynamic_cast<IndependentSource*>(c.ckt().findDevice(srcName));
    if (!src) throw std::runtime_error("dc: '" + srcName + "' is not an independent source");

    const double start = num(c, a, "start", "dc"), stop = num(c, a, "stop", "dc");
    const double step = num(c, a, "step", "dc");
    if (step == 0.0 || (stop - start) / step < 0.0)
        throw std::runtime_error("dc: step has the wrong sign or is zero");
    const int points = static_cast<int>(std::floor((stop - start) / step + 1e-9)) + 1;

    const auto probes = probesFor(c, a);
    std::vector<std::string> header{srcName};
    for (const Probe& p : probes) header.push_back(p.label);
    Table table(c, a, "dc", header);
    Measures measures(c, a);

    c.ckt().setTransientMode(false);
    const auto saved = src->dcState();
    int iterations = 0;
    try {
        Vector x = c.ckt().initialGuess();
        std::vector<double> row;
        for (int k = 0; k < points; ++k) {
            const double value = start + k * step;
            src->setDc(value);
            DcResult info;
            x = solveOperatingPoint(c.ckt(), c.nl.options, &x, &info);  // continuation
            iterations += info.iterations;
            row.clear();
            for (const Probe& p : probes) row.push_back(p.value(x));
            table.row(value, row);
            measures.sample(value, [&](const Probe& p) { return p.value(x); });
        }
    } catch (...) {
        src->setDcState(saved);
        throw;
    }
    src->setDcState(saved);
    c.log << "[dc] " << srcName << " = " << start << " .. " << stop << ": " << points
          << " points, " << iterations << " Newton iterations" << table.arrow() << '\n';
    json* e = c.entry(a, "dc");
    if (e) {
        (*e)["file"] = table.file();
        (*e)["source"] = srcName;
    }
    measures.report(c, e);
}

IntegrationMethod parseMethod(const std::string& m) {
    if (m == "trap" || m == "trapezoidal") return IntegrationMethod::Trapezoidal;
    if (m == "gear2" || m == "gear" || m == "bdf2") return IntegrationMethod::Bdf2;
    if (m == "bdf") return IntegrationMethod::Bdf;
    if (m == "be" || m == "euler") return IntegrationMethod::BackwardEuler;
    throw std::runtime_error("tran: unknown method \"" + m + "\" (gear2, bdf, trap, be)");
}

// Options of a time integration over tstop, from the keys shared by "tran" and "pss".
TransientOptions transientOptions(Context& c, const json& a, const char* where, double tstop) {
    const SimOptions& so = c.nl.options;
    TransientOptions to;
    to.tstop = tstop;
    // tstep is the resolution the user wants to see: no step is longer.
    const double tstep = num(c, a, "tstep", where, 0.0);
    to.hmax = num(c, a, "hmax", where, tstep > 0.0 ? std::min(tstep, to.tstop / 50.0) : 0.0);
    to.hmin = num(c, a, "hmin", where, 0.0);
    to.hinit = num(c, a, "hinit", where, 0.0);
    to.method = parseMethod(a.value("method", std::string("bdf")));
    to.outputStep = num(c, a, "output_step", where, 0.0);
    to.maxEvents = static_cast<int>(num(c, a, "max_events", where, to.maxEvents));
    to.maxOrder = static_cast<int>(num(c, a, "max_order", where, to.maxOrder));
    to.reltol = so.reltol;
    to.trtol = so.trtol;
    to.peakFloor = so.lteFloor;
    to.newton.maxIter = so.maxIterTran;
    to.newton.reltol = so.reltol;
    return to;
}

// State at t = 0: the operating point with prescribed states held, or the
// initial conditions ("uic"); q0 is set in the second case only.
Vector initialPoint(Context& c, bool uic, Vector& q0) {
    Circuit& ckt = c.ckt();
    Vector x0;
    if (uic) {
        ckt.initialState(x0, q0);
    } else {
        ckt.setInitialPointMode(true);
        try {
            x0 = solveOperatingPoint(ckt, c.nl.options);
        } catch (...) {
            ckt.setInitialPointMode(false);
            throw;
        }
        ckt.setInitialPointMode(false);
    }
    return x0;
}

void runTran(Context& c, const json& a) {
    checkKeys(a, "tran",
              {"type", "name", "outputs", "tstop", "tstep", "hmax", "hmin", "hinit", "method",
               "uic", "output_step", "max_events", "max_order", "measure", "save", "fourier"},
              c.log);
    const double tstop = num(c, a, "tstop", "tran");
    if (!(tstop > 0.0)) throw std::runtime_error("tran: tstop must be > 0");
    const TransientOptions to = transientOptions(c, a, "tran", tstop);
    const bool uic = a.value("uic", false);

    const auto probes = probesFor(c, a);
    std::vector<std::string> header{"time"};
    for (const Probe& p : probes) header.push_back(p.label);
    Table table(c, a, "tran", header);
    Measures measures(c, a);
    FourierBlock fourierBlock(c, a);

    Circuit& ckt = c.ckt();
    ckt.setTransientMode(true);
    TransientStats stats;
    try {
        Vector q0;
        const Vector x0 = initialPoint(c, uic, q0);
        std::vector<double> row;
        stats = runTransient(ckt, x0, uic ? &q0 : nullptr, to, [&](double t, const Vector& x) {
            row.clear();
            for (const Probe& p : probes) row.push_back(p.value(x, t));
            table.row(t, row);
            measures.sample(t, [&](const Probe& p) { return p.value(x, t); });
            fourierBlock.sample(t, x);
        });
    } catch (...) {
        ckt.setTransientMode(false);
        throw;
    }
    ckt.setTransientMode(false);
    c.log << "[tran] 0 .. " << to.tstop << " s, " << methodName(to.method);
    if (to.method == IntegrationMethod::Bdf) c.log << " (order up to " << stats.maxOrder << ")";
    c.log << (uic ? ", uic" : "") << ": " << stats.accepted << " steps (" << stats.rejected
          << " rejected, " << stats.newtonFailures << " Newton failures), "
          << stats.newtonIterations << " Newton iterations";
    if (stats.events > 0) c.log << ", " << stats.events << " events";
    c.log << table.arrow() << '\n';
    json* e = c.entry(a, "tran");
    if (e) {
        (*e)["file"] = table.file();
        (*e)["stats"] = {{"steps", stats.accepted},
                         {"rejected", stats.rejected},
                         {"newton_failures", stats.newtonFailures},
                         {"newton_iterations", stats.newtonIterations},
                         {"events", stats.events},
                         {"max_order", stats.maxOrder}};
    }
    measures.report(c, e);
    fourierBlock.report(c, table.fileName(c, a), e);
}

// {"type": "pss", "period": "10u", "tstab": 0, "periods": 1, "tol": 1e-4, ...}
// Periodic steady state of a circuit driven with a known period. Looks for a
// state x with x = P(x), where P integrates over one period ("shooting"). The
// fixed point is found by Anderson acceleration, which uses the last few
// results of P to extrapolate; for a circuit whose slow parts are nearly
// linear this needs about as many periods as there are slow modes, instead of
// waiting for them to die out.
void runPss(Context& c, const json& a) {
    checkKeys(a, "pss",
              {"type", "name", "outputs", "period", "tstab", "periods", "max_iterations", "tol",
               "tstep", "hmax", "hmin", "hinit", "method", "uic", "output_step", "max_events",
               "max_order", "measure", "save", "fourier"},
              c.log);
    const double period = num(c, a, "period", "pss");
    if (!(period > 0.0)) throw std::runtime_error("pss: \"period\" must be > 0");
    const int outPeriods = std::max(1, static_cast<int>(num(c, a, "periods", "pss", 1)));
    const int maxIterations = static_cast<int>(num(c, a, "max_iterations", "pss", 60));
    const double tol = num(c, a, "tol", "pss", 1e-4);
    const double tstab = num(c, a, "tstab", "pss", 0.0);
    const TransientOptions to = transientOptions(c, a, "pss", period);
    const bool uic = a.value("uic", false);

    const auto probes = probesFor(c, a);
    std::vector<std::string> header{"time"};
    for (const Probe& p : probes) header.push_back(p.label);
    Table table(c, a, "pss", header);
    Measures measures(c, a);
    FourierBlock fourierBlock(c, a);

    Circuit& ckt = c.ckt();
    const int n = ckt.size();
    ckt.setTransientMode(true);
    int simulated = 0, iterations = 0;
    long steps = 0;
    double residual = 0.0;
    bool converged = false;
    try {
        Evaluation ev;
        // Integrate from the state xs over `span`; returns the final state.
        auto advance = [&](const Vector& xs, double span, const StepCallback& onStep) {
            TransientOptions o = to;
            o.tstop = span;
            if (!onStep) o.outputStep = 0.0;
            ckt.evaluate(0.0, xs, LimitMode::Off, ev);
            const Vector qs = ev.q;  // prescribing the charges makes the solver settle the rest
            Vector last = xs;
            const TransientStats st = runTransient(ckt, xs, &qs, o, [&](double t, const Vector& xx) {
                last = xx;
                if (onStep) onStep(t, xx);
            });
            steps += st.accepted;
            return last;
        };

        Vector q0;
        Vector x = initialPoint(c, uic, q0);
        for (int k = 0; k < static_cast<int>(std::ceil(tstab / period - 1e-9)); ++k, ++simulated)
            x = advance(x, period, nullptr);

        // The continuous states: unknowns a charge depends on, without discrete ones.
        ckt.evaluate(0.0, x, LimitMode::Off, ev);
        std::vector<int> state;
        {
            std::vector<char> mark(n, 0);
            for (const Triplet& t : ev.C)
                if (t.value() != 0.0) mark[t.col()] = 1;
            for (int i = 0; i < n; ++i)
                if (mark[i] && !ckt.isDiscrete(i)) state.push_back(i);
        }
        const auto m = static_cast<Eigen::Index>(state.size());

        // Fixed-point iteration x = P(x) with Anderson acceleration. f = P(x) - x
        // alone does not tell how far the steady state is: a slow mode changes
        // little per period however far it still has to go. The extrapolation
        // step estimates the remaining distance, so both have to be small.
        // No more secants than states: with fewer states the newest ones decide.
        const int depth = static_cast<int>(std::min<Eigen::Index>(6, std::max<Eigen::Index>(m, 1)));
        std::vector<Eigen::VectorXd> histF, histG;  // f and P(x) of the last iterations, newest last
        Eigen::VectorXd f(m), gm(m), w(m);
        double best = std::numeric_limits<double>::infinity();
        for (; iterations < maxIterations && !converged;) {
            const Vector g = advance(x, period, nullptr);
            ++simulated;
            ++iterations;
            for (Eigen::Index k = 0; k < m; ++k) {
                const int i = state[static_cast<size_t>(k)];
                w[k] = 1.0 / (tol * std::max(std::abs(g[i]), std::abs(x[i])) + ckt.absTol(i));
                f[k] = g[i] - x[i];
                gm[k] = g[i];
            }
            const double change = m > 0 ? f.cwiseProduct(w).cwiseAbs().maxCoeff() : 0.0;
            if (change > 10.0 * best) {  // the extrapolation went astray: start afresh
                histF.clear();
                histG.clear();
            }
            best = std::min(best, change);
            histF.push_back(f);
            histG.push_back(gm);
            if (static_cast<int>(histF.size()) > depth + 1) {
                histF.erase(histF.begin());
                histG.erase(histG.begin());
            }
            // Anderson step: x_next = g - dG * gamma with gamma = argmin |f - dF * gamma|.
            const auto cols = static_cast<Eigen::Index>(histF.size()) - 1;
            Eigen::VectorXd next = gm;
            if (cols > 0 && m > 0) {
                Eigen::MatrixXd dF(m, cols), dG(m, cols);
                for (Eigen::Index j = 0; j < cols; ++j) {
                    dF.col(j) = histF[static_cast<size_t>(j + 1)] - histF[static_cast<size_t>(j)];
                    dG.col(j) = histG[static_cast<size_t>(j + 1)] - histG[static_cast<size_t>(j)];
                }
                const Eigen::VectorXd gamma =
                    (w.asDiagonal() * dF).colPivHouseholderQr().solve(f.cwiseProduct(w));
                next = gm - dG * gamma;
                if (!next.allFinite()) next = gm;
            }
            const double ahead = m > 0 ? (next - gm).cwiseProduct(w).cwiseAbs().maxCoeff() : 0.0;
            residual = std::max(change, ahead);
            x = g;
            if (m == 0 || (cols > 0 && residual <= 1.0)) {
                converged = true;
                break;
            }
            for (Eigen::Index k = 0; k < m; ++k) x[state[static_cast<size_t>(k)]] = next[k];
        }
        if (!converged) {
            std::ostringstream os;
            os << "pss: no periodic steady state after " << iterations
               << " iterations (the change over one period is still " << residual
               << " times the tolerance). Is \"period\" the period of all sources?";
            throw std::runtime_error(os.str());
        }

        // The iteration restarts the time in every period, so it cannot notice
        // sources with another period: compare them one period apart.
        {
            Evaluation later;
            for (double phase : {0.0, 0.31, 0.73}) {
                ckt.evaluate(phase * period, x, LimitMode::Off, ev);
                ckt.evaluate((phase + 1.0) * period, x, LimitMode::Off, later);
                const double scale = std::max(ev.f.cwiseAbs().maxCoeff(), later.f.cwiseAbs().maxCoeff());
                if ((ev.f - later.f).cwiseAbs().maxCoeff() > 1e-6 * scale)
                    throw std::runtime_error("pss: the sources do not repeat after \"period\" = " +
                                             std::to_string(period) + " s");
            }
        }

        // The result: one or more periods from the steady state.
        std::vector<double> row;
        advance(x, outPeriods * period, [&](double t, const Vector& xx) {
            row.clear();
            for (const Probe& p : probes) row.push_back(p.value(xx, t));
            table.row(t, row);
            measures.sample(t, [&](const Probe& p) { return p.value(xx, t); });
            fourierBlock.sample(t, xx);
        });
    } catch (...) {
        ckt.setTransientMode(false);
        throw;
    }
    ckt.setTransientMode(false);
    c.log << "[pss] period " << period << " s: steady state after " << iterations
          << " iterations (" << simulated << " periods simulated, " << steps << " steps)"
          << table.arrow() << '\n';
    json* e = c.entry(a, "pss");
    if (e) {
        (*e)["file"] = table.file();
        (*e)["iterations"] = iterations;
        (*e)["periods_simulated"] = simulated;
    }
    measures.report(c, e);
    fourierBlock.report(c, table.fileName(c, a), e);
}

void runAcSweep(Context& c, const json& a) {
    checkKeys(a, "ac",
              {"type", "name", "outputs", "sweep", "points", "fstart", "fstop", "measure", "save"},
              c.log);
    const auto freqs =
        frequencySweep(a.value("sweep", std::string("dec")), a.value("points", 10),
                       num(c, a, "fstart", "ac"), num(c, a, "fstop", "ac"));

    const auto probes = probesFor(c, a);
    std::vector<std::string> header{"freq"};
    for (const Probe& p : probes) {
        header.push_back("mag(" + p.label + ")");
        header.push_back("phase(" + p.label + ")");
    }
    Table table(c, a, "ac", header);
    Measures measures(c, a);  // of the magnitude

    c.ckt().setTransientMode(false);
    const Vector xop = solveOperatingPoint(c.ckt(), c.nl.options);
    std::vector<double> row;
    runAc(c.ckt(), xop, freqs, [&](double f, const Eigen::VectorXcd& x) {
        row.clear();
        for (const Probe& p : probes) {
            const std::complex<double> v = p.phasor(x);
            row.push_back(std::abs(v));
            row.push_back(std::arg(v) * 180.0 / kPi);
        }
        table.row(f, row);
        measures.sample(f, [&](const Probe& p) { return std::abs(p.phasor(x)); });
    });
    c.log << "[ac] " << freqs.front() << " .. " << freqs.back() << " Hz: " << freqs.size()
          << " points" << table.arrow() << '\n';
    json* e = c.entry(a, "ac");
    if (e) (*e)["file"] = table.file();
    measures.report(c, e);
}

// {"type": "noise", "output": "v(out)", "input": "Vin", "sweep": "dec", "points": 10,
//  "fstart": 10, "fstop": "100k"}
void runNoiseAnalysis(Context& c, const json& a) {
    checkKeys(a, "noise",
              {"type", "name", "output", "input", "sweep", "points", "fstart", "fstop", "save"},
              c.log);
    if (!a.contains("output") || !a["output"].is_string())
        throw std::runtime_error("noise: needs \"output\", e.g. \"v(out)\" or \"v(a,b)\"");
    const Probe out = parseProbe(c.ckt(), a["output"].get<std::string>());
    if (out.expression)
        throw std::runtime_error("noise: \"output\" must be v(node), v(a,b) or i(component)");
    const IndependentSource* input = nullptr;
    if (a.contains("input")) {
        const std::string name = a["input"].get<std::string>();
        input = dynamic_cast<const IndependentSource*>(c.ckt().findDevice(name));
        if (!input) throw std::runtime_error("noise: input '" + name + "' is not an independent source");
    }
    const auto freqs =
        frequencySweep(a.value("sweep", std::string("dec")), a.value("points", 10),
                       num(c, a, "fstart", "noise"), num(c, a, "fstop", "noise"));

    // Densities are reported as square roots (V/sqrt(Hz) for a voltage output).
    std::vector<std::string> header{"freq", "onoise"};
    if (input) {
        header.push_back("inoise");
        header.push_back("gain");
    }
    Table table(c, a, "noise", header);

    c.ckt().setTransientMode(false);
    const Vector xop = solveOperatingPoint(c.ckt(), c.nl.options);
    std::vector<NoiseSource> sources;
    std::vector<double> integrated;  // per source, V^2 over the band
    double total = 0.0, totalIn = 0.0, prevF = 0.0, prevOn = 0.0, prevIn = 0.0;
    std::vector<double> prev;
    bool first = true;
    std::vector<double> row;
    runNoise(c.ckt(), xop, freqs, out.pos, out.neg, input, sources,
             [&](double f, double onoise2, double gain, const std::vector<double>& contrib) {
                 const double inoise2 = gain > 0.0 ? onoise2 / (gain * gain) : 0.0;
                 row = {std::sqrt(onoise2)};
                 if (input) {
                     row.push_back(std::sqrt(inoise2));
                     row.push_back(gain);
                 }
                 table.row(f, row);
                 if (first) integrated.assign(contrib.size(), 0.0);
                 else {  // trapezoidal rule over the frequency
                     const double df = f - prevF;
                     total += 0.5 * df * (onoise2 + prevOn);
                     totalIn += 0.5 * df * (inoise2 + prevIn);
                     for (size_t k = 0; k < contrib.size(); ++k)
                         integrated[k] += 0.5 * df * (contrib[k] + prev[k]);
                 }
                 first = false;
                 prevF = f;
                 prevOn = onoise2;
                 prevIn = inoise2;
                 prev = contrib;
             });

    c.log << "[noise] " << freqs.front() << " .. " << freqs.back() << " Hz: " << freqs.size()
          << " points" << table.arrow() << '\n';
    c.log << "  output noise in the band: " << std::setprecision(6) << std::sqrt(total) << " rms\n";
    if (input) c.log << "  referred to the input:    " << std::sqrt(totalIn) << " rms\n";
    // Largest contributors
    std::vector<size_t> order(sources.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t i, size_t j) { return integrated[i] > integrated[j]; });
    json* e = c.entry(a, "noise");
    if (e) {
        (*e)["file"] = table.file();
        (*e)["onoise_rms"] = number(std::sqrt(total));
        if (input) (*e)["inoise_rms"] = number(std::sqrt(totalIn));
    }
    for (size_t r = 0; r < order.size(); ++r) {
        const size_t k = order[r];
        if (r < 5 && total > 0.0)
            c.log << "    " << std::left << std::setw(12) << sources[k].name << ' '
                  << std::setprecision(4) << 100.0 * integrated[k] / total << " %\n";
        if (e) (*e)["contributions"][sources[k].name] = number(std::sqrt(integrated[k]));
    }
}

void runOne(Context& c, const json& a, const std::string& type) {
    c.ckt().resetDevices();
    if (type == "op") runOp(c, a);
    else if (type == "dc") runDcSweep(c, a);
    else if (type == "tran") runTran(c, a);
    else if (type == "ac") runAcSweep(c, a);
    else if (type == "noise") runNoiseAnalysis(c, a);
    else if (type == "pss") runPss(c, a);
    else
        throw std::runtime_error("analyses: unknown type \"" + type +
                                 "\" (op, dc, tran, pss, ac, noise, fit, sens)");
}

void runOne(Context& c, const json& a, const std::string& type);

// One series of values of "param_sweep":
//   {"param": "R1", "values": [...]}
//   {"param": "R1", "start": 1e3, "stop": 1e5, "points": 5, "scale": "log"}
std::vector<double> sweepValues(Context& c, const json& spec) {
    const std::string where = "param_sweep";
    checkKeys(spec, where, {"param", "values", "start", "stop", "points", "scale"}, c.log);
    std::vector<double> values;
    if (spec.contains("values")) {
        if (!spec["values"].is_array()) throw std::runtime_error(where + ": \"values\" must be an array");
        for (const json& v : spec["values"])
            values.push_back(jsonNumber(v, where + ", \"values\"", &c.ckt().parameters));
    } else {
        const double start = num(c, spec, "start", where), stop = num(c, spec, "stop", where);
        const int points = static_cast<int>(num(c, spec, "points", where));
        const std::string scale = spec.value("scale", std::string("lin"));
        if (points < 2) throw std::runtime_error(where + ": \"points\" must be at least 2");
        if (scale != "lin" && scale != "log")
            throw std::runtime_error(where + ": \"scale\" must be \"lin\" or \"log\"");
        if (scale == "log" && !(start > 0.0 && stop > 0.0))
            throw std::runtime_error(where + ": a logarithmic sweep needs positive limits");
        for (int k = 0; k < points; ++k) {
            const double s = static_cast<double>(k) / (points - 1);
            values.push_back(scale == "log" ? start * std::pow(stop / start, s)
                                            : start + s * (stop - start));
        }
    }
    if (values.empty()) throw std::runtime_error(where + ": no values");
    return values;
}

const std::string& definedParameter(Context& c, const std::string& name, const char* where) {
    if (!c.ckt().parameters.count(name))
        throw std::runtime_error(std::string(where) + ": '" + name +
                                 "' is not defined under \"parameters\"");
    return name;
}

// One run of an analysis with changed parameters. Parameters are resolved
// while the input is read, so every run gets a freshly built system.
void runWith(Context& c, Sweep& sweep, const json& inner, const std::string& type,
             const std::vector<std::pair<std::string, double>>& parameters) {
    json root = c.nl.source;
    for (const auto& [name, value] : parameters) root["parameters"][name] = value;
    Netlist sub = parseNetlist(root, /*warnings=*/false, c.nl.baseDir);
    std::ostringstream quiet;  // per-run progress is not reported
    Context sc{sub, c.cfg, quiet, {}, &sweep};  // sc.entry() stays silent inside a series
    runOne(sc, inner, type);
}

std::string describe(const std::vector<std::pair<std::string, double>>& parameters) {
    std::ostringstream os;
    for (size_t k = 0; k < parameters.size(); ++k)
        os << (k ? ", " : "") << parameters[k].first << " = " << parameters[k].second;
    return os.str();
}

// "param_sweep": one sweep, or a list of sweeps that are nested (the first
// one is the outermost).
void runParamSweep(Context& c, const json& a, const std::string& type) {
    const json specs = a["param_sweep"].is_array() ? a["param_sweep"] : json::array({a["param_sweep"]});
    Sweep sweep;
    std::vector<std::vector<double>> axes;
    for (const json& spec : specs) {
        if (!spec.is_object() || !spec.contains("param") || !spec["param"].is_string())
            throw std::runtime_error("param_sweep: needs \"param\": \"<parameter name>\"");
        sweep.columns.push_back(definedParameter(c, spec["param"].get<std::string>(), "param_sweep"));
        axes.push_back(sweepValues(c, spec));
    }
    if (axes.empty()) throw std::runtime_error("param_sweep: no sweep given");
    sweep.path = outputPath(c, a, type);
    sweep.measurePath = sweep.path.substr(0, sweep.path.size() - 4) + ".measure.csv";

    json inner = a;
    inner.erase("param_sweep");
    size_t runs = 0;
    std::vector<size_t> index(axes.size(), 0);
    for (bool done = false; !done;) {
        std::vector<std::pair<std::string, double>> parameters;
        sweep.values.clear();
        for (size_t k = 0; k < axes.size(); ++k) {
            parameters.emplace_back(sweep.columns[k], axes[k][index[k]]);
            sweep.values.push_back(axes[k][index[k]]);
        }
        try {
            runWith(c, sweep, inner, type, parameters);
        } catch (const std::exception& e) {
            throw std::runtime_error("param_sweep, " + describe(parameters) + ": " + e.what());
        }
        ++runs;
        // Advance like an odometer; the last parameter runs fastest.
        done = true;
        for (size_t k = axes.size(); k-- > 0;) {
            if (++index[k] < axes[k].size()) {
                done = false;
                break;
            }
            index[k] = 0;
        }
    }

    c.log << "[" << type << "] sweep of";
    for (size_t k = 0; k < axes.size(); ++k)
        c.log << (k ? "," : "") << ' ' << sweep.columns[k] << " = " << axes[k].front() << " .. "
              << axes[k].back();
    c.log << ": " << runs << " runs";
    if (sweep.csv) c.log << " -> " << sweep.path;
    if (sweep.measureCsv) c.log << " -> " << sweep.measurePath;
    c.log << '\n';
    if (json* e = c.entry(a, type)) {
        if (axes.size() == 1) (*e)["sweep"] = {{"param", sweep.columns[0]}, {"values", axes[0]}};
        else (*e)["sweep"] = {{"params", sweep.columns}, {"grid", axes}};
        (*e)["file"] = sweep.csv ? json(sweep.path) : json(nullptr);
        (*e)["measure_file"] = sweep.measureCsv ? json(sweep.measurePath) : json(nullptr);
    }
}

// "monte_carlo": {"runs": 200, "seed": 1,
//                 "vary": {"R": {"tol": 0.05}, "C": {"tol": 0.1, "dist": "gauss"}, "V": {"sigma": 0.01}}}
// Repeats the analysis with randomly varied parameters and reports the
// statistics of the measurements.
//   tol    relative tolerance: uniform within +-tol, or with "dist": "gauss"
//          normal with standard deviation tol/3
//   sigma  absolute standard deviation of a normal distribution
void runMonteCarlo(Context& c, const json& a, const std::string& type) {
    const json& mc = a["monte_carlo"];
    if (!mc.is_object() || !mc.contains("vary") || !mc["vary"].is_object() || mc["vary"].empty())
        throw std::runtime_error("monte_carlo: needs \"vary\": {\"parameter\": {\"tol\": 0.05}, ...}");
    checkKeys(mc, "monte_carlo", {"runs", "seed", "vary"}, c.log);
    const int runs = static_cast<int>(num(c, mc, "runs", "monte_carlo", 100));
    if (runs < 1) throw std::runtime_error("monte_carlo: \"runs\" must be at least 1");

    struct Varied {
        std::string name;
        double nominal, spread;
        bool gauss;
    };
    std::vector<Varied> varied;
    Sweep sweep;
    sweep.columns = {"run"};
    for (const auto& [name, v] : mc["vary"].items()) {
        const std::string where = "monte_carlo, " + name;
        definedParameter(c, name, "monte_carlo");
        if (!v.is_object()) throw std::runtime_error(where + ": expected {\"tol\": ...} or {\"sigma\": ...}");
        checkKeys(v, where, {"tol", "sigma", "dist"}, c.log);
        Varied p{name, c.ckt().parameters.at(name), 0.0, false};
        if (v.contains("sigma") == v.contains("tol"))
            throw std::runtime_error(where + ": give either \"tol\" or \"sigma\"");
        const std::string dist = v.value("dist", std::string(v.contains("sigma") ? "gauss" : "uniform"));
        if (dist != "gauss" && dist != "uniform")
            throw std::runtime_error(where + ": \"dist\" must be \"uniform\" or \"gauss\"");
        p.gauss = dist == "gauss";
        if (v.contains("sigma")) {
            if (!p.gauss) throw std::runtime_error(where + ": \"sigma\" belongs to \"dist\": \"gauss\"");
            p.spread = num(c, v, "sigma", where);
        } else {
            const double tol = num(c, v, "tol", where);
            p.spread = std::abs(p.nominal) * (p.gauss ? tol / 3.0 : tol);
        }
        varied.push_back(p);
        sweep.columns.push_back(name);
    }

    // Own generator and transformation, so that a seed gives the same numbers everywhere.
    std::mt19937_64 rng(static_cast<std::uint64_t>(num(c, mc, "seed", "monte_carlo", 1)));
    auto uniform = [&]() { return (static_cast<double>(rng() >> 11) + 0.5) / 9007199254740992.0; };
    auto normal = [&]() { return std::sqrt(-2.0 * std::log(uniform())) * std::cos(2.0 * kPi * uniform()); };

    sweep.path = outputPath(c, a, type);
    sweep.measurePath = sweep.path.substr(0, sweep.path.size() - 4) + ".measure.csv";
    json inner = a;
    inner.erase("monte_carlo");
    inner["save"] = a.value("save", false);  // the curves of all runs only on request
    if (!a.contains("measure"))
        throw std::runtime_error("monte_carlo: the analysis needs \"measure\" (the statistics are "
                                 "taken over the measurements)");
    for (int run = 0; run < runs; ++run) {
        std::vector<std::pair<std::string, double>> parameters;
        sweep.values = {static_cast<double>(run)};
        for (const Varied& p : varied) {
            const double value = p.nominal + p.spread * (p.gauss ? normal() : 2.0 * uniform() - 1.0);
            parameters.emplace_back(p.name, value);
            sweep.values.push_back(value);
        }
        try {
            runWith(c, sweep, inner, type, parameters);
        } catch (const std::exception& e) {
            throw std::runtime_error("monte_carlo, run " + std::to_string(run) + " (" +
                                     describe(parameters) + "): " + e.what());
        }
    }

    c.log << "[" << type << "] monte carlo: " << runs << " runs -> " << sweep.measurePath << '\n';
    json* e = c.entry(a, type);
    if (e) {
        std::vector<double> index(runs);
        for (int k = 0; k < runs; ++k) index[k] = k;
        (*e)["sweep"] = {{"param", "run"}, {"values", index}};
        (*e)["file"] = sweep.csv ? json(sweep.path) : json(nullptr);
        (*e)["measure_file"] = sweep.measurePath;
    }
    size_t width = 0;
    for (const std::string& n : sweep.measureNames) width = std::max(width, n.size());
    for (size_t k = 0; k < sweep.measureNames.size(); ++k) {
        double sum = 0.0, sum2 = 0.0, lo = 0.0, hi = 0.0;
        int count = 0;
        for (double v : sweep.measureValues[k]) {
            if (std::isnan(v)) continue;
            lo = count ? std::min(lo, v) : v;
            hi = count ? std::max(hi, v) : v;
            sum += v;
            sum2 += v * v;
            ++count;
        }
        const double mean = count ? sum / count : 0.0;
        const double sdev = count > 1 ? std::sqrt(std::max(0.0, (sum2 - count * mean * mean) / (count - 1))) : 0.0;
        c.log << "  " << std::left << std::setw(static_cast<int>(width)) << sweep.measureNames[k]
              << "  mean " << std::setprecision(6) << mean << "  std " << sdev << "  min " << lo
              << "  max " << hi;
        if (count < runs) c.log << "  (" << runs - count << " runs without result)";
        c.log << '\n';
        if (e)
            (*e)["statistics"][sweep.measureNames[k]] = {
                {"mean", number(mean)}, {"std", number(sdev)}, {"min", number(lo)},
                {"max", number(hi)},    {"count", count}};
    }
}

// Linear interpolation of a sampled curve at x; NaN outside its range.
double interpolate(const std::vector<double>& xs, const std::vector<double>& ys, double x) {
    if (xs.empty() || x < std::min(xs.front(), xs.back()) || x > std::max(xs.front(), xs.back()))
        return std::numeric_limits<double>::quiet_NaN();
    for (size_t k = 1; k < xs.size(); ++k) {
        const double a = xs[k - 1], b = xs[k];
        if ((x - a) * (x - b) <= 0.0)
            return b == a ? ys[k] : ys[k - 1] + (ys[k] - ys[k - 1]) * (x - a) / (b - a);
    }
    return ys.back();
}

// {"type": "fit",
//  "analysis": {"type": "dc", "source": "V1", "start": 0.4, "stop": 0.8, "step": 0.01},
//  "output": "-i(V1)",
//  "data": {"x": [...], "y": [...]}      (or "points", or "file" with "xcol"/"ycol")
//  "parameters": {"is": {"start": 1e-13, "min": 1e-16, "max": 1e-9, "log": true}, "n": {...}},
//  "residual": "log"}
// Adjusts parameters so that the simulated curve matches the data in the
// least-squares sense (Levenberg-Marquardt with finite-difference derivatives).
void runFit(Context& c, const json& a) {
    checkKeys(a, "fit",
              {"type", "name", "analysis", "output", "data", "parameters", "residual",
               "max_iterations", "tol"},
              c.log);
    if (!a.contains("analysis") || !a["analysis"].is_object() || !a["analysis"].contains("type"))
        throw std::runtime_error("fit: needs \"analysis\": {\"type\": \"dc\" | \"ac\" | \"tran\", ...}");
    const std::string innerType = a["analysis"]["type"].get<std::string>();
    if (innerType != "dc" && innerType != "ac" && innerType != "tran")
        throw std::runtime_error("fit: the analysis must be of type dc, ac or tran");
    if (!a.contains("output") || !a["output"].is_string())
        throw std::runtime_error("fit: needs \"output\", the simulated quantity");
    const std::string residual = a.value("residual", std::string("lin"));
    if (residual != "lin" && residual != "rel" && residual != "log")
        throw std::runtime_error("fit: \"residual\" must be \"lin\", \"rel\" or \"log\"");

    // Data points
    std::vector<double> dx, dy;
    if (!a.contains("data") || !a["data"].is_object())
        throw std::runtime_error("fit: needs \"data\": {\"x\": [...], \"y\": [...]}, \"points\" or \"file\"");
    {
        const json& d = a["data"];
        const auto& par = c.ckt().parameters;
        if (d.contains("x") && d.contains("y")) {
            for (const json& v : d["x"]) dx.push_back(jsonNumber(v, "fit, data", &par));
            for (const json& v : d["y"]) dy.push_back(jsonNumber(v, "fit, data", &par));
        } else if (d.contains("points")) {
            for (const json& pt : d["points"]) {
                if (!pt.is_array() || pt.size() != 2)
                    throw std::runtime_error("fit, data: each point must be [x, y]");
                dx.push_back(jsonNumber(pt[0], "fit, data", &par));
                dy.push_back(jsonNumber(pt[1], "fit, data", &par));
            }
        } else if (d.contains("file")) {
            std::filesystem::path path(d["file"].get<std::string>());
            if (path.is_relative() && !c.nl.baseDir.empty())
                path = std::filesystem::path(c.nl.baseDir) / path;
            std::ifstream in(path);
            if (!in) throw std::runtime_error("fit: cannot open '" + path.string() + "'");
            const int xcol = d.value("xcol", 0), ycol = d.value("ycol", 1);
            for (std::string line; std::getline(in, line);) {
                for (char& ch : line)
                    if (ch == ',' || ch == ';' || ch == '\t') ch = ' ';
                std::istringstream fields(line);
                std::vector<double> row;
                for (std::string field; fields >> field;) {
                    char* end = nullptr;
                    const double v = std::strtod(field.c_str(), &end);
                    if (end == field.c_str() || *end != '\0') break;
                    row.push_back(v);
                }
                if (static_cast<int>(row.size()) <= std::max(xcol, ycol)) continue;
                dx.push_back(row[xcol]);
                dy.push_back(row[ycol]);
            }
        }
        if (dx.size() != dy.size() || dx.size() < 2)
            throw std::runtime_error("fit: the data need at least two points with x and y");
    }

    // Parameters to adjust. With "log": true the logarithm is varied.
    struct Fitted {
        std::string name;
        double lo, hi;
        bool log;
    };
    std::vector<Fitted> fitted;
    if (!a.contains("parameters") || !a["parameters"].is_object() || a["parameters"].empty())
        throw std::runtime_error("fit: needs \"parameters\": {\"name\": {\"start\": ...}, ...}");
    Eigen::VectorXd u(static_cast<Eigen::Index>(a["parameters"].size()));
    for (const auto& [name, p] : a["parameters"].items()) {
        const std::string where = "fit, parameter " + name;
        definedParameter(c, name, "fit");
        if (!p.is_object()) throw std::runtime_error(where + ": expected {\"start\": ..., \"min\": ..., \"max\": ...}");
        checkKeys(p, where, {"start", "min", "max", "log"}, c.log);
        Fitted f{name, num(c, p, "min", where, -std::numeric_limits<double>::infinity()),
                 num(c, p, "max", where, std::numeric_limits<double>::infinity()),
                 p.value("log", false)};
        const double start = num(c, p, "start", where, c.ckt().parameters.at(name));
        if (f.log && !(start > 0.0 && f.lo > 0.0))
            throw std::runtime_error(where + ": \"log\" needs a positive start value and \"min\"");
        if (!(start >= f.lo && start <= f.hi))
            throw std::runtime_error(where + ": the start value lies outside min .. max");
        u[static_cast<Eigen::Index>(fitted.size())] = f.log ? std::log(start) : start;
        fitted.push_back(f);
    }
    const auto np = static_cast<Eigen::Index>(fitted.size());
    const auto nd = static_cast<Eigen::Index>(dx.size());
    auto value = [&](const Eigen::VectorXd& v, Eigen::Index k) {
        const Fitted& f = fitted[static_cast<size_t>(k)];
        return std::min(f.hi, std::max(f.lo, f.log ? std::exp(v[k]) : v[k]));
    };

    // Residual vector for a parameter set; also returns the model at the data points.
    Sweep sweep;
    sweep.capture = a["output"].get<std::string>();
    json inner = a["analysis"];
    inner["save"] = false;
    inner.erase("measure");
    int evaluations = 0;
    std::vector<double> model(dx.size());
    auto residuals = [&](const Eigen::VectorXd& v, Eigen::VectorXd& r) {
        std::vector<std::pair<std::string, double>> parameters;
        for (Eigen::Index k = 0; k < np; ++k) parameters.emplace_back(fitted[static_cast<size_t>(k)].name, value(v, k));
        try {
            runWith(c, sweep, inner, innerType, parameters);
        } catch (const std::exception& e) {
            throw std::runtime_error("fit, " + describe(parameters) + ": " + e.what());
        }
        ++evaluations;
        r.resize(nd);
        for (Eigen::Index i = 0; i < nd; ++i) {
            const double m = interpolate(sweep.captureX, sweep.captureY, dx[static_cast<size_t>(i)]);
            const double d = dy[static_cast<size_t>(i)];
            if (std::isnan(m))
                throw std::runtime_error("fit: the data point x = " + std::to_string(dx[static_cast<size_t>(i)]) +
                                         " lies outside the range of the analysis");
            model[static_cast<size_t>(i)] = m;
            if (residual == "lin") r[i] = m - d;
            else if (residual == "rel") r[i] = (m - d) / d;
            else {
                if (!(m > 0.0 && d > 0.0))
                    throw std::runtime_error("fit: \"residual\": \"log\" needs positive data and model values");
                r[i] = std::log(m / d);
            }
        }
    };

    // Levenberg-Marquardt
    const int maxIterations = a.value("max_iterations", 50);
    const double tol = num(c, a, "tol", "fit", 1e-8);
    Eigen::VectorXd r, rTrial, uTrial;
    residuals(u, r);
    double cost = r.squaredNorm(), lambda = 1e-3;
    const double startCost = cost;
    Eigen::MatrixXd J(nd, np);
    int iterations = 0;
    for (; iterations < maxIterations; ++iterations) {
        for (Eigen::Index k = 0; k < np; ++k) {  // forward differences
            const double h = 1e-4 * std::max(std::abs(u[k]), 1e-3);
            uTrial = u;
            uTrial[k] += h;
            residuals(uTrial, rTrial);
            J.col(k) = (rTrial - r) / h;
        }
        const Eigen::MatrixXd JtJ = J.transpose() * J;
        const Eigen::VectorXd g = J.transpose() * r;
        bool improved = false;
        double newCost = cost;
        for (int attempt = 0; attempt < 12 && !improved; ++attempt) {
            Eigen::MatrixXd A = JtJ;
            for (Eigen::Index k = 0; k < np; ++k) A(k, k) += lambda * std::max(JtJ(k, k), 1e-30);
            uTrial = u - A.ldlt().solve(g);
            for (Eigen::Index k = 0; k < np; ++k) {  // stay inside the limits
                const Fitted& f = fitted[static_cast<size_t>(k)];
                const double lo = f.log ? std::log(f.lo) : f.lo, hi = f.log ? std::log(f.hi) : f.hi;
                uTrial[k] = std::min(hi, std::max(lo, uTrial[k]));
            }
            residuals(uTrial, rTrial);
            newCost = rTrial.squaredNorm();
            if (newCost < cost) improved = true;
            else lambda *= 4.0;
        }
        if (!improved) break;
        const double decrease = (cost - newCost) / std::max(cost, 1e-300);
        u = uTrial;
        r = rTrial;
        cost = newCost;
        lambda = std::max(lambda / 3.0, 1e-12);
        if (decrease < tol) {
            ++iterations;
            break;
        }
    }
    residuals(u, r);  // the model curve of the result

    const std::string path = outputPath(c, a, "fit");
    CsvWriter csv(path, {"x", "data", "model"});
    for (size_t i = 0; i < dx.size(); ++i) csv.row({dx[i], dy[i], model[i]});
    const double rms = std::sqrt(cost / static_cast<double>(nd));
    c.log << "[fit] " << iterations << " iterations, " << evaluations
          << " simulations, rms residual " << std::setprecision(6) << rms << " (start "
          << std::sqrt(startCost / static_cast<double>(nd)) << ") -> " << path << '\n';
    json* e = c.entry(a, "fit");
    if (e) {
        (*e)["file"] = path;
        (*e)["rms_residual"] = number(rms);
        (*e)["iterations"] = iterations;
        (*e)["simulations"] = evaluations;
    }
    size_t width = 0;
    for (const Fitted& f : fitted) width = std::max(width, f.name.size());
    for (Eigen::Index k = 0; k < np; ++k) {
        const Fitted& f = fitted[static_cast<size_t>(k)];
        c.log << "  " << std::left << std::setw(static_cast<int>(width)) << f.name << " = "
              << std::setprecision(6) << value(u, k) << '\n';
        if (e) (*e)["parameters"][f.name] = number(value(u, k));
    }
}

// {"type": "sens", "analysis": {...}, "parameters": ["R1", "C"], "delta": 1e-3}
// Sensitivities of the measurements of an analysis (for "op": of its outputs)
// with respect to parameters, by central differences: every parameter is
// changed by +-delta (relative) and the analysis repeated.
void runSensitivity(Context& c, const json& a) {
    checkKeys(a, "sens", {"type", "name", "analysis", "parameters", "delta"}, c.log);
    if (!a.contains("analysis") || !a["analysis"].is_object() || !a["analysis"].contains("type"))
        throw std::runtime_error("sens: needs \"analysis\": {\"type\": ..., ...}");
    json inner = a["analysis"];
    const std::string innerType = inner["type"].get<std::string>();
    if (innerType != "op" && !inner.contains("measure"))
        throw std::runtime_error("sens: the analysis needs \"measure\" (the sensitivities are those "
                                 "of its measurements)");
    inner["save"] = false;
    const double delta = num(c, a, "delta", "sens", 1e-3);
    std::vector<std::string> params;
    if (a.contains("parameters")) {
        for (const json& p : a["parameters"])
            params.push_back(definedParameter(c, p.get<std::string>(), "sens"));
    } else {
        for (const auto& [name, v] : c.ckt().parameters) params.push_back(name);
    }
    if (params.empty()) throw std::runtime_error("sens: the netlist defines no parameters");

    Sweep sweep;
    sweep.writeMeasures = false;
    auto quantities = [&](const std::vector<std::pair<std::string, double>>& changed) {
        try {
            runWith(c, sweep, inner, innerType, changed);
        } catch (const std::exception& e) {
            throw std::runtime_error("sens" + (changed.empty() ? std::string() : ", " + describe(changed)) +
                                     ": " + e.what());
        }
        std::vector<double> q;
        for (const auto& column : sweep.measureValues) q.push_back(column.back());
        return q;
    };
    const std::vector<double> nominal = quantities({});
    const std::vector<std::string> names = sweep.measureNames;

    const std::string path = outputPath(c, a, "sens");
    std::ofstream csv(path);
    if (!csv) throw std::runtime_error("cannot write '" + path + "'");
    csv.precision(10);
    csv << "parameter";
    for (const std::string& n : names) csv << ',' << (n.find(',') == std::string::npos ? n : '"' + n + '"');
    csv << '\n';

    json* e = c.entry(a, "sens");
    if (e) {
        (*e)["file"] = path;
        for (size_t k = 0; k < names.size(); ++k) (*e)["nominal"][names[k]] = number(nominal[k]);
    }
    c.log << "[sens] " << params.size() << (params.size() == 1 ? " parameter, " : " parameters, ")
          << names.size() << (names.size() == 1 ? " quantity" : " quantities") << "; normalized sensitivities (% change per % change) -> " << path << '\n';
    size_t wp = 9, wq = 12;
    for (const std::string& p : params) wp = std::max(wp, p.size());
    for (const std::string& n : names) wq = std::max(wq, n.size());
    c.log << "  " << std::left << std::setw(static_cast<int>(wp)) << "";
    for (const std::string& n : names) c.log << "  " << std::right << std::setw(static_cast<int>(wq)) << n;
    c.log << '\n';
    for (const std::string& p : params) {
        const double p0 = c.ckt().parameters.at(p);
        if (p0 == 0.0) {
            c.log << "  " << std::left << std::setw(static_cast<int>(wp)) << p << "  (is 0, skipped)\n";
            continue;
        }
        const double h = delta * std::abs(p0);
        const std::vector<double> up = quantities({{p, p0 + h}}), down = quantities({{p, p0 - h}});
        csv << p;
        c.log << "  " << std::left << std::setw(static_cast<int>(wp)) << p;
        for (size_t k = 0; k < names.size(); ++k) {
            const double absolute = (up[k] - down[k]) / (2.0 * h);
            const double normalized = absolute * p0 / nominal[k] + 0.0;  // no "-0"
            csv << ',' << normalized;
            c.log << "  " << std::right << std::setw(static_cast<int>(wq)) << std::setprecision(4)
                  << normalized;
            if (e) {
                (*e)["normalized"][p][names[k]] = number(normalized);
                (*e)["absolute"][p][names[k]] = number(absolute);
            }
        }
        csv << '\n';
        c.log << '\n';
    }
    c.log << std::left;
}

}  // namespace

void runAnalyses(Netlist& nl, const RunConfig& cfg, std::ostream& log) {
    Context c{nl, cfg, log, {}, nullptr};
    if (nl.analyses.empty()) log << "no analyses requested (add an \"analyses\" array)\n";

    for (const json& a : nl.analyses) {
        if (!a.is_object() || !a.contains("type") || !a["type"].is_string())
            throw std::runtime_error("analyses: each entry needs a \"type\"");
        const std::string type = a["type"].get<std::string>();
        if (type == "fit") runFit(c, a);
        else if (type == "sens") runSensitivity(c, a);
        else if (a.contains("monte_carlo")) runMonteCarlo(c, a, type);
        else if (a.contains("param_sweep")) runParamSweep(c, a, type);
        else runOne(c, a, type);
    }
}

}  // namespace esim
