#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "circuit/Device.hpp"
#include "expr/Expression.hpp"
#include "plugin/esim_plugin.h"

namespace esim {

// A loaded plugin library (see plugin/esim_plugin.h). Libraries are loaded
// once per file; the instances of a device type share one.
class PluginLibrary {
public:
    // Opens the file and checks its entry point and interface version. A
    // missing extension is added (.dll, .so, .dylib); a relative path is
    // taken relative to baseDir. Throws std::runtime_error.
    static std::shared_ptr<PluginLibrary> load(const std::string& file, const std::string& baseDir);
    ~PluginLibrary();
    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;

    const esim_plugin_v1& api() const { return *api_; }
    const std::string& path() const { return path_; }
    std::mutex& mutex() const { return mutex_; }  // for plugins that are not thread safe

private:
    PluginLibrary() = default;
    void* handle_ = nullptr;
    const esim_plugin_v1* api_ = nullptr;
    std::string path_;
    mutable std::mutex mutex_;
};

// A device whose equations come from a plugin. The plugin owns the rows of its
// branches, outputs and internal unknowns; the node rows it writes are added
// to those of the circuit.
class PluginDevice final : public Device {
public:
    // inputs: plugin input name -> expression of the circuit.
    // outputs: plugin output name -> name of the variable that carries it
    //          (unlisted outputs become "<device>.<output>").
    // Creates the plugin instance; throws std::runtime_error if that fails or
    // if inputs and the description do not fit.
    PluginDevice(std::string name, std::shared_ptr<PluginLibrary> lib, const std::string& configJson,
                 const std::string& baseDir, std::vector<int> nodes,
                 const std::map<std::string, expr::AstPtr>& inputs,
                 const std::map<std::string, std::string>& outputs, bool snapshots = false,
                 std::vector<double> snapshotTimes = {});
    ~PluginDevice() override;

    void setup(Circuit& ckt) override;
    void bind(Circuit& ckt) override;
    void stamp(StampContext& ctx) override;
    int branch() const override { return branchIdx_.empty() ? -1 : branchIdx_[0]; }
    int branchOf(const std::string& sub) const override;
    void applyGuess(Vector& x) const override;
    void acceptStep(double t, const Vector& x, bool start) override;
    double nextBreakpoint(double t) const override;
    double maxStep() const override;
    void reset() override;
    void noiseSources(std::vector<NoiseSource>& out) const override;
    bool wantsSnapshot() const override;
    void snapshot(double t, const Vector& x, const std::string& tag) override;

private:
    void gather(const Vector& x, double t, bool withGradient);
    [[noreturn]] void fail(const std::string& what) const;

    std::shared_ptr<PluginLibrary> lib_;
    void* inst_ = nullptr;
    bool threadSafe_ = false;

    // Description of the plugin, copied.
    int nNodes_ = 0, nIn_ = 0, nBr_ = 0, nOut_ = 0, nInt_ = 0, nLocal_ = 0;
    int oIn_ = 0, oBr_ = 0, oOut_ = 0, oInt_ = 0;  // offsets of the blocks
    std::vector<std::string> inNames_, brNames_, outNames_, intNames_;
    std::vector<int> brNodes_;  // pairs of terminals
    std::vector<double> start_, abstol_;
    std::vector<int> kinds_;  // ESIM_KIND_* of branches, outputs, internal
    int nNoise_ = 0;
    std::vector<int> noiseRows_;  // pairs of local rows (-1: ground)
    std::vector<std::string> noiseNames_;
    std::vector<esim_pos> posG_, posC_;

    // Connection to the circuit.
    std::vector<int> nodes_;                 // terminal -> node unknown (-1: ground)
    std::vector<expr::AstPtr> inAst_;        // input expressions
    std::vector<expr::Program> inProg_;
    std::vector<std::string> outVar_;        // output -> variable name
    std::vector<int> branchIdx_;             // unknowns of the branches
    std::vector<int> unknown_;               // local index -> unknown (-1 ground; inputs unused)
    Circuit* ckt_ = nullptr;
    bool snapshots_ = false;
    std::vector<double> snapTimes_;          // sorted times of snapshots during a transient
    size_t nextSnap_ = 0;
    double temperature_ = 300.15;            // of the last evaluation

    // Scratch of the last evaluation.
    std::vector<double> w_, f_, q_, G_, C_;
    std::vector<std::vector<double>> grad_;  // per input: d input / d x[vars()[k]]
};

}  // namespace esim
