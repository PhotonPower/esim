#include "app/Runner.hpp"

#include <algorithm>
#include <cmath>
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

// A parameter sweep in progress: all runs write to one CSV file whose first
// column is the parameter value.
struct Sweep {
    std::string param;
    std::string path;         // curves of all runs
    std::string measurePath;  // one row of measurements per run
    double value = 0.0;
    std::unique_ptr<CsvWriter> csv;
    std::unique_ptr<CsvWriter> measureCsv;
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
                header.insert(header.begin(), c.sweep->param);
                c.sweep->csv = std::make_unique<CsvWriter>(c.sweep->path, header);
            }
            csv_ = c.sweep->csv.get();
            path_ = c.sweep->path;
            prefix_ = {c.sweep->value};
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
        const std::string tag = a.value("name", std::string("tran"));
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
        if (!a.contains("measure")) return;
        specs_ = parseMeasures(a["measure"], c.ckt().parameters);
        for (const MeasureSpec& spec : specs_) probes_.push_back(parseProbe(c.ckt(), spec.expression));
        y_.resize(specs_.size());
    }

    template <class ValueOf>
    void sample(double x, ValueOf&& valueOf) {
        if (specs_.empty()) return;
        x_.push_back(x);
        for (size_t k = 0; k < probes_.size(); ++k) y_[k].push_back(valueOf(probes_[k]));
    }

    void report(Context& c, json* entry = nullptr) {
        if (specs_.empty()) return;
        std::vector<double> results;
        for (size_t k = 0; k < specs_.size(); ++k)
            results.push_back(evaluateMeasure(specs_[k], x_, y_[k]));
        if (entry)
            for (size_t k = 0; k < specs_.size(); ++k)
                (*entry)["measures"][specs_[k].name] = number(results[k]);

        if (c.sweep) {  // table: one row per parameter value
            if (!c.sweep->measureCsv) {
                std::vector<std::string> header{c.sweep->param};
                for (const MeasureSpec& spec : specs_) header.push_back(spec.name);
                c.sweep->measureCsv = std::make_unique<CsvWriter>(c.sweep->measurePath, header);
            }
            results.insert(results.begin(), c.sweep->value);
            c.sweep->measureCsv->row(results);
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
    std::vector<Probe> probes_;
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

void runTran(Context& c, const json& a) {
    checkKeys(a, "tran",
              {"type", "name", "outputs", "tstop", "tstep", "hmax", "hmin", "hinit", "method",
               "uic", "output_step", "max_events", "max_order", "measure", "save", "fourier"},
              c.log);
    const SimOptions& so = c.nl.options;
    TransientOptions to;
    to.tstop = num(c, a, "tstop", "tran");
    if (!(to.tstop > 0.0)) throw std::runtime_error("tran: tstop must be > 0");
    // tstep is the resolution the user wants to see: no step is longer.
    const double tstep = num(c, a, "tstep", "tran", 0.0);
    to.hmax = num(c, a, "hmax", "tran", tstep > 0.0 ? std::min(tstep, to.tstop / 50.0) : 0.0);
    to.hmin = num(c, a, "hmin", "tran", 0.0);
    to.hinit = num(c, a, "hinit", "tran", 0.0);
    to.method = parseMethod(a.value("method", std::string("bdf")));
    to.outputStep = num(c, a, "output_step", "tran", 0.0);
    to.maxEvents = static_cast<int>(num(c, a, "max_events", "tran", to.maxEvents));
    to.maxOrder = static_cast<int>(num(c, a, "max_order", "tran", to.maxOrder));
    to.reltol = so.reltol;
    to.trtol = so.trtol;
    to.peakFloor = so.lteFloor;
    to.newton.maxIter = so.maxIterTran;
    to.newton.reltol = so.reltol;
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
        Vector x0, q0;
        if (uic) {
            ckt.initialState(x0, q0);
        } else {
            // Initial point: DC solution with the prescribed states held.
            ckt.setInitialPointMode(true);
            x0 = solveOperatingPoint(ckt, so);
            ckt.setInitialPointMode(false);
        }
        std::vector<double> row;
        stats = runTransient(ckt, x0, uic ? &q0 : nullptr, to, [&](double t, const Vector& x) {
            row.clear();
            for (const Probe& p : probes) row.push_back(p.value(x, t));
            table.row(t, row);
            measures.sample(t, [&](const Probe& p) { return p.value(x, t); });
            fourierBlock.sample(t, x);
        });
    } catch (...) {
        ckt.setInitialPointMode(false);
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
    else
        throw std::runtime_error("analyses: unknown type \"" + type +
                                 "\" (op, dc, tran, ac, noise)");
}

// "param_sweep": {"param": "R1", "values": [...]}
//             or {"param": "R1", "start": 1e3, "stop": 1e5, "points": 5, "scale": "log"}
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

// Repeat one analysis for several values of a parameter. Parameters are
// resolved while the input is read, so every value gets a freshly built system.
void runParamSweep(Context& c, const json& a, const std::string& type) {
    const json& spec = a["param_sweep"];
    if (!spec.is_object() || !spec.contains("param") || !spec["param"].is_string())
        throw std::runtime_error("param_sweep: needs \"param\": \"<parameter name>\"");
    Sweep sweep;
    sweep.param = spec["param"].get<std::string>();
    if (!c.ckt().parameters.count(sweep.param))
        throw std::runtime_error("param_sweep: '" + sweep.param +
                                 "' is not defined under \"parameters\"");
    const std::vector<double> values = sweepValues(c, spec);
    sweep.path = outputPath(c, a, type);
    sweep.measurePath = sweep.path.substr(0, sweep.path.size() - 4) + ".measure.csv";

    json inner = a;
    inner.erase("param_sweep");
    std::ostringstream quiet;  // per-run progress is not reported
    for (double value : values) {
        sweep.value = value;
        try {
            json root = c.nl.source;
            root["parameters"][sweep.param] = value;
            Netlist sub = parseNetlist(root, /*warnings=*/false, c.nl.baseDir);
            Context sc{sub, c.cfg, quiet, {}, &sweep};  // sc.entry() stays silent in a sweep
            runOne(sc, inner, type);
        } catch (const std::exception& e) {
            std::ostringstream os;
            os << "param_sweep, " << sweep.param << " = " << value << ": " << e.what();
            throw std::runtime_error(os.str());
        }
    }
    c.log << "[" << type << "] sweep of " << sweep.param << " = " << values.front() << " .. "
          << values.back() << ": " << values.size() << " runs";
    if (sweep.csv) c.log << " -> " << sweep.path;
    if (sweep.measureCsv) c.log << " -> " << sweep.measurePath;
    c.log << '\n';
    if (json* e = c.entry(a, type)) {
        (*e)["sweep"] = {{"param", sweep.param}, {"values", values}};
        (*e)["file"] = sweep.csv ? json(sweep.path) : json(nullptr);
        (*e)["measure_file"] = sweep.measureCsv ? json(sweep.measurePath) : json(nullptr);
    }
}

}  // namespace

void runAnalyses(Netlist& nl, const RunConfig& cfg, std::ostream& log) {
    Context c{nl, cfg, log, {}, nullptr};
    if (nl.analyses.empty()) log << "no analyses requested (add an \"analyses\" array)\n";

    for (const json& a : nl.analyses) {
        if (!a.is_object() || !a.contains("type") || !a["type"].is_string())
            throw std::runtime_error("analyses: each entry needs a \"type\"");
        const std::string type = a["type"].get<std::string>();
        if (a.contains("param_sweep")) runParamSweep(c, a, type);
        else runOne(c, a, type);
    }
}

}  // namespace esim
