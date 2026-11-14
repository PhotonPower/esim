#include "circuit/Circuit.hpp"

#include <algorithm>
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
    for (const auto& [name, value] : ic) {
        const int idx = findNode(name);
        if (idx >= 0) x0[idx] = value;
    }
    Evaluation ev;
    useIc_ = true;
    evaluate(0.0, x0, LimitMode::Off, ev);
    useIc_ = false;
    q0 = ev.q;
}

void Circuit::evaluate(double t, const Vector& x, LimitMode limiting, Evaluation& out) {
    out.reset(size());
    StampContext ctx{t, x, out, limiting, transient_, useIc_, initPoint_, liveModes_,
                     srcScale_, options.gmin, options.vt()};
    for (auto& d : devices_) d->stamp(ctx);

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
    if (transient_)
        for (const auto& d : devices_) best = std::min(best, d->maxStep());
    return best;
}

void Circuit::resetDevices() {
    for (auto& d : devices_) d->reset();
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
    return best;
}

}  // namespace esim
