#include "circuit/Circuit.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>

#include "circuit/Device.hpp"

namespace esim {

Circuit::Circuit() = default;
Circuit::~Circuit() = default;

bool Circuit::isGroundName(const std::string& name) {
    return name == "0" || name == "gnd" || name == "GND" || name == "Gnd";
}

int Circuit::node(const std::string& name) {
    if (name.empty()) throw std::runtime_error("empty node name");
    if (isGroundName(name)) {
        groundUsed_ = true;
        return Ground;
    }
    auto it = nodes_.find(name);
    if (it != nodes_.end()) return it->second;
    const int idx = addUnknown("v(" + name + ")", UnknownKind::Voltage, false);
    nodes_.emplace(name, idx);
    return idx;
}

int Circuit::findNode(const std::string& name) const {
    if (isGroundName(name)) return Ground;
    auto it = nodes_.find(name);
    if (it == nodes_.end()) throw std::runtime_error("unknown node '" + name + "'");
    return it->second;
}

int Circuit::addUnknown(const std::string& label, UnknownKind kind, bool internal) {
    unknowns_.push_back({label, kind, internal});
    return static_cast<int>(unknowns_.size()) - 1;
}

int Circuit::addVariable(const std::string& name, double abstol) {
    if (variables_.count(name))
        throw std::runtime_error("variable '" + name + "' is defined twice");
    const int idx = addUnknown(name, UnknownKind::Variable, false);
    unknowns_[idx].abstol = abstol;
    variables_.emplace(name, idx);
    return idx;
}

int Circuit::findVariable(const std::string& name) const {
    auto it = variables_.find(name);
    return it == variables_.end() ? -1 : it->second;
}

Device& Circuit::add(std::unique_ptr<Device> dev) {
    if (finalized_) throw std::logic_error("circuit is already finalized");
    if (!deviceByName_.emplace(dev->name(), dev.get()).second)
        throw std::runtime_error("duplicate component name '" + dev->name() + "'");
    devices_.push_back(std::move(dev));
    return *devices_.back();
}

Device* Circuit::findDevice(const std::string& name) const {
    auto it = deviceByName_.find(name);
    return it == deviceByName_.end() ? nullptr : it->second;
}

void Circuit::finalize() {
    if (finalized_) return;
    if (devices_.empty()) throw std::runtime_error("nothing to simulate: no components or equations");
    if (!nodes_.empty() && !groundUsed_)
        throw std::runtime_error("circuit has no ground node (use node name \"0\" or \"gnd\")");
    for (auto& d : devices_) d->setup(*this);
    for (auto& d : devices_) d->bind(*this);
    for (auto& d : devices_) eventCount_ += d->eventCount();
    finalized_ = true;
}

Vector Circuit::initialGuess() const {
    Vector x = Vector::Zero(size());
    for (const auto& d : devices_) d->applyGuess(x);
    for (const auto& [name, value] : nodeset) {
        const int idx = findNode(name);
        if (idx >= 0) x[idx] = value;
    }
    return x;
}

void Circuit::initialState(Vector& x0, Vector& q0) {
    x0 = Vector::Zero(size());
    for (const auto& d : devices_) d->applyIc(x0);
    std::vector<char> known(static_cast<size_t>(size()), 0);
    for (const auto& [name, value] : ic) {
        const int idx = findNode(name);
        if (idx >= 0) {
            x0[idx] = value;
            known[static_cast<size_t>(idx)] = 1;
        }
    }
    // Nodes held by voltage sources start at the voltage the sources give
    // them, not at 0: a supply that jumped from 0 to its value within the
    // first step would be a shock nobody asked for. The voltages are passed
    // on from ground (and from the given initial conditions) through chains
    // of sources.
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& d : devices_) {
            int p, n;
            double v;
            if (!d->startConstraint(p, n, v)) continue;
            const bool kp = p < 0 || known[static_cast<size_t>(p)], kn = n < 0 || known[static_cast<size_t>(n)];
            if (kp == kn) continue;
            if (kn) {
                x0[p] = (n < 0 ? 0.0 : x0[n]) + v;
                known[static_cast<size_t>(p)] = 1;
            } else {
                x0[n] = (p < 0 ? 0.0 : x0[p]) - v;
                known[static_cast<size_t>(n)] = 1;
            }
            changed = true;
        }
    }
    Evaluation ev;
    useIc_ = true;
    evaluate(0.0, x0, LimitMode::Off, ev);
    useIc_ = false;
    q0 = ev.q;
}

void Circuit::setThreads(int threads) {
    threads_ = threads < 1 ? 1 : threads;
    if (devices_.size() < kParallelDevices) {
        pool_.reset();
        blocks_.clear();
        return;
    }
    // The blocks are the same for every number of threads (also for one).
    if (blocks_.empty()) {
        blocks_.assign(kBlocks, Block());
        for (size_t k = 0; k < kBlocks; ++k) {
            blocks_[k].first = devices_.size() * k / kBlocks;
            blocks_[k].last = devices_.size() * (k + 1) / kBlocks;
        }
        measured_ = false;
    }
    if (!pool_ || pool_->parts() != threads_) pool_ = std::make_unique<WorkerPool>(threads_);
}

// Sequential pass, block by block, that notes which section of the matrix
// entries every block produces.
void Circuit::stampMeasured(double t, const Vector& x, LimitMode limiting, Evaluation& out) {
    out.reset(size());
    for (Block& b : blocks_) {
        StampContext ctx{t, x, out, limiting, transient_, useIc_, initPoint_, liveModes_,
                         srcScale_, options.gmin, options.vt()};
        b.gFirst = out.G.size();
        b.cFirst = out.C.size();
        for (size_t i = b.first; i < b.last; ++i) devices_[i]->stamp(ctx);
        b.gLast = out.G.size();
        b.cLast = out.C.size();
        b.rowLo = std::min(b.rowLo, ctx.rowLo);
        b.rowHi = std::max(b.rowHi, ctx.rowHi);
        // The buffer of the block has to be clean over the rows it will use.
        b.part.f.setZero(size());
        b.part.q.setZero(size());
    }
    measured_ = true;
}

void Circuit::stampAll(double t, const Vector& x, LimitMode limiting, Evaluation& out) {
    if (blocks_.empty()) {
        out.reset(size());
        StampContext ctx{t, x, out, limiting, transient_, useIc_, initPoint_, liveModes_,
                         srcScale_, options.gmin, options.vt()};
        for (auto& d : devices_) d->stamp(ctx);
        return;
    }
    if (!measured_) {
        stampMeasured(t, x, limiting, out);
        return;
    }
    const double vt = options.vt();
    out.G.resize(blocks_.back().gLast);
    out.C.resize(blocks_.back().cLast);
    std::atomic<size_t> next{0};
    pool_->run([&](int) {
        for (size_t k; (k = next.fetch_add(1, std::memory_order_relaxed)) < blocks_.size();) {
            Block& b = blocks_[k];
            if (b.rowHi >= b.rowLo) {
                b.part.f.segment(b.rowLo, b.rowHi - b.rowLo + 1).setZero();
                b.part.q.segment(b.rowLo, b.rowHi - b.rowLo + 1).setZero();
            }
            b.part.limited = false;
            StampContext ctx{t, x, b.part, limiting, transient_, useIc_, initPoint_, liveModes_,
                             srcScale_, options.gmin, vt};
            ctx.gNext = out.G.data() + b.gFirst;
            ctx.gEnd = out.G.data() + b.gLast;
            ctx.cNext = out.C.data() + b.cFirst;
            ctx.cEnd = out.C.data() + b.cLast;
            for (size_t i = b.first; i < b.last; ++i) devices_[i]->stamp(ctx);
            // Entries outside of the rows known so far, or another number of
            // matrix entries: the block has to be measured again.
            b.mismatch = ctx.overflow || ctx.gNext != ctx.gEnd || ctx.cNext != ctx.cEnd ||
                         ctx.rowLo < b.rowLo || ctx.rowHi > b.rowHi;
            if (ctx.rowLo < b.rowLo) b.rowLo = ctx.rowLo;
            if (ctx.rowHi > b.rowHi) b.rowHi = ctx.rowHi;
        }
    });
    for (const Block& b : blocks_) {
        if (!b.mismatch) continue;
        // The components stamp differently than when the sections were
        // measured (another analysis mode): do this evaluation again in
        // sequence, which measures them anew. Limiters have seen the same
        // point twice, which does not change them.
        stampMeasured(t, x, limiting, out);
        return;
    }
    out.f.setZero(size());
    out.q.setZero(size());
    out.limited = false;
    for (const Block& b : blocks_) {
        if (b.rowHi >= b.rowLo) {
            const int count = b.rowHi - b.rowLo + 1;
            out.f.segment(b.rowLo, count) += b.part.f.segment(b.rowLo, count);
            out.q.segment(b.rowLo, count) += b.part.q.segment(b.rowLo, count);
        }
        out.limited = out.limited || b.part.limited;
    }
}

void Circuit::evaluate(double t, const Vector& x, LimitMode limiting, Evaluation& out) {
    stampAll(t, x, limiting, out);

    // Shunt from every node to ground (gmin stepping) and damping towards a
    // previous solution (pseudo-transient). The entry is always stamped, also
    // with value 0, so that the matrix pattern stays fixed.
    for (int i = 0; i < size(); ++i) {
        if (unknowns_[i].kind != UnknownKind::Voltage) continue;
        out.f[i] += gshunt_ * x[i];
        if (gdamp_ > 0.0) out.f[i] += gdamp_ * (x[i] - anchor_[i]);
        out.G.emplace_back(i, i, gshunt_ + gdamp_);
    }
}

double Circuit::absTol(int i) const {
    const Unknown& u = unknowns_[i];
    if (u.abstol > 0.0) return u.abstol;
    switch (u.kind) {
        case UnknownKind::Voltage: return options.vntol;
        case UnknownKind::Current: return options.abstol;
        case UnknownKind::Variable: break;
    }
    return options.vartol;
}

double Circuit::maxStep(double /*t*/) const {
    double best = std::numeric_limits<double>::infinity();
    if (transient_) {
        for (const auto& d : devices_) best = std::min(best, d->maxStep());
        // A delayed value has to lie in the past of the step that uses it.
        for (const auto& w : delays_)
            if (const auto line = w.lock()) best = std::min(best, line->td);
    }
    return best;
}

void Circuit::resetDevices() {
    for (auto& d : devices_) d->reset();
    for (const auto& w : delays_)
        if (const auto line = w.lock()) line->history.clear();
}

void Circuit::acceptStep(double t, const Vector& x, bool start) {
    for (auto& d : devices_) d->acceptStep(t, x, start);
    delays_.erase(std::remove_if(delays_.begin(), delays_.end(), [](const auto& w) { return w.expired(); }),
                  delays_.end());
    for (const auto& w : delays_)
        if (const auto line = w.lock()) line->record(t, x, start);
}

void Circuit::noteDiscontinuity(double t) {
    for (auto& d : devices_) d->noteDiscontinuity(t);
    for (const auto& w : delays_)
        if (const auto line = w.lock()) line->history.mark(t);
}

std::string Circuit::equationText(int row) const {
    auto it = equationText_.find(row);
    if (it != equationText_.end()) return "\"" + it->second + "\"";
    const Unknown& u = unknowns_[row];
    if (u.kind == UnknownKind::Voltage) return "the current balance of node " + u.label.substr(2, u.label.size() - 3);
    return "the branch equation of " + u.label;
}

bool Circuit::hasHistory() const {
    for (const auto& d : devices_)
        if (d->usesHistory()) return true;
    for (const auto& w : delays_)
        if (!w.expired()) return true;
    return false;
}

void Circuit::eventFunctions(double t, const Vector& x, Vector& g) {
    g.resize(eventCount_);
    int offset = 0;
    for (const auto& d : devices_) {
        const int count = d->eventCount();
        if (count > 0) d->eventFunctions(t, x, g.data() + offset);
        offset += count;
    }
}

bool Circuit::eventUpdate(double t, Vector& x, bool initial) {
    bool changed = false;
    for (auto& d : devices_) changed = d->eventUpdate(t, x, initial) || changed;
    return changed;
}

double Circuit::nextBreakpoint(double t) const {
    double best = std::numeric_limits<double>::infinity();
    for (const auto& d : devices_) best = std::min(best, d->nextBreakpoint(t));
    for (const auto& w : delays_)
        if (const auto line = w.lock()) best = std::min(best, line->history.nextBreakpoint(t));
    return best;
}

}  // namespace esim
