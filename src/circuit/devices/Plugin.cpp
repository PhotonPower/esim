#include "circuit/devices/Plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "circuit/Circuit.hpp"
#include "circuit/Symbols.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace esim {

namespace fs = std::filesystem;

namespace {

void hostLog(int32_t level, const char* message) {
    std::cerr << (level >= 1 ? "warning: plugin: " : "note: plugin: ") << (message ? message : "")
              << '\n';
}

const esim_host kHost = {sizeof(esim_host), ESIM_PLUGIN_API_VERSION, hostLog};

std::mutex g_registryMutex;
std::map<std::string, std::weak_ptr<PluginLibrary>>& registry() {
    static std::map<std::string, std::weak_ptr<PluginLibrary>> r;
    return r;
}

#ifdef _WIN32
std::string lastSystemError() {
    const DWORD code = GetLastError();
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string s = text ? text : "error " + std::to_string(code);
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
#endif

}  // namespace

std::shared_ptr<PluginLibrary> PluginLibrary::load(const std::string& file, const std::string& baseDir) {
    fs::path p = fs::u8path(file);
    if (p.is_relative() && !baseDir.empty()) p = fs::u8path(baseDir) / p;
    fs::path found;
    std::string tried;
    for (const char* ext : {"", ".dll", ".so", ".dylib"}) {
        fs::path candidate = p;
        candidate += ext;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            found = candidate;
            break;
        }
        tried += (tried.empty() ? "" : ", ") + candidate.u8string();
    }
    if (found.empty()) throw std::runtime_error("plugin library not found (tried " + tried + ")");
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(found, ec);
    const std::string key = (ec ? found : canonical).u8string();

    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (auto live = registry()[key].lock()) return live;

    std::shared_ptr<PluginLibrary> lib(new PluginLibrary());
    lib->path_ = key;
    esim_plugin_entry_fn entry = nullptr;
#ifdef _WIN32
    HMODULE h = LoadLibraryExW((ec ? found : canonical).wstring().c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!h) throw std::runtime_error("cannot load plugin '" + key + "': " + lastSystemError());
    lib->handle_ = h;
    entry = reinterpret_cast<esim_plugin_entry_fn>(
        reinterpret_cast<void*>(GetProcAddress(h, ESIM_PLUGIN_ENTRY)));
#else
    void* h = dlopen(key.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) throw std::runtime_error("cannot load plugin '" + key + "': " + dlerror());
    lib->handle_ = h;
    entry = reinterpret_cast<esim_plugin_entry_fn>(dlsym(h, ESIM_PLUGIN_ENTRY));
#endif
    if (!entry)
        throw std::runtime_error("'" + key + "' is not an esim plugin: no function " ESIM_PLUGIN_ENTRY);
    lib->api_ = entry();
    const esim_plugin_v1* api = lib->api_;
    if (!api || api->size < offsetof(esim_plugin_v1, accept_step))
        throw std::runtime_error("plugin '" + key + "': invalid interface table");
    if (api->api_version != ESIM_PLUGIN_API_VERSION)
        throw std::runtime_error("plugin '" + key + "' has interface version " +
                                 std::to_string(api->api_version) + ", esim understands version " +
                                 std::to_string(ESIM_PLUGIN_API_VERSION));
    if (!api->create || !api->destroy || !api->describe || !api->evaluate)
        throw std::runtime_error("plugin '" + key + "': create, destroy, describe and evaluate are required");
    registry()[key] = lib;
    return lib;
}

PluginLibrary::~PluginLibrary() {
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
}

// ---------------------------------------------------------------------------

namespace {

// Optional members of the table only exist if the plugin's table is long enough.
template <typename Fn>
Fn optionalMember(const esim_plugin_v1& api, Fn esim_plugin_v1::*member, size_t offset) {
    return api.size >= offset + sizeof(Fn) ? api.*member : nullptr;
}

std::vector<std::string> names(const char* const* list, int n, const std::string& what) {
    std::vector<std::string> out;
    for (int k = 0; k < n; ++k) {
        if (list && list[k]) out.emplace_back(list[k]);
        else out.push_back(what + std::to_string(k));
    }
    return out;
}

}  // namespace

PluginDevice::PluginDevice(std::string name, std::shared_ptr<PluginLibrary> lib,
                           const std::string& configJson, const std::string& baseDir,
                           std::vector<int> nodes, const std::map<std::string, expr::AstPtr>& inputs,
                           const std::map<std::string, std::string>& outputs, bool snapshots,
                           std::vector<double> snapshotTimes)
    : Device(std::move(name)), lib_(std::move(lib)), nodes_(std::move(nodes)) {
    snapshots_ = snapshots;
    snapTimes_ = std::move(snapshotTimes);
    std::sort(snapTimes_.begin(), snapTimes_.end());
    snapTimes_.erase(std::unique(snapTimes_.begin(), snapTimes_.end()), snapTimes_.end());
    const esim_plugin_v1& api = lib_->api();
    char err[512] = "";
    inst_ = api.create(configJson.c_str(), baseDir.c_str(), &kHost, err, sizeof err);
    if (!inst_)
        throw std::runtime_error("plugin '" + std::string(api.name ? api.name : "?") + "': " +
                                 (*err ? err : "create failed"));
    try {
        esim_description d;
        std::memset(&d, 0, sizeof d);
        d.size = sizeof d;
        if (api.describe(inst_, &d) != 0) fail("describe failed");

        nNodes_ = d.n_nodes;
        nIn_ = d.n_inputs;
        nBr_ = d.n_branches;
        nOut_ = d.n_outputs;
        nInt_ = d.n_internal;
        if (nNodes_ < 0 || nIn_ < 0 || nBr_ < 0 || nOut_ < 0 || nInt_ < 0)
            fail("negative number of unknowns in the description");
        oIn_ = nNodes_;
        oBr_ = oIn_ + nIn_;
        oOut_ = oBr_ + nBr_;
        oInt_ = oOut_ + nOut_;
        nLocal_ = oInt_ + nInt_;
        threadSafe_ = d.thread_safe != 0;

        if (static_cast<int>(nodes_.size()) != nNodes_)
            fail("the plugin has " + std::to_string(nNodes_) + " terminal(s), the component lists " +
                 std::to_string(nodes_.size()) + " node(s)");

        if (nIn_ > 0 && !d.input_names) fail("the description lacks input_names");
        if (nOut_ > 0 && !d.output_names) fail("the description lacks output_names");
        inNames_ = names(d.input_names, nIn_, "in");
        brNames_ = names(d.branch_names, nBr_, "b");
        outNames_ = names(d.output_names, nOut_, "out");
        intNames_ = names(d.internal_names, nInt_, "y");

        if (nBr_ > 0) {
            if (!d.branch_nodes) fail("the description lacks branch_nodes");
            brNodes_.assign(d.branch_nodes, d.branch_nodes + 2 * nBr_);
            for (int t : brNodes_)
                if (t < 0 || t >= nNodes_) fail("branch_nodes refers to a terminal that does not exist");
        }
        const int owned = nBr_ + nOut_ + nInt_;
        start_.assign(owned, 0.0);
        abstol_.assign(owned, -1.0);
        if (d.start) start_.assign(d.start, d.start + owned);
        if (d.abstol) abstol_.assign(d.abstol, d.abstol + owned);
        kinds_.assign(owned, ESIM_KIND_DEFAULT);
        if (d.kinds) {
            kinds_.assign(d.kinds, d.kinds + owned);
            for (int k : kinds_)
                if (k < ESIM_KIND_DEFAULT || k > ESIM_KIND_VARIABLE)
                    fail("unknown kind of an unknown in the description");
        }

        auto positions = [&](const esim_pos* pos, int count, std::vector<esim_pos>& out, const char* what) {
            if (count < 0 || (count > 0 && !pos)) fail(std::string("invalid ") + what + " pattern");
            out.assign(pos, pos + count);
            for (const esim_pos& e : out) {
                if (e.row < 0 || e.row >= nLocal_ || e.col < 0 || e.col >= nLocal_)
                    fail(std::string(what) + " pattern: index out of range");
                if (e.row >= oIn_ && e.row < oBr_)
                    fail(std::string(what) + " pattern: an input has no row");
            }
        };
        positions(d.pos_G, d.n_G, posG_, "G");
        positions(d.pos_C, d.n_C, posC_, "C");

        // Noise sources: rows of the terminals or of the plugin's own unknowns.
        nNoise_ = d.n_noise;
        if (nNoise_ < 0 || (nNoise_ > 0 && !d.noise_rows)) fail("invalid noise sources in the description");
        if (nNoise_ > 0) {
            if (api.size < offsetof(esim_plugin_v1, noise) + sizeof(api.noise) || !api.noise)
                fail("the description lists noise sources, but the plugin has no noise function");
            noiseRows_.assign(d.noise_rows, d.noise_rows + 2 * nNoise_);
            for (int r : noiseRows_)
                if (r < -1 || r >= nLocal_ || (r >= oIn_ && r < oBr_))
                    fail("noise source: row out of range (an input has no row)");
            noiseNames_ = names(d.noise_names, nNoise_, "noise");
        }
        if (snapshots_ &&
            !(api.size >= offsetof(esim_plugin_v1, snapshot) + sizeof(api.snapshot) && api.snapshot))
            fail("\"snapshots\" is requested, but the plugin cannot write snapshots");

        // Netlist inputs and outputs against the description.
        for (const auto& [key, ast] : inputs) {
            bool known = false;
            for (const std::string& n : inNames_) known = known || n == key;
            if (!known) fail("has no input \"" + key + "\" (inputs: " + [&] {
                std::string s;
                for (const std::string& n : inNames_) s += (s.empty() ? "" : ", ") + n;
                return s.empty() ? std::string("none") : s;
            }() + ")");
        }
        for (const std::string& n : inNames_) {
            auto it = inputs.find(n);
            if (it == inputs.end()) fail("input \"" + n + "\" is not given (\"inputs\": {\"" + n + "\": expression})");
            inAst_.push_back(it->second);
        }
        for (const auto& [key, var] : outputs) {
            bool known = false;
            for (const std::string& n : outNames_) known = known || n == key;
            if (!known) fail("has no output \"" + key + "\"");
        }
        for (const std::string& n : outNames_) {
            auto it = outputs.find(n);
            outVar_.push_back(it == outputs.end() ? this->name() + "." + n : it->second);
        }
    } catch (...) {
        api.destroy(inst_);
        inst_ = nullptr;
        throw;
    }
}

PluginDevice::~PluginDevice() {
    if (inst_) lib_->api().destroy(inst_);
}

void PluginDevice::fail(const std::string& what) const {
    std::string msg = "plugin component '" + name() + "': " + what;
    const esim_plugin_v1& api = lib_->api();
    if (inst_ && api.last_error && api.size >= offsetof(esim_plugin_v1, last_error) + sizeof(api.last_error)) {
        const char* detail = api.last_error(inst_);
        if (detail && *detail) msg += " (" + std::string(detail) + ")";
    }
    throw std::runtime_error(msg);
}

void PluginDevice::setup(Circuit& ckt) {
    ckt_ = &ckt;
    unknown_.assign(nLocal_, -1);
    auto kindOf = [&](int owned, UnknownKind fallback) {
        switch (kinds_[owned]) {
            case ESIM_KIND_VOLTAGE: return UnknownKind::Voltage;
            case ESIM_KIND_CURRENT: return UnknownKind::Current;
            case ESIM_KIND_VARIABLE: return UnknownKind::Variable;
            default: return fallback;
        }
    };
    for (int t = 0; t < nNodes_; ++t) unknown_[t] = nodes_[t];
    int owned = 0;
    for (int k = 0; k < nBr_; ++k, ++owned) {
        const std::string label = k == 0 ? "i(" + name() + ")" : "i(" + name() + "." + brNames_[k] + ")";
        const int idx = ckt.addUnknown(label, kindOf(owned, UnknownKind::Current), false, abstol_[owned]);
        branchIdx_.push_back(idx);
        unknown_[oBr_ + k] = idx;
    }
    for (int k = 0; k < nOut_; ++k, ++owned)
        unknown_[oOut_ + k] =
            ckt.addVariable(outVar_[k], abstol_[owned], kindOf(owned, UnknownKind::Variable));
    for (int k = 0; k < nInt_; ++k, ++owned) {
        const int idx = ckt.addVariable(name() + "." + intNames_[k], abstol_[owned],
                                        kindOf(owned, UnknownKind::Variable));
        ckt.setInternal(idx, true);
        unknown_[oInt_ + k] = idx;
        ckt.setEquationText(idx, "the equation of internal unknown '" + intNames_[k] + "' of plugin " + name());
    }
    for (int k = 0; k < nBr_; ++k)
        ckt.setEquationText(branchIdx_[k], "the branch equation of " + name() + " (plugin)");
    for (int k = 0; k < nOut_; ++k)
        ckt.setEquationText(unknown_[oOut_ + k], "the equation of output '" + outNames_[k] + "' of plugin " + name());
    ckt.notes.push_back("plugin component " + name() + ": " + lib_->path());
}

void PluginDevice::bind(Circuit& ckt) {
    const CircuitSymbols symbols(ckt);
    for (int k = 0; k < nIn_; ++k) {
        try {
            inProg_.push_back(expr::Program::compile(inAst_[k], symbols));
        } catch (const std::runtime_error& e) {
            throw std::runtime_error("plugin component '" + name() + "', input \"" + inNames_[k] +
                                     "\": " + e.what());
        }
    }
    grad_.assign(nIn_, {});
}

int PluginDevice::branchOf(const std::string& sub) const {
    for (int k = 0; k < nBr_ && k < static_cast<int>(branchIdx_.size()); ++k)
        if (brNames_[k] == sub) return branchIdx_[k];
    return -1;
}

// Local vector from the solution; inputs are evaluated (with their gradient).
void PluginDevice::gather(const Vector& x, double t, bool withGradient) {
    w_.assign(nLocal_, 0.0);
    for (int k = 0; k < nNodes_; ++k) w_[k] = nodes_[k] < 0 ? 0.0 : x[nodes_[k]];
    for (int k = 0; k < nIn_; ++k) {
        if (withGradient) w_[oIn_ + k] = inProg_[k].evalGrad(x, t, grad_[k]);
        else w_[oIn_ + k] = inProg_[k].eval(x, t);
    }
    for (int r = oBr_; r < nLocal_; ++r) w_[r] = x[unknown_[r]];
}

void PluginDevice::stamp(StampContext& ctx) {
    gather(ctx.x, ctx.t, true);

    f_.assign(nLocal_, 0.0);
    q_.assign(nLocal_, 0.0);
    G_.assign(posG_.size(), 0.0);
    C_.assign(posC_.size(), 0.0);
    esim_eval ev;
    std::memset(&ev, 0, sizeof ev);
    ev.size = sizeof ev;
    ev.t = ctx.t;
    ev.mode = !ctx.transient ? ESIM_MODE_DC
              : (ctx.initPoint || ctx.useIc) ? ESIM_MODE_INIT : ESIM_MODE_TRAN;
    ev.temperature = ctx.vt * kElementaryCharge / kBoltzmann;
    ev.n_local = nLocal_;
    temperature_ = ev.temperature;
    ev.limit_mode = static_cast<int32_t>(ctx.limiting);  // LimitMode and ESIM_LIMIT_* agree
    ev.w = w_.data();
    ev.f = f_.data();
    ev.q = q_.data();
    ev.G = G_.data();
    ev.C = C_.data();

    int rc;
    if (threadSafe_) {
        rc = lib_->api().evaluate(inst_, &ev);
    } else {
        std::lock_guard<std::mutex> lock(lib_->mutex());
        rc = lib_->api().evaluate(inst_, &ev);
    }
    if (rc != 0) fail("evaluation failed at t = " + std::to_string(ctx.t));
    if (ev.limited) ctx.out.limited = true;

    // Currents of the branches into their terminals.
    for (int k = 0; k < nBr_; ++k) {
        const int br = branchIdx_[k], p = nodes_[brNodes_[2 * k]], n = nodes_[brNodes_[2 * k + 1]];
        ctx.addF(p, ctx.x[br]);
        ctx.addF(n, -ctx.x[br]);
        ctx.addG(p, br, 1.0);
        ctx.addG(n, br, -1.0);
    }
    // Rows of the plugin: the terminals add to the circuit's rows.
    for (int r = 0; r < nLocal_; ++r) {
        if (r >= oIn_ && r < oBr_) continue;
        ctx.addF(unknown_[r], f_[r]);
        ctx.addQ(unknown_[r], q_[r]);
    }
    // Jacobians; a column of an input goes through the chain rule.
    auto spread = [&](const std::vector<esim_pos>& pos, const std::vector<double>& val, bool charge) {
        for (size_t e = 0; e < pos.size(); ++e) {
            const int row = unknown_[pos[e].row], col = pos[e].col;
            if (col >= oIn_ && col < oBr_) {
                const int j = col - oIn_;
                const std::vector<int>& vars = inProg_[j].vars();
                for (size_t k = 0; k < vars.size(); ++k) {
                    const double v = val[e] * grad_[j][k];
                    if (charge) ctx.addC(row, vars[k], v);
                    else ctx.addG(row, vars[k], v);
                }
            } else {
                if (charge) ctx.addC(row, unknown_[col], val[e]);
                else ctx.addG(row, unknown_[col], val[e]);
            }
        }
    };
    spread(posG_, G_, false);
    spread(posC_, C_, true);
}

void PluginDevice::applyGuess(Vector& x) const {
    for (int k = 0; k < nBr_ + nOut_ + nInt_; ++k) x[unknown_[oBr_ + k]] = start_[k];
}

void PluginDevice::acceptStep(double t, const Vector& x, bool start) {
    const esim_plugin_v1& api = lib_->api();
    if (optionalMember(api, &esim_plugin_v1::accept_step, offsetof(esim_plugin_v1, accept_step))) {
        gather(x, t, false);
        if (threadSafe_) {
            api.accept_step(inst_, t, w_.data(), nLocal_, start ? 1 : 0);
        } else {
            std::lock_guard<std::mutex> lock(lib_->mutex());
            api.accept_step(inst_, t, w_.data(), nLocal_, start ? 1 : 0);
        }
    }
    // Snapshots at given times of a transient. The times are breakpoints, so a
    // step ends on each of them.
    if (start) {
        nextSnap_ = 0;
        while (nextSnap_ < snapTimes_.size() && snapTimes_[nextSnap_] <= t) ++nextSnap_;
        return;
    }
    while (snapshots_ && nextSnap_ < snapTimes_.size() && t >= snapTimes_[nextSnap_] * (1.0 - 1e-12)) {
        std::ostringstream tag;
        tag << "tran.t" << std::setprecision(6) << snapTimes_[nextSnap_];
        ++nextSnap_;
        snapshot(t, x, tag.str());
    }
}

double PluginDevice::nextBreakpoint(double t) const {
    const esim_plugin_v1& api = lib_->api();
    double best = std::numeric_limits<double>::infinity();
    if (optionalMember(api, &esim_plugin_v1::next_breakpoint, offsetof(esim_plugin_v1, next_breakpoint)))
        best = api.next_breakpoint(inst_, t);
    if (snapshots_) {
        const auto it = std::upper_bound(snapTimes_.begin(), snapTimes_.end(), t);
        if (it != snapTimes_.end()) best = std::min(best, *it);
    }
    return best;
}

double PluginDevice::maxStep() const {
    const esim_plugin_v1& api = lib_->api();
    if (!optionalMember(api, &esim_plugin_v1::max_step, offsetof(esim_plugin_v1, max_step)))
        return std::numeric_limits<double>::infinity();
    return api.max_step(inst_);
}

void PluginDevice::noiseSources(std::vector<NoiseSource>& out) const {
    if (nNoise_ == 0 || w_.size() != static_cast<size_t>(nLocal_)) return;
    std::vector<double> white(nNoise_, 0.0), flicker(nNoise_, 0.0);
    int rc;
    {
        std::unique_lock<std::mutex> lock(lib_->mutex(), std::defer_lock);
        if (!threadSafe_) lock.lock();
        rc = lib_->api().noise(inst_, w_.data(), nLocal_, temperature_, white.data(), flicker.data());
    }
    if (rc != 0) fail("the noise function failed");
    for (int k = 0; k < nNoise_; ++k) {
        const int a = noiseRows_[2 * k], b = noiseRows_[2 * k + 1];
        out.push_back({name() + "." + noiseNames_[k], a < 0 ? -1 : unknown_[a],
                       b < 0 ? -1 : unknown_[b], white[k], flicker[k]});
    }
}

bool PluginDevice::wantsSnapshot() const { return snapshots_; }

void PluginDevice::snapshot(double t, const Vector& x, const std::string& tag) {
    if (!ckt_ || ckt_->snapshotDir.empty()) return;
    std::filesystem::create_directories(std::filesystem::u8path(ckt_->snapshotDir));
    gather(x, t, false);
    const std::string stem = ckt_->snapshotStem + "." + tag + "." + name();
    int rc;
    {
        std::unique_lock<std::mutex> lock(lib_->mutex(), std::defer_lock);
        if (!threadSafe_) lock.lock();
        rc = lib_->api().snapshot(inst_, t, w_.data(), nLocal_, ckt_->snapshotDir.c_str(), stem.c_str());
    }
    if (rc != 0) fail("writing the snapshot \"" + stem + "\" failed");
}

void PluginDevice::reset() {
    const esim_plugin_v1& api = lib_->api();
    if (optionalMember(api, &esim_plugin_v1::reset, offsetof(esim_plugin_v1, reset))) api.reset(inst_);
}

}  // namespace esim
