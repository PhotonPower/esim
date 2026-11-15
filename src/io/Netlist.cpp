#include "io/Netlist.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "circuit/Waveform.hpp"
#include "circuit/devices/Behavioral.hpp"
#include "circuit/devices/Bjt.hpp"
#include "circuit/devices/Diode.hpp"
#include "circuit/devices/Equations.hpp"
#include "circuit/devices/Jfet.hpp"
#include "circuit/devices/TransmissionLine.hpp"
#include "circuit/devices/Mosfet.hpp"
#include "circuit/devices/Passive.hpp"
#include "circuit/devices/Sources.hpp"
#include "circuit/devices/Switching.hpp"
#include "expr/Expression.hpp"
#include "io/Units.hpp"

namespace esim {

using json = nlohmann::json;
using ParamMap = std::map<std::string, double>;

double jsonNumber(const json& v, const std::string& where, const ParamMap* parameters) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        const std::string text = v.get<std::string>();
        try {
            return parseValue(text);  // "4.7k", "100nF"
        } catch (const std::invalid_argument&) {
        }
        try {
            static const ParamMap none;
            return expr::evalConstant(text, parameters ? *parameters : none);  // "2*R0"
        } catch (const std::runtime_error& e) {
            throw std::runtime_error(where + ": " + e.what());
        }
    }
    throw std::runtime_error(where + ": expected a number or a string like \"4.7k\"");
}

namespace {

const double kInf = std::numeric_limits<double>::infinity();

bool g_warnings = true;  // hints about unknown keys, see parseNetlist()

std::ostream& warn() {
    static std::ostream discard(nullptr);
    return g_warnings ? std::cerr << "warning: " : discard;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Parameter set of one JSON object. Keys are case insensitive; keys that are
// never read are reported, which catches typos like "restistance".
class Params {
public:
    Params(const json& obj, std::string where, const ParamMap* parameters)
        : where_(std::move(where)), parameters_(parameters) {
        if (!obj.is_object()) throw std::runtime_error(where_ + ": expected a JSON object");
        for (const auto& [k, v] : obj.items()) j_[lower(k)] = v;
    }

    // Add defaults (e.g. from a model) without overriding existing keys.
    void addDefaults(const json& defaults) {
        for (const auto& [k, v] : defaults.items())
            if (!j_.contains(lower(k))) j_[lower(k)] = v;
    }

    bool has(const std::string& k) const { return j_.contains(k); }

    const json& raw(const std::string& k) {
        if (!has(k)) throw std::runtime_error(where_ + ": missing parameter \"" + k + "\"");
        used_.insert(k);
        return j_.at(k);
    }
    double num(const std::string& k) {
        return jsonNumber(raw(k), where_ + ", \"" + k + "\"", parameters_);
    }
    double num(const std::string& k, double def) { return has(k) ? num(k) : def; }
    std::optional<double> optNum(const std::string& k) {
        return has(k) ? std::optional<double>(num(k)) : std::nullopt;
    }
    // First key of the list that is present.
    double numAny(std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (has(k)) return num(k);
        throw std::runtime_error(where_ + ": missing parameter \"" + *keys.begin() + "\"");
    }
    std::string str(const std::string& k) {
        const json& v = raw(k);
        if (!v.is_string()) throw std::runtime_error(where_ + ": \"" + k + "\" must be a string");
        return v.get<std::string>();
    }
    std::string str(const std::string& k, const std::string& def) { return has(k) ? str(k) : def; }
    bool flag(const std::string& k, bool def) {
        if (!has(k)) return def;
        const json& v = raw(k);
        if (!v.is_boolean())
            throw std::runtime_error(where_ + ": \"" + k + "\" must be true or false");
        return v.get<bool>();
    }
    void ignore(const std::string& k) { used_.insert(k); }

    void warnUnused() const {
        for (const auto& [k, v] : j_.items())
            if (!used_.count(k))
                warn() << where_ << ": unknown parameter \"" << k
                          << "\" ignored\n";
    }

    const std::string& where() const { return where_; }
    const ParamMap* parameters() const { return parameters_; }

private:
    json j_ = json::object();
    std::string where_;
    const ParamMap* parameters_;
    std::set<std::string> used_;
};

std::unique_ptr<Waveform> parseWave(const json& j, const std::string& where,
                                    const ParamMap* parameters) {
    Params p(j, where + ", wave", parameters);
    const std::string type = lower(p.str("type"));
    std::unique_ptr<Waveform> w;
    if (type == "dc") {
        w = std::make_unique<DcWave>(p.num("value"));
    } else if (type == "sin" || type == "sine") {
        w = std::make_unique<SinWave>(p.num("offset", 0.0), p.num("amplitude"), p.num("freq"),
                                      p.num("delay", 0.0), p.num("damping", 0.0),
                                      p.num("phase", 0.0));
    } else if (type == "pulse") {
        const double v1 = p.num("v1"), v2 = p.num("v2");
        const double delay = p.num("delay", 0.0);
        const double width = p.num("width", kInf);
        const double period = p.num("period", kInf);
        // Edges must not be vertical; default: 0.1 % of the period or width.
        const double ref = std::isfinite(period) ? period : std::isfinite(width) ? width : 1e-6;
        const double rise = p.num("rise", 1e-3 * ref);
        const double fall = p.num("fall", 1e-3 * ref);
        w = std::make_unique<PulseWave>(v1, v2, delay, rise, fall, width, period);
    } else if (type == "pwl") {
        std::vector<std::pair<double, double>> pts;
        const json& arr = p.raw("points");
        if (!arr.is_array()) throw std::runtime_error(p.where() + ": \"points\" must be an array");
        for (const json& pt : arr) {
            if (!pt.is_array() || pt.size() != 2)
                throw std::runtime_error(p.where() + ": each point must be [time, value]");
            pts.emplace_back(jsonNumber(pt[0], p.where(), parameters),
                             jsonNumber(pt[1], p.where(), parameters));
        }
        w = std::make_unique<PwlWave>(std::move(pts));
    } else {
        throw std::runtime_error(p.where() + ": unknown type \"" + type +
                                 "\" (dc, sin, pulse, pwl)");
    }
    p.warnUnused();
    return w;
}

// Canonical one-letter kind of a component type; 0 if unknown.
char componentKind(const std::string& type) {
    static const std::map<std::string, char> kinds = {
        {"r", 'r'}, {"resistor", 'r'},
        {"c", 'c'}, {"capacitor", 'c'},
        {"l", 'l'}, {"inductor", 'l'},
        {"v", 'v'}, {"vsource", 'v'},
        {"i", 'i'}, {"isource", 'i'},
        {"e", 'e'}, {"vcvs", 'e'},
        {"g", 'g'}, {"vccs", 'g'},
        {"f", 'f'}, {"cccs", 'f'},
        {"h", 'h'}, {"ccvs", 'h'},
        {"d", 'd'}, {"diode", 'd'},
        {"q", 'q'}, {"bjt", 'q'}, {"npn", 'q'}, {"pnp", 'q'},
        {"m", 'm'}, {"mosfet", 'm'}, {"nmos", 'm'}, {"pmos", 'm'},
        {"b", 'b'}, {"behavioral", 'b'},
        {"s", 's'}, {"switch", 's'},
        {"ideal_diode", 'y'},
        {"j", 'j'}, {"jfet", 'j'}, {"njf", 'j'}, {"pjf", 'j'},
        {"k", 'k'}, {"coupling", 'k'},
        {"t", 't'}, {"tline", 't'},
        {"x", 'x'}, {"subcircuit", 'x'},
    };
    auto it = kinds.find(type);
    return it == kinds.end() ? 0 : it->second;
}

void configureSource(IndependentSource& src, Params& p) {
    if (p.has("dc")) src.setDc(p.num("dc"));
    else if (p.has("value")) src.setDc(p.num("value"));
    if (p.has("wave")) src.setWave(parseWave(p.raw("wave"), p.where(), p.parameters()));
    if (p.has("ac")) src.setAc(p.num("ac"), p.num("acphase", 0.0));
    else p.num("acphase", 0.0);
}

// Polarity (+1 / -1) from the component type or the model type.
int polarity(const std::string& type, const std::string& modelType, const char* pos,
             const char* neg, const std::string& where) {
    for (const std::string& t : {type, modelType}) {
        if (t == pos) return 1;
        if (t == neg) return -1;
    }
    throw std::runtime_error(where + ": polarity unknown, use type \"" + pos + "\" or \"" + neg +
                             "\" (on the component or its model)");
}

// Where a component, an expression or an equation lives: at the top level or
// inside an instance of a subcircuit. Subcircuits are flattened while the
// input is read: everything inside the instance "U1" gets the prefix "U1.",
// its ports are replaced by the nodes it is connected to.
struct Scope {
    std::string prefix;                        // "" or "U1." or "U1.U2."
    std::map<std::string, std::string> ports;  // port name -> node name outside
    ParamMap params;                           // visible constants: global and local
    std::set<std::string> localParams;         // parameters of the subcircuit
    std::set<std::string> localVars;           // variables defined in this scope
    int depth = 0;
};

// Everything collected while the input is read.
struct Builder {
    Circuit& ckt;
    std::map<std::string, json> models;
    std::map<std::string, json> subcircuits;
    std::vector<VariableSpec> variables;
    std::vector<EquationSpec> equations;
    std::vector<WhenSpec> whens;
    std::vector<std::string> instancePrefixes;
};

std::string scopedNode(const Scope& scope, const std::string& name) {
    if (Circuit::isGroundName(name)) return name;  // ground is global
    auto it = scope.ports.find(name);
    return it != scope.ports.end() ? it->second : scope.prefix + name;
}

// Translate the names of an expression written inside a scope: nodes and
// components get the prefix, local parameters their value.
expr::AstPtr scopedAst(const Scope& scope, const expr::AstPtr& ast) {
    if (scope.prefix.empty()) return ast;
    return expr::mapLeaves(ast, [&](const expr::Ast& n) -> expr::AstPtr {
        if (n.kind == expr::Ast::Kind::Probe) {
            auto copy = std::make_shared<expr::Ast>(n);
            for (std::string& nm : copy->names)
                nm = n.name == "v" ? scopedNode(scope, nm) : scope.prefix + nm;
            return copy;
        }
        if (scope.localVars.count(n.name)) {
            auto copy = std::make_shared<expr::Ast>(n);
            copy->name = scope.prefix + n.name;
            return copy;
        }
        if (scope.localParams.count(n.name)) return expr::makeNumber(scope.params.at(n.name));
        return nullptr;
    });
}

expr::AstPtr scopedExpression(const Scope& scope, const std::string& text) {
    return scopedAst(scope, expr::parseExpression(text));
}

ParamMap resolveParameters(const json& j, ParamMap known, const std::string& what);
void declareVariables(Scope& scope, const json& def);
void addEquations(Builder& b, Scope& scope, const json& def, const std::string& what);
void instantiate(Builder& b, const Scope& scope, const std::string& name, Params& p,
                 const std::vector<std::string>& nodeNames);

void addComponent(Builder& b, const Scope& scope, const json& spec, int index) {
    Circuit& ckt = b.ckt;
    const std::map<std::string, json>& models = b.models;
    const std::string at = scope.prefix.empty() ? "" : " in '" + scope.prefix.substr(0, scope.prefix.size() - 1) + "'";
    if (!spec.is_object())
        throw std::runtime_error("components[" + std::to_string(index) + "]" + at +
                                 ": expected an object");

    std::string name;
    if (spec.contains("name") && spec["name"].is_string()) name = spec["name"].get<std::string>();
    Params p(spec, name.empty() ? "components[" + std::to_string(index) + "]" + at
                                : "component '" + scope.prefix + name + "'",
             &scope.params);
    p.ignore("name");

    const std::string type = lower(p.str("type"));
    const char kind = componentKind(type);
    if (!kind) throw std::runtime_error(p.where() + ": unknown type \"" + type + "\"");
    if (name.empty()) name = std::string(1, static_cast<char>(std::toupper(kind))) +
                             std::to_string(index + 1);
    name = scope.prefix + name;

    // Model parameters act as defaults for the component's own parameters.
    std::string modelType;
    if (p.has("model")) {
        const std::string modelName = p.str("model");
        auto it = models.find(modelName);
        if (it == models.end())
            throw std::runtime_error(p.where() + ": model \"" + modelName + "\" is not defined");
        json defaults = json::object();
        for (const auto& [k, v] : it->second.items()) {
            if (lower(k) == "type") modelType = lower(v.is_string() ? v.get<std::string>() : "");
            else defaults[k] = v;
        }
        if (!modelType.empty() && componentKind(modelType) != kind)
            throw std::runtime_error(p.where() + ": model \"" + modelName + "\" has type \"" +
                                     modelType + "\", which does not fit a \"" + type + "\"");
        p.addDefaults(defaults);
    }

    // Nodes (a coupling refers to inductors instead)
    std::vector<int> n;
    std::vector<std::string> nodeNames;
    if (kind != 'k') {
        const json& nodeList = p.raw("nodes");
        if (!nodeList.is_array())
            throw std::runtime_error(p.where() + ": \"nodes\" must be an array");
        for (const json& nd : nodeList) {
            std::string raw;
            if (nd.is_string()) raw = nd.get<std::string>();
            else if (nd.is_number_integer()) raw = std::to_string(nd.get<long long>());
            else throw std::runtime_error(p.where() + ": node names must be strings or integers");
            nodeNames.push_back(scopedNode(scope, raw));
            n.push_back(ckt.node(nodeNames.back()));
        }
    }
    auto needNodes = [&](size_t lo, size_t hi, const char* what) {
        if (n.size() < lo || n.size() > hi)
            throw std::runtime_error(p.where() + ": expected nodes " + what);
    };

    switch (kind) {
        case 'r':
            needNodes(2, 2, "[a, b]");
            ckt.add(std::make_unique<Resistor>(name, n[0], n[1], p.numAny({"value", "r"})));
            break;
        case 'c':
            needNodes(2, 2, "[a, b]");
            ckt.add(std::make_unique<Capacitor>(name, n[0], n[1], p.numAny({"value", "c"}),
                                                p.optNum("ic")));
            break;
        case 'l':
            needNodes(2, 2, "[a, b]");
            ckt.add(std::make_unique<Inductor>(name, n[0], n[1], p.numAny({"value", "l"}),
                                               p.optNum("ic")));
            break;
        case 'v': {
            needNodes(2, 2, "[plus, minus]");
            auto src = std::make_unique<VoltageSource>(name, n[0], n[1]);
            configureSource(*src, p);
            ckt.add(std::move(src));
            break;
        }
        case 'i': {
            needNodes(2, 2, "[from, to]");
            auto src = std::make_unique<CurrentSource>(name, n[0], n[1]);
            configureSource(*src, p);
            ckt.add(std::move(src));
            break;
        }
        case 'e':
            needNodes(4, 4, "[plus, minus, ctrl_plus, ctrl_minus]");
            ckt.add(std::make_unique<Vcvs>(name, n[0], n[1], n[2], n[3], p.num("gain")));
            break;
        case 'g':
            needNodes(4, 4, "[from, to, ctrl_plus, ctrl_minus]");
            ckt.add(std::make_unique<Vccs>(name, n[0], n[1], n[2], n[3], p.numAny({"gm", "gain"})));
            break;
        case 'f':
            needNodes(2, 2, "[from, to]");
            ckt.add(std::make_unique<Cccs>(name, n[0], n[1], scope.prefix + p.str("control"),
                                           p.num("gain")));
            break;
        case 'h':
            needNodes(2, 2, "[plus, minus]");
            ckt.add(std::make_unique<Ccvs>(name, n[0], n[1], scope.prefix + p.str("control"),
                                           p.numAny({"r", "gain"})));
            break;
        case 'd': {
            needNodes(2, 2, "[anode, cathode]");
            DiodeParams d;
            d.is = p.num("is", d.is);
            d.n = p.num("n", d.n);
            d.rs = p.num("rs", d.rs);
            d.cj0 = p.num("cj0", d.cj0);
            d.vj = p.num("vj", d.vj);
            d.m = p.num("m", d.m);
            d.fc = p.num("fc", d.fc);
            d.tt = p.num("tt", d.tt);
            d.bv = p.num("bv", d.bv);
            d.ibv = p.num("ibv", d.ibv);
            d.eg = p.num("eg", d.eg);
            d.xti = p.num("xti", d.xti);
            d.kf = p.num("kf", d.kf);
            d.af = p.num("af", d.af);
            ckt.add(std::make_unique<Diode>(name, n[0], n[1], d));
            break;
        }
        case 'q': {
            needNodes(3, 3, "[collector, base, emitter]");
            BjtParams b;
            b.is = p.num("is", b.is);
            b.bf = p.num("bf", b.bf);
            b.br = p.num("br", b.br);
            b.nf = p.num("nf", b.nf);
            b.nr = p.num("nr", b.nr);
            b.vaf = p.num("vaf", b.vaf);
            b.var = p.num("var", b.var);
            b.cje = p.num("cje", b.cje);
            b.vje = p.num("vje", b.vje);
            b.mje = p.num("mje", b.mje);
            b.cjc = p.num("cjc", b.cjc);
            b.vjc = p.num("vjc", b.vjc);
            b.mjc = p.num("mjc", b.mjc);
            b.tf = p.num("tf", b.tf);
            b.tr = p.num("tr", b.tr);
            b.fc = p.num("fc", b.fc);
            b.ikf = p.num("ikf", b.ikf);
            b.ikr = p.num("ikr", b.ikr);
            b.ise = p.num("ise", b.ise);
            b.ne = p.num("ne", b.ne);
            b.isc = p.num("isc", b.isc);
            b.nc = p.num("nc", b.nc);
            b.rb = p.num("rb", b.rb);
            b.rc = p.num("rc", b.rc);
            b.re = p.num("re", b.re);
            b.eg = p.num("eg", b.eg);
            b.xti = p.num("xti", b.xti);
            b.xtb = p.num("xtb", b.xtb);
            b.kf = p.num("kf", b.kf);
            b.af = p.num("af", b.af);
            const int pol = polarity(type, modelType, "npn", "pnp", p.where());
            ckt.add(std::make_unique<Bjt>(name, n[0], n[1], n[2], pol, b));
            break;
        }
        case 's': {
            // Either [a, b] with a "control" expression, or the SPICE form
            // [a, b, ctrl_plus, ctrl_minus] controlled by v(ctrl_plus, ctrl_minus).
            needNodes(2, 4, "[a, b] or [a, b, ctrl_plus, ctrl_minus]");
            if (n.size() == 3) needNodes(0, 0, "[a, b] or [a, b, ctrl_plus, ctrl_minus]");
            if ((n.size() == 4) == p.has("control"))
                throw std::runtime_error(p.where() +
                                         ": give either a \"control\" expression or two "
                                         "controlling nodes");
            expr::AstPtr control;
            if (n.size() == 4) {
                auto probe = std::make_shared<expr::Ast>();
                probe->kind = expr::Ast::Kind::Probe;
                probe->name = "v";
                probe->names = {nodeNames[2], nodeNames[3]};
                control = probe;
            } else {
                const json& e = p.raw("control");
                if (!e.is_string())
                    throw std::runtime_error(p.where() + ": \"control\" must be an expression string");
                try {
                    control = scopedExpression(scope, e.get<std::string>());
                } catch (const std::runtime_error& err) {
                    throw std::runtime_error(p.where() + ", control: " + err.what());
                }
            }
            SwitchParams sw;
            sw.ron = p.num("ron", sw.ron);
            sw.roff = p.num("roff", sw.roff);
            sw.threshold = p.has("vt") ? p.num("vt") : p.num("threshold", sw.threshold);
            sw.hysteresis = p.has("vh") ? p.num("vh") : p.num("hysteresis", sw.hysteresis);
            const std::string initial = lower(p.str("initial", "off"));
            if (initial != "on" && initial != "off")
                throw std::runtime_error(p.where() + ": \"initial\" must be \"on\" or \"off\"");
            sw.initiallyOn = initial == "on";
            ckt.add(std::make_unique<Switch>(name, n[0], n[1], control, sw));
            break;
        }
        case 'k': {
            const json& ls = p.raw("inductors");
            if (!ls.is_array() || ls.size() != 2 || !ls[0].is_string() || !ls[1].is_string())
                throw std::runtime_error(p.where() +
                                         ": \"inductors\" must name two inductors, e.g. [\"L1\", \"L2\"]");
            ckt.add(std::make_unique<MutualInductance>(name, scope.prefix + ls[0].get<std::string>(),
                                                       scope.prefix + ls[1].get<std::string>(),
                                                       p.num("k")));
            break;
        }
        case 't': {
            needNodes(4, 4, "[port1+, port1-, port2+, port2-]");
            double td;
            if (p.has("td")) td = p.num("td");
            else {  // electrical length at a frequency: "f" and "nl" (wavelengths), as in SPICE
                if (!p.has("f")) throw std::runtime_error(p.where() + ": needs \"td\", or \"f\" and \"nl\"");
                td = p.num("nl", 0.25) / p.num("f");
            }
            ckt.add(std::make_unique<TransmissionLine>(name, n[0], n[1], n[2], n[3], p.num("z0"), td));
            break;
        }
        case 'x':
            instantiate(b, scope, name, p, nodeNames);
            break;
        case 'y': {
            needNodes(2, 2, "[anode, cathode]");
            IdealDiodeParams d;
            d.vf = p.num("vf", d.vf);
            d.ron = p.num("ron", d.ron);
            d.roff = p.num("roff", d.roff);
            ckt.add(std::make_unique<IdealDiode>(name, n[0], n[1], d));
            break;
        }
        case 'b': {
            needNodes(2, 2, "[plus, minus]");
            // Exactly one of "i", "v", "q" holds the defining expression.
            const std::pair<const char*, BehavioralSource::Kind> forms[] = {
                {"i", BehavioralSource::Kind::Current},
                {"v", BehavioralSource::Kind::Voltage},
                {"q", BehavioralSource::Kind::Charge},
                {"phi", BehavioralSource::Kind::Flux}};
            int given = 0;
            for (const auto& [key, kindOf] : forms) {
                if (!p.has(key)) continue;
                ++given;
                const json& e = p.raw(key);
                if (!e.is_string() && !e.is_number())
                    throw std::runtime_error(p.where() + ": \"" + key +
                                             "\" must be an expression string");
                const std::string text = e.is_string() ? e.get<std::string>() : e.dump();
                expr::AstPtr ast;
                try {
                    ast = scopedExpression(scope, text);
                } catch (const std::runtime_error& err) {
                    throw std::runtime_error(p.where() + ": " + err.what());
                }
                ckt.add(std::make_unique<BehavioralSource>(name, n[0], n[1], kindOf, text, ast));
            }
            if (given != 1)
                throw std::runtime_error(p.where() +
                                         ": needs exactly one of \"i\", \"v\", \"q\" or \"phi\" "
                                         "(expression for current, voltage, charge or flux)");
            break;
        }
        case 'j': {
            needNodes(3, 3, "[drain, gate, source]");
            JfetParams j;
            j.vto = p.num("vto", j.vto);
            j.beta = p.num("beta", j.beta);
            j.lambda = p.num("lambda", j.lambda);
            j.is = p.num("is", j.is);
            j.cgs = p.num("cgs", j.cgs);
            j.cgd = p.num("cgd", j.cgd);
            j.pb = p.num("pb", j.pb);
            j.fc = p.num("fc", j.fc);
            j.kf = p.num("kf", j.kf);
            j.af = p.num("af", j.af);
            const int pol = polarity(type, modelType, "njf", "pjf", p.where());
            ckt.add(std::make_unique<Jfet>(name, n[0], n[1], n[2], pol, j));
            break;
        }
        case 'm': {
            needNodes(3, 4, "[drain, gate, source] or [drain, gate, source, bulk]");
            MosfetParams m;
            m.vto = p.num("vto", m.vto);
            m.kp = p.num("kp", m.kp);
            m.gamma = p.num("gamma", m.gamma);
            m.phi = p.num("phi", m.phi);
            m.lambda = p.num("lambda", m.lambda);
            m.w = p.num("w", m.w);
            m.l = p.num("l", m.l);
            m.cgso = p.num("cgso", m.cgso);
            m.cgdo = p.num("cgdo", m.cgdo);
            m.cgbo = p.num("cgbo", m.cgbo);
            m.cbd = p.num("cbd", m.cbd);
            m.cbs = p.num("cbs", m.cbs);
            m.pb = p.num("pb", m.pb);
            m.mj = p.num("mj", m.mj);
            m.fc = p.num("fc", m.fc);
            m.is = p.num("is", m.is);
            m.tox = p.num("tox", m.tox);
            m.tcv = p.num("tcv", m.tcv);
            m.kf = p.num("kf", m.kf);
            m.af = p.num("af", m.af);
            m.theta = p.num("theta", m.theta);
            m.ucrit = p.num("ucrit", m.ucrit);
            if (p.has("level")) {  // 1 (Shichman-Hodges) or "ekv"
                const json& level = p.raw("level");
                if (level.is_string() && lower(level.get<std::string>()) == "ekv") m.ekv = true;
                else if (!(level.is_number() && level.get<double>() == 1.0))
                    throw std::runtime_error(p.where() + ": \"level\" must be 1 or \"ekv\"");
            }
            if (!m.ekv && (m.theta != 0.0 || m.ucrit != 0.0))
                warn() << p.where() << ": theta and ucrit only act with \"level\": \"ekv\"\n";
            const int pol = polarity(type, modelType, "nmos", "pmos", p.where());
            const int bulk = n.size() == 4 ? n[3] : n[2];
            ckt.add(std::make_unique<Mosfet>(name, n[0], n[1], n[2], bulk, pol, m));
            break;
        }
    }
    p.warnUnused();
}

void parseOptions(const json& j, Netlist& nl) {
    Params p(j, "options", &nl.circuit->parameters);
    CircuitOptions& c = nl.circuit->options;
    c.gmin = p.num("gmin", c.gmin);
    c.vntol = p.num("vntol", c.vntol);
    c.abstol = p.num("abstol", c.abstol);
    c.vartol = p.num("vartol", c.vartol);
    c.temp = p.num("temp", c.temp);
    c.tnom = p.num("tnom", c.tnom);
    SimOptions& s = nl.options;
    s.reltol = p.num("reltol", s.reltol);
    s.trtol = p.num("trtol", s.trtol);
    s.lteFloor = p.num("lte_floor", s.lteFloor);
    s.maxIterDc = static_cast<int>(p.num("maxiter_dc", s.maxIterDc));
    s.maxIterTran = static_cast<int>(p.num("maxiter_tran", s.maxIterTran));
    p.warnUnused();
}

std::map<std::string, double> parseNodeValues(const json& j, const std::string& where,
                                              const ParamMap* parameters) {
    if (!j.is_object())
        throw std::runtime_error(where + ": expected an object {\"node\": value, ...}");
    std::map<std::string, double> out;
    for (const auto& [k, v] : j.items())
        out[k] = jsonNumber(v, where + ", \"" + k + "\"", parameters);
    return out;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

// Named constants. A value is a number, a string like "4.7k" or an expression
// of other parameters; the order of definition does not matter. `known` holds
// the constants that are already fixed (global parameters, overridden values);
// names defined there are not evaluated again.
ParamMap resolveParameters(const json& j, ParamMap known, const std::string& what) {
    if (!j.is_object())
        throw std::runtime_error(what + ": expected an object {\"name\": value, ...}");
    ParamMap out = std::move(known);
    std::map<std::string, json> pending;
    for (const auto& [name, v] : j.items()) {
        if (!isIdentifier(name))
            throw std::runtime_error(what + " \"" + name +
                                     "\": names consist of letters, digits and _");
        pending[name] = v;
    }
    while (!pending.empty()) {
        bool progress = false;
        std::string lastError;
        for (auto it = pending.begin(); it != pending.end();) {
            try {
                out[it->first] = jsonNumber(it->second, what + " \"" + it->first + "\"", &out);
                it = pending.erase(it);
                progress = true;
            } catch (const std::runtime_error& e) {
                lastError = e.what();
                ++it;
            }
        }
        if (!progress) throw std::runtime_error(lastError + " (or a circular definition)");
    }
    return out;
}

// "variables": {"x": 1, "y": {"init": 0, "abstol": 1e-10}, "z": {"guess": 2}}
// The names are registered in the scope and get its prefix.
std::vector<VariableSpec> parseVariables(const json& j, Scope& scope, const std::string& what) {
    if (!j.is_object())
        throw std::runtime_error(what + ": expected an object {\"name\": initial value, ...}");
    std::vector<VariableSpec> out;
    for (const auto& [name, v] : j.items()) {
        const std::string where = "variable \"" + scope.prefix + name + "\"";
        if (!isIdentifier(name))
            throw std::runtime_error(where + ": names consist of letters, digits and _");
        if (name == "t" || name == "time" || name == "pi")
            throw std::runtime_error(where + ": this name is reserved");
        if (scope.params.count(name))
            throw std::runtime_error(where + ": there is a parameter with the same name");
        VariableSpec spec;
        spec.name = scope.prefix + name;
        if (v.is_object()) {
            Params p(v, where, &scope.params);
            if (p.has("init") + p.has("guess") + p.has("discrete") > 1)
                throw std::runtime_error(
                    where + ": give only one of \"init\", \"guess\" and \"discrete\"");
            if (p.has("discrete")) {
                spec.start = p.num("discrete");
                spec.hold = true;
                spec.discrete = true;
            } else if (p.has("init")) {
                spec.start = p.num("init");
                spec.hold = true;
            } else {
                spec.start = p.num("guess", 0.0);
            }
            spec.abstol = p.num("abstol", -1.0);
            p.warnUnused();
        } else if (!v.is_null()) {
            spec.start = jsonNumber(v, where, &scope.params);
            spec.hold = true;
        }
        scope.localVars.insert(name);
        out.push_back(spec);
    }
    return out;
}

// "events": [{"when": "h < 0", "set": {"v": "-0.8*v"}},
//            {"every": "1m", "set": {"u": "u + ki*e"}},
//            {"at": [1, 2.5], "set": {"v(c)": 0}}]
std::vector<WhenSpec> parseEvents(const json& j, const Scope& scope, const std::string& what) {
    if (!j.is_array()) throw std::runtime_error(what + ": expected an array");
    std::vector<WhenSpec> out;
    for (const json& e : j) {
        if (!e.is_object())
            throw std::runtime_error(what + ": each entry must be an object");
        const int triggers = e.contains("when") + e.contains("every") + e.contains("at");
        if (triggers != 1)
            throw std::runtime_error(what + ": each entry needs exactly one of \"when\" "
                                            "(condition), \"every\" (period) or \"at\" (times)");
        for (const auto& [k, v] : e.items())
            if (k != "when" && k != "every" && k != "start" && k != "at" && k != "set")
                warn() << what << ": unknown key \"" << k << "\" ignored\n";

        WhenSpec w;
        if (e.contains("when")) {
            if (!e["when"].is_string())
                throw std::runtime_error(what + ": \"when\" must be a condition like \"x > 1\"");
            w.text = e["when"].get<std::string>();
            try {
                w.condition = scopedExpression(scope, w.text);
            } catch (const std::runtime_error& err) {
                throw std::runtime_error(what + ": " + err.what());
            }
        } else if (e.contains("every")) {
            w.period = jsonNumber(e["every"], what + ", \"every\"", &scope.params);
            if (!(w.period > 0.0)) throw std::runtime_error(what + ": \"every\" must be > 0");
            w.start = e.contains("start") ? jsonNumber(e["start"], what + ", \"start\"", &scope.params)
                                          : w.period;
            w.text = "every " + e["every"].dump();
        } else {
            const json& at = e["at"];
            if (at.is_array())
                for (const json& t : at) w.times.push_back(jsonNumber(t, what + ", \"at\"", &scope.params));
            else
                w.times.push_back(jsonNumber(at, what + ", \"at\"", &scope.params));
            std::sort(w.times.begin(), w.times.end());
            w.text = "at " + at.dump();
        }

        if (!e.contains("set") || !e["set"].is_object() || e["set"].empty())
            throw std::runtime_error(what + ": \"" + w.text +
                                     "\" needs assignments \"set\": {\"variable\": \"expression\"}");
        for (const auto& [name, value] : e["set"].items()) {
            if (!value.is_string() && !value.is_number())
                throw std::runtime_error(what + ": the new value of '" + name +
                                         "' must be a number or an expression");
            WhenSpec::Assignment a;
            // Target: a variable, or v(node) / i(component) of the circuit.
            std::string target;
            for (char ch : name)
                if (!std::isspace(static_cast<unsigned char>(ch))) target += ch;
            if (target.size() > 3 && target[1] == '(' && target.back() == ')' &&
                (target[0] == 'v' || target[0] == 'i')) {
                const std::string inner = target.substr(2, target.size() - 3);
                a.variable = std::string(1, target[0]) + "(" +
                             (target[0] == 'v' ? scopedNode(scope, inner) : scope.prefix + inner) + ")";
            } else {
                a.variable = scope.localVars.count(target) ? scope.prefix + target : target;
            }
            a.text = value.is_string() ? value.get<std::string>() : value.dump();
            try {
                a.value = scopedExpression(scope, a.text);
            } catch (const std::runtime_error& err) {
                throw std::runtime_error(what + ": " + err.what());
            }
            w.assignments.push_back(std::move(a));
        }
        out.push_back(std::move(w));
    }
    return out;
}

// "tables": {"ntc": {"x": [...], "y": [...], "interp": "cubic", "extrap": "hold"},
//            "meas": {"file": "curve.csv", "xcol": 0, "ycol": 1}}
std::shared_ptr<const expr::Table> parseTable(const std::string& name, const json& j,
                                              const ParamMap& parameters,
                                              const std::string& baseDir) {
    const std::string where = "table \"" + name + "\"";
    if (!isIdentifier(name))
        throw std::runtime_error(where + ": names consist of letters, digits and _");
    Params p(j, where, &parameters);
    std::vector<double> x, y;
    const int sources = p.has("x") + p.has("points") + p.has("file");
    if (sources != 1)
        throw std::runtime_error(where + ": give the data as \"x\" and \"y\", as \"points\" "
                                         "or as \"file\"");
    if (p.has("x")) {
        const json& xs = p.raw("x");
        const json& ys = p.raw("y");
        if (!xs.is_array() || !ys.is_array())
            throw std::runtime_error(where + ": \"x\" and \"y\" must be arrays");
        for (const json& v : xs) x.push_back(jsonNumber(v, where + ", \"x\"", &parameters));
        for (const json& v : ys) y.push_back(jsonNumber(v, where + ", \"y\"", &parameters));
    } else if (p.has("points")) {
        const json& pts = p.raw("points");
        if (!pts.is_array()) throw std::runtime_error(where + ": \"points\" must be an array");
        for (const json& pt : pts) {
            if (!pt.is_array() || pt.size() != 2)
                throw std::runtime_error(where + ": each point must be [x, y]");
            x.push_back(jsonNumber(pt[0], where, &parameters));
            y.push_back(jsonNumber(pt[1], where, &parameters));
        }
    } else {
        // Text file with numbers in columns (comma, semicolon or blank separated);
        // lines that do not start with a number (headers, comments) are skipped.
        const std::string file = p.str("file");
        const int xcol = static_cast<int>(p.num("xcol", 0)), ycol = static_cast<int>(p.num("ycol", 1));
        std::filesystem::path path(file);
        if (path.is_relative() && !baseDir.empty()) path = std::filesystem::path(baseDir) / path;
        std::ifstream in(path);
        if (!in) throw std::runtime_error(where + ": cannot open '" + path.string() + "'");
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
            x.push_back(row[xcol]);
            y.push_back(row[ycol]);
        }
    }
    const std::string interp = lower(p.str("interp", "linear"));
    const std::string extrap = lower(p.str("extrap", "hold"));
    if (interp != "linear" && interp != "cubic")
        throw std::runtime_error(where + ": \"interp\" must be \"linear\" or \"cubic\"");
    if (extrap != "hold" && extrap != "linear")
        throw std::runtime_error(where + ": \"extrap\" must be \"hold\" or \"linear\"");
    p.warnUnused();
    try {
        return std::make_shared<expr::Table>(
            std::move(x), std::move(y),
            interp == "cubic" ? expr::Table::Interp::Cubic : expr::Table::Interp::Linear,
            extrap == "linear" ? expr::Table::Extrap::Linear : expr::Table::Extrap::Hold);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(where + ": " + e.what());
    }
}

// Register the names of the variables of a scope. This has to happen before
// any expression of that scope is translated.
void declareVariables(Scope& scope, const json& def) {
    if (!def.contains("variables") || !def["variables"].is_object()) return;
    for (const auto& [name, v] : def["variables"].items()) scope.localVars.insert(name);
}

// "variables", "equations" and "events" of the top level or of a subcircuit.
void addEquations(Builder& b, Scope& scope, const json& def, const std::string& what) {
    if (def.contains("events")) {  // events alone may act on the circuit
        std::vector<WhenSpec> whens = parseEvents(def["events"], scope, what + ", events");
        b.whens.insert(b.whens.end(), std::make_move_iterator(whens.begin()),
                       std::make_move_iterator(whens.end()));
    }
    if (!def.contains("variables") && !def.contains("equations")) return;
    if (!def.contains("variables") || !def.contains("equations"))
        throw std::runtime_error(what + ": \"variables\" and \"equations\" belong together");
    if (!def["equations"].is_array())
        throw std::runtime_error(what + ": \"equations\" must be an array of strings");

    const std::vector<VariableSpec> vars = parseVariables(def["variables"], scope, what);
    std::vector<std::string> texts;
    for (const json& e : def["equations"]) {
        if (!e.is_string()) throw std::runtime_error(what + ": equations must be strings");
        texts.push_back(e.get<std::string>());
    }
    size_t discrete = 0;
    for (const VariableSpec& v : vars) discrete += v.discrete ? 1 : 0;
    if (texts.size() + discrete != vars.size())
        throw std::runtime_error(what + ": " + std::to_string(texts.size()) + " equation(s) for " +
                                 std::to_string(vars.size() - discrete) +
                                 " variable(s) that need one; the numbers must match");
    // A discrete variable only changes at events: its equation is implied.
    for (const VariableSpec& v : vars)
        if (v.discrete) texts.push_back("der(" + v.name.substr(scope.prefix.size()) + ") = 0");

    for (const std::string& text : texts) {
        try {
            b.equations.push_back({text, scopedAst(scope, expr::parseEquation(text))});
        } catch (const std::runtime_error& err) {
            throw std::runtime_error(what + ": equation " + err.what());
        }
    }
    b.variables.insert(b.variables.end(), vars.begin(), vars.end());
}

// Component of type "X": insert a copy of a subcircuit.
void instantiate(Builder& b, const Scope& scope, const std::string& name, Params& p,
                 const std::vector<std::string>& nodeNames) {
    const std::string subName = p.str("subcircuit");
    auto it = b.subcircuits.find(subName);
    if (it == b.subcircuits.end())
        throw std::runtime_error(p.where() + ": subcircuit \"" + subName + "\" is not defined");
    const json& def = it->second;
    const std::string what = "subcircuit \"" + subName + "\"";
    if (scope.depth >= 20)
        throw std::runtime_error(p.where() + ": subcircuits are nested more than 20 levels deep "
                                             "(does \"" + subName + "\" contain itself?)");

    static const std::set<std::string> known = {"ports", "parameters", "components",
                                                "variables", "equations", "events"};
    for (const auto& [k, v] : def.items())
        if (!known.count(k)) warn() << what << ": unknown key \"" << k << "\" ignored\n";

    if (!def.contains("ports") || !def["ports"].is_array())
        throw std::runtime_error(what + ": needs \"ports\": [\"a\", \"b\", ...]");
    if (def["ports"].size() != nodeNames.size())
        throw std::runtime_error(p.where() + ": " + what + " has " +
                                 std::to_string(def["ports"].size()) + " ports, but " +
                                 std::to_string(nodeNames.size()) + " nodes are connected");

    Scope child;
    child.prefix = name + ".";
    child.depth = scope.depth + 1;
    for (size_t i = 0; i < nodeNames.size(); ++i) {
        if (!def["ports"][i].is_string())
            throw std::runtime_error(what + ": port names must be strings");
        const std::string port = def["ports"][i].get<std::string>();
        if (Circuit::isGroundName(port))
            throw std::runtime_error(what + ": ground cannot be a port (it is global)");
        if (!child.ports.emplace(port, nodeNames[i]).second)
            throw std::runtime_error(what + ": port \"" + port + "\" appears twice");
    }

    // Parameters: the values given at the instance are evaluated outside, the
    // defaults of the subcircuit inside (they may use global and other local ones).
    const json defaults = def.value("parameters", json::object());
    ParamMap fixed = b.ckt.parameters;
    if (p.has("parameters")) {
        const json& over = p.raw("parameters");
        if (!over.is_object())
            throw std::runtime_error(p.where() + ": \"parameters\" must be an object");
        for (const auto& [k, v] : over.items()) {
            if (!defaults.is_object() || !defaults.contains(k))
                throw std::runtime_error(p.where() + ": " + what + " has no parameter \"" + k + "\"");
            fixed[k] = jsonNumber(v, p.where() + ", parameter \"" + k + "\"", &scope.params);
        }
    }
    json remaining = json::object();
    if (defaults.is_object())
        for (const auto& [k, v] : defaults.items()) {
            child.localParams.insert(k);
            if (!p.has("parameters") || !p.raw("parameters").contains(k)) remaining[k] = v;
        }
    child.params = resolveParameters(remaining, std::move(fixed), what + ", parameter");
    declareVariables(child, def);

    if (def.contains("components")) {
        if (!def["components"].is_array())
            throw std::runtime_error(what + ": \"components\" must be an array");
        int index = 0;
        for (const json& spec : def["components"]) addComponent(b, child, spec, index++);
    }
    addEquations(b, child, def, what);
    b.instancePrefixes.push_back(child.prefix);
}

// Number in SPICE notation: suffixes are case insensitive, "m" is milli and
// "meg" is mega; anything after the suffix is a unit.
double parseSpiceValue(const std::string& token, const std::string& where) {
    char* end = nullptr;
    const double v = std::strtod(token.c_str(), &end);
    if (end == token.c_str())
        throw std::runtime_error(where + ": cannot read the value '" + token + "'");
    const std::string suffix = lower(end);
    if (suffix.rfind("meg", 0) == 0) return v * 1e6;
    if (suffix.rfind("mil", 0) == 0) return v * 25.4e-6;
    if (suffix.empty()) return v;
    switch (suffix[0]) {
        case 't': return v * 1e12;
        case 'g': return v * 1e9;
        case 'k': return v * 1e3;
        case 'm': return v * 1e-3;
        case 'u': return v * 1e-6;
        case 'n': return v * 1e-9;
        case 'p': return v * 1e-12;
        case 'f': return v * 1e-15;
        default: return v;
    }
}

// A model in SPICE syntax, ".model NAME TYPE (KEY=VALUE ...)", converted to
// the JSON form used here. Parameters this simulator does not model are left
// out and reported once.
json parseSpiceModel(const std::string& name, const std::string& text) {
    const std::string where = "model \"" + name + "\"";
    // Separators to blanks; a '+' at the start of a line continues the previous one.
    std::string flat;
    bool lineStart = true;
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') {
            flat += ' ';
            lineStart = true;
            continue;
        }
        if (lineStart && ch == '+') ch = ' ';
        if (!std::isspace(static_cast<unsigned char>(ch))) lineStart = false;
        if (ch == '(' || ch == ')' || ch == ',') flat += ' ';
        else if (ch == '=') flat += " = ";
        else flat += ch;
    }
    std::vector<std::string> tok;
    std::istringstream in(flat);
    for (std::string t; in >> t;) tok.push_back(t);
    if (tok.size() < 3 || lower(tok[0]) != ".model")
        throw std::runtime_error(where + ": expected \".model NAME TYPE (KEY=VALUE ...)\"");

    static const std::map<std::string, std::pair<std::string, std::set<std::string>>> types = {
        {"d", {"diode", {"is", "n", "rs", "cj0", "vj", "m", "fc", "tt", "bv", "ibv", "eg", "xti",
                         "kf", "af"}}},
        {"npn", {"npn", {}}}, {"pnp", {"pnp", {}}},
        {"nmos", {"nmos", {}}}, {"pmos", {"pmos", {}}},
        {"njf", {"njf", {"vto", "beta", "lambda", "is", "cgs", "cgd", "pb", "fc", "kf", "af"}}},
        {"pjf", {"pjf", {"vto", "beta", "lambda", "is", "cgs", "cgd", "pb", "fc", "kf", "af"}}},
    };
    static const std::set<std::string> bjtKeys = {
        "is", "bf", "br", "nf", "nr", "vaf", "var", "ikf", "ikr", "ise", "ne", "isc", "nc",
        "rb", "rc", "re", "cje", "vje", "mje", "cjc", "vjc", "mjc", "tf", "tr", "fc", "eg",
        "xti", "xtb", "kf", "af"};
    static const std::set<std::string> mosKeys = {
        "vto", "kp", "gamma", "phi", "lambda", "cgso", "cgdo", "cgbo", "cbd", "cbs", "pb",
        "mj", "fc", "is", "tox", "tcv", "w", "l", "kf", "af", "theta", "ucrit"};
    const std::string type = lower(tok[2]);
    auto it = types.find(type);
    if (it == types.end())
        throw std::runtime_error(where + ": SPICE model type '" + tok[2] +
                                 "' is not supported (D, NPN, PNP, NMOS, PMOS, NJF, PJF)");
    const std::set<std::string>& keys =
        (type == "d" || type == "njf" || type == "pjf") ? it->second.second
        : (type == "npn" || type == "pnp") ? bjtKeys : mosKeys;

    json out = json::object();
    out["type"] = it->second.first;
    std::string ignored;
    for (size_t k = 3; k < tok.size(); k += 3) {
        if (k + 2 >= tok.size() || tok[k + 1] != "=")
            throw std::runtime_error(where + ": expected KEY=VALUE near '" + tok[k] + "'");
        std::string key = lower(tok[k]);
        const double value = parseSpiceValue(tok[k + 2], where + ", " + key);
        if (key == "cjo") key = "cj0";
        if (key == "level") {
            if (value != 1.0)
                warn() << where << ": level " << value
                       << " requested, the level 1 equations are used\n";
            continue;
        }
        if (!keys.count(key)) {
            ignored += (ignored.empty() ? "" : ", ") + key;
            continue;
        }
        // In SPICE a value of 0 means "infinite" for these parameters.
        if (value == 0.0 && (key == "vaf" || key == "var" || key == "ikf" || key == "ikr" || key == "bv"))
            continue;
        out[key] = value;
    }
    if (!ignored.empty())
        warn() << where << ": SPICE parameters without effect here: " << ignored << '\n';
    return out;
}

}  // namespace

Netlist parseNetlist(const json& root, bool warnings, const std::string& baseDir) {
    if (!root.is_object()) throw std::runtime_error("netlist: top level must be a JSON object");
    struct WarningScope {
        bool saved;
        explicit WarningScope(bool on) : saved(g_warnings) { g_warnings = on; }
        ~WarningScope() { g_warnings = saved; }
    } scope(warnings);

    static const std::set<std::string> known = {"title",      "options",   "models",    "components",
                                                "analyses",   "outputs",   "nodeset",   "ic",
                                                "parameters", "variables", "equations", "events",
                                                "subcircuits", "tables", "functions"};
    for (const auto& [k, v] : root.items())
        if (!known.count(k))
            warn() << "netlist: unknown top-level key \"" << k << "\" ignored\n";

    Netlist nl;
    nl.source = root;
    nl.baseDir = baseDir;
    nl.circuit = std::make_unique<Circuit>();
    nl.title = root.value("title", std::string());
    Circuit& ckt = *nl.circuit;
    if (root.contains("parameters"))
        ckt.parameters = resolveParameters(root["parameters"], {}, "parameter");
    if (root.contains("options")) parseOptions(root["options"], nl);
    if (root.contains("tables")) {
        if (!root["tables"].is_object()) throw std::runtime_error("tables: expected an object");
        for (const auto& [name, t] : root["tables"].items())
            ckt.tables[name] = parseTable(name, t, ckt.parameters, baseDir);
    }

    // "functions": {"sat(x, lim)": "max(-lim, min(lim, x))"}
    if (root.contains("functions")) {
        if (!root["functions"].is_object()) throw std::runtime_error("functions: expected an object");
        for (const auto& [signature, body] : root["functions"].items()) {
            const std::string where = "function \"" + signature + "\"";
            const size_t open = signature.find('('), close = signature.rfind(')');
            if (open == std::string::npos || close != signature.size() - 1 || !body.is_string())
                throw std::runtime_error(where + ": expected \"name(a, b)\": \"expression\"");
            auto trim = [](std::string t) {
                t.erase(0, t.find_first_not_of(" \t"));
                t.erase(t.find_last_not_of(" \t") + 1);
                return t;
            };
            const std::string name = trim(signature.substr(0, open));
            if (!isIdentifier(name)) throw std::runtime_error(where + ": invalid name");
            if (expr::isBuiltinFunction(name) || name == "v" || name == "i")
                throw std::runtime_error(where + ": '" + name + "' is a built-in function");
            expr::FunctionDef def;
            std::istringstream list(signature.substr(open + 1, close - open - 1));
            for (std::string arg; std::getline(list, arg, ',');) {
                arg = trim(arg);
                if (!isIdentifier(arg)) throw std::runtime_error(where + ": invalid parameter name");
                def.params.push_back(arg);
            }
            try {
                def.body = expr::parseExpression(body.get<std::string>());
            } catch (const std::runtime_error& e) {
                throw std::runtime_error(where + ": " + e.what());
            }
            ckt.functions[name] = def;
        }
    }

    Builder b{ckt, {}, {}, {}, {}, {}, {}};
    for (const char* section : {"models", "subcircuits"}) {
        if (!root.contains(section)) continue;
        if (!root[section].is_object())
            throw std::runtime_error(std::string(section) + ": expected an object");
        const bool isModels = std::string(section) == "models";
        for (const auto& [name, m] : root[section].items()) {
            if (isModels && m.is_string()) {  // a SPICE ".model" line
                b.models[name] = parseSpiceModel(name, m.get<std::string>());
                continue;
            }
            if (!m.is_object())
                throw std::runtime_error(std::string(section) + ", \"" + name +
                                         "\": expected an object");
            (isModels ? b.models : b.subcircuits)[name] = m;
        }
    }

    Scope top;
    top.params = ckt.parameters;
    declareVariables(top, root);
    const bool hasEquations =
        root.contains("equations") || root.contains("variables") || root.contains("events");
    if (root.contains("components")) {
        if (!root["components"].is_array())
            throw std::runtime_error("netlist: \"components\" must be an array");
        int index = 0;
        for (const json& spec : root["components"]) addComponent(b, top, spec, index++);
    } else if (!hasEquations) {
        throw std::runtime_error("netlist: needs \"components\" and/or \"equations\"");
    }
    addEquations(b, top, root, "netlist");
    if (!b.variables.empty() || !b.whens.empty())
        ckt.add(std::make_unique<EquationBlock>("#equations", std::move(b.variables),
                                                std::move(b.equations), std::move(b.whens)));

    if (root.contains("nodeset"))
        ckt.nodeset = parseNodeValues(root["nodeset"], "nodeset", &ckt.parameters);
    if (root.contains("ic")) ckt.ic = parseNodeValues(root["ic"], "ic", &ckt.parameters);
    nl.circuit->finalize();
    for (int i = 0; i < ckt.size(); ++i) {
        std::string label = ckt.unknownName(i);
        if (label.size() > 3 && label[1] == '(' && label.back() == ')')
            label = label.substr(2, label.size() - 3);
        for (const std::string& prefix : b.instancePrefixes)
            if (label.rfind(prefix, 0) == 0) ckt.setInternal(i, true);
    }
    for (const auto& [name, v] : nl.circuit->nodeset) nl.circuit->findNode(name);
    for (const auto& [name, v] : nl.circuit->ic) nl.circuit->findNode(name);

    if (root.contains("analyses")) {
        if (!root["analyses"].is_array()) throw std::runtime_error("analyses: expected an array");
        for (const json& a : root["analyses"]) nl.analyses.push_back(a);
    }
    if (root.contains("outputs")) {
        if (!root["outputs"].is_array()) throw std::runtime_error("outputs: expected an array");
        for (const json& o : root["outputs"]) {
            if (!o.is_string()) throw std::runtime_error("outputs: entries must be strings");
            nl.outputs.push_back(o.get<std::string>());
        }
    }
    return nl;
}

Netlist parseNetlistText(const std::string& text) {
    try {
        return parseNetlist(json::parse(text, nullptr, true, /*ignore_comments=*/true));
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("JSON syntax error: ") + e.what());
    }
}

Netlist loadNetlist(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open '" + path + "'");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        return parseNetlist(json::parse(text, nullptr, true, /*ignore_comments=*/true), true,
                            std::filesystem::path(path).parent_path().string());
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("JSON syntax error: ") + e.what());
    }
}

}  // namespace esim
