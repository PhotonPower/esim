#include "io/Spice.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstring>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "core/ParseNumber.hpp"

namespace esim {

using json = nlohmann::json;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// shortest text that reads back as v; independent of the locale (snprintf
// would write "0,5" in a process with a German locale, as the editor)
std::string numberText(double v) {
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

// A number in SPICE notation ("4.7k", "1meg", "10uF"): suffixes ignore case,
// "m" is milli, letters after the suffix are a unit.
bool spiceNumber(const std::string& t, double& v) {
    if (t.empty()) return false;
    char* end = nullptr;
    v = parseDouble(t.c_str(), &end);
    if (end == t.c_str()) return false;
    const std::string suffix = lower(end);
    for (char c : suffix)
        if (!std::isalpha(static_cast<unsigned char>(c))) return false;
    if (suffix.rfind("meg", 0) == 0) v *= 1e6;
    else if (suffix.rfind("mil", 0) == 0) v *= 25.4e-6;
    else if (!suffix.empty()) {
        switch (suffix[0]) {
            case 't': v *= 1e12; break;
            case 'g': v *= 1e9; break;
            case 'k': v *= 1e3; break;
            case 'm': v *= 1e-3; break;
            case 'u': v *= 1e-6; break;
            case 'n': v *= 1e-9; break;
            case 'p': v *= 1e-12; break;
            case 'f': v *= 1e-15; break;
            default: break;  // a unit only ("10V")
        }
    }
    return true;
}

// --- expressions ------------------------------------------------------------------

class ExpressionParser {
public:
    explicit ExpressionParser(const std::string& s) : s_(s) {}

    std::string parse() {
        std::string r = ternary();
        skip();
        if (pos_ < s_.size()) fail("unexpected \"" + s_.substr(pos_) + "\"");
        return r;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("expression \"" + s_ + "\": " + what);
    }
    void skip() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    bool peek(const char* op) {
        skip();
        return s_.compare(pos_, std::strlen(op), op) == 0;
    }
    bool eat(const char* op) {
        if (!peek(op)) return false;
        pos_ += std::strlen(op);
        return true;
    }

    std::string ternary() {
        const std::string c = orExpr();
        if (!eat("?")) return c;
        const std::string a = ternary();
        if (!eat(":")) fail("expected ':'");
        const std::string b = ternary();
        return "if(" + c + ", " + a + ", " + b + ")";
    }
    std::string orExpr() {
        std::string a = andExpr();
        while (eat("||") || eat("|")) a = "(" + a + " || " + andExpr() + ")";
        return a;
    }
    std::string andExpr() {
        std::string a = comparison();
        while (eat("&&") || eat("&")) a = "(" + a + " && " + comparison() + ")";
        return a;
    }
    std::string comparison() {
        std::string a = sum();
        for (const char* op : {"==", "!=", "<=", ">=", "<", ">"}) {
            if (eat(op)) return "(" + a + " " + op + " " + sum() + ")";
        }
        return a;
    }
    std::string sum() {
        std::string a = product();
        while (true) {
            if (eat("+")) a = "(" + a + " + " + product() + ")";
            else if (eat("-")) a = "(" + a + " - " + product() + ")";
            else return a;
        }
    }
    std::string product() {
        std::string a = unary();
        while (true) {
            if (!peek("**") && eat("*")) a = "(" + a + " * " + unary() + ")";
            else if (eat("/")) a = "(" + a + " / " + unary() + ")";
            else return a;
        }
    }
    std::string unary() {
        if (eat("-")) return "(-" + unary() + ")";
        if (eat("+")) return unary();
        if (!peek("!=") && eat("!")) return "(!" + unary() + ")";
        return power();
    }
    std::string power() {
        const std::string a = primary();
        if (eat("**") || eat("^")) return "(" + a + " ^ " + unary() + ")";
        return a;
    }
    std::vector<std::string> arguments() {
        std::vector<std::string> args;
        if (eat(")")) return args;
        while (true) {
            args.push_back(ternary());
            if (eat(")")) return args;
            if (!eat(",")) fail("expected ',' or ')'");
        }
    }
    std::string primary() {
        skip();
        if (pos_ >= s_.size()) fail("unexpected end");
        const char c = s_[pos_];
        if (c == '(' || c == '{') {
            ++pos_;
            const std::string inner = ternary();
            if (!eat(c == '(' ? ")" : "}")) fail(std::string("expected '") + (c == '(' ? ')' : '}') + "'");
            return "(" + inner + ")";
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
            size_t end = pos_;
            while (end < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[end])) || s_[end] == '.')) ++end;
            if (end < s_.size() && (s_[end] == 'e' || s_[end] == 'E')) {
                size_t e = end + 1;
                if (e < s_.size() && (s_[e] == '+' || s_[e] == '-')) ++e;
                if (e < s_.size() && std::isdigit(static_cast<unsigned char>(s_[e]))) {
                    end = e;
                    while (end < s_.size() && std::isdigit(static_cast<unsigned char>(s_[end]))) ++end;
                }
            }
            while (end < s_.size() && std::isalpha(static_cast<unsigned char>(s_[end]))) ++end;  // suffix, unit
            double v = 0.0;
            if (!spiceNumber(s_.substr(pos_, end - pos_), v)) fail("cannot read a number");
            pos_ = end;
            return numberText(v);
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t end = pos_;
            while (end < s_.size() &&
                   (std::isalnum(static_cast<unsigned char>(s_[end])) || s_[end] == '_' || s_[end] == '.' || s_[end] == '$'))
                ++end;
            const std::string name = lower(s_.substr(pos_, end - pos_));
            pos_ = end;
            if (!eat("(")) {
                if (name == "pi") return numberText(3.14159265358979323846);
                return name;
            }
            if (name == "v" || name == "i") {
                // node or source names, which may start with a digit
                const size_t close = s_.find(')', pos_);
                if (close == std::string::npos) fail("expected ')'");
                std::string args;
                std::stringstream in(s_.substr(pos_, close - pos_));
                for (std::string part; std::getline(in, part, ',');) args += (args.empty() ? "" : ",") + lower(trim(part));
                pos_ = close + 1;
                return name + "(" + args + ")";
            }
            const std::vector<std::string> a = arguments();
            const auto need = [&](size_t n) {
                if (a.size() != n) fail(name + "() takes " + std::to_string(n) + " arguments");
            };
            if (name == "limit") {
                need(3);
                return "min(max(" + a[0] + ", min(" + a[1] + ", " + a[2] + ")), max(" + a[1] + ", " + a[2] + "))";
            }
            if (name == "pwr") {
                need(2);
                return "pow(abs(" + a[0] + "), " + a[1] + ")";
            }
            if (name == "pwrs") {
                need(2);
                return "(sign(" + a[0] + ") * pow(abs(" + a[0] + "), " + a[1] + "))";
            }
            if (name == "uramp") {
                need(1);
                return "max(" + a[0] + ", 0)";
            }
            if (name == "u" || name == "stp") {
                need(1);
                return "if(" + a[0] + " > 0, 1, 0)";
            }
            std::string out = (name == "sgn" ? std::string("sign") : name) + "(";
            for (size_t k = 0; k < a.size(); ++k) out += (k ? ", " : "") + a[k];
            return out + ")";
        }
        fail(std::string("unexpected '") + c + "'");
    }

    const std::string& s_;
    size_t pos_ = 0;
};

// A value of an element: a number, or an expression ({...}, '...', a parameter name).
json value(const std::string& token) {
    std::string t = trim(token);
    if (t.size() >= 2 && ((t.front() == '{' && t.back() == '}') || (t.front() == '\'' && t.back() == '\'')))
        return spiceExpression(t.substr(1, t.size() - 2));
    double v = 0.0;
    if (spiceNumber(t, v)) return v;
    return spiceExpression(t);
}

// --- lines and tokens -------------------------------------------------------------

struct Line {
    std::string text;
    int number;  // of its first line in the text, from 1
};

std::vector<Line> logicalLines(const std::string& text) {
    std::vector<Line> out;
    std::stringstream in(text);
    int number = 0;
    for (std::string raw; std::getline(in, raw);) {
        ++number;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string t = trim(raw);
        if (t.empty() || t[0] == '*') continue;
        // comments after ';' and " $ "
        const size_t semi = t.find(';');
        if (semi != std::string::npos) t = trim(t.substr(0, semi));
        const size_t dollar = t.find(" $ ");
        if (dollar != std::string::npos) t = trim(t.substr(0, dollar));
        if (t.empty()) continue;
        if (t[0] == '+') {
            if (!out.empty()) out.back().text += " " + trim(t.substr(1));
            continue;
        }
        out.push_back({t, number});
    }
    return out;
}

// Splits a line: blanks, parentheses and commas separate; "a = b" becomes
// one token "a=b"; {...} and '...' stay together.
std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&]() {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '{' || c == '\'') {
            const char close = c == '{' ? '}' : '\'';
            int depth = 0;
            size_t j = i;
            for (; j < line.size(); ++j) {
                if (c == '{' && line[j] == '{') ++depth;
                else if (line[j] == close && (c == '\'' ? j > i : --depth == 0)) break;
            }
            cur += line.substr(i, j - i + 1);
            i = j;
        } else if (c == '=') {
            // join with the neighbours: "a = b" -> "a=b"
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            if (cur.empty() && !out.empty()) {
                cur = out.back();
                out.pop_back();
            }
            cur += '=';
            while (i + 1 < line.size() && line[i + 1] == ' ') ++i;
        } else if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == ',') {
            if (!cur.empty() && cur.back() == '=') continue;  // "a=" waits for its value
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

void splitParams(const std::vector<std::string>& parts, size_t from, std::vector<std::string>& positional,
                 json& named) {
    for (size_t k = from; k < parts.size(); ++k) {
        const size_t eq = parts[k].find('=');
        if (eq == std::string::npos || parts[k][0] == '{' || parts[k][0] == '\'') {
            positional.push_back(parts[k]);
        } else {
            named[lower(parts[k].substr(0, eq))] = value(parts[k].substr(eq + 1));
        }
    }
}

bool isNumber(const std::string& t) {
    double v;
    return spiceNumber(t, v);
}

// --- definitions ------------------------------------------------------------------

struct Def {
    std::string name;  // as written
    int line = 0;
    std::vector<std::string> ports;
    json params = json::object();
    std::vector<Line> elements;
    std::vector<Def*> children;
    std::map<std::string, Line> models;  // lower-case name -> its .model line
    Def* parent = nullptr;
    std::string assigned;  // name in the result
};

const std::vector<std::string> kModelTypes = {"d", "npn", "pnp", "nmos", "pmos", "njf", "pjf"};

class Translator {
public:
    Translator(const std::string& key, const std::string& text) : key_(key) {
        root_.name = "";
        build(text);
    }

    SpiceDefinitions run() {
        Def* main = nullptr;
        for (Def* d : root_.children)
            if (lower(d->name) == lower(key_)) main = d;
        if (!main && root_.children.size() == 1) main = root_.children.front();
        if (!main) {
            std::string names;
            for (Def* d : root_.children) names += (names.empty() ? "" : ", ") + d->name;
            throw std::runtime_error(where() + ": no .subckt " + key_ +
                                     (names.empty() ? " in the text" : " (the text defines " + names + ")"));
        }
        main_ = main;
        assign(&root_);
        for (const Line& l : root_.elements)
            throw std::runtime_error(where(l) + ": element outside of a .subckt: " + l.text);
        // parameters of the text's top level are defaults of the subcircuit
        for (auto it = root_.params.begin(); it != root_.params.end(); ++it)
            if (!main->params.contains(it.key())) main->params[it.key()] = it.value();
        translate(&root_);
        return std::move(out_);
    }

private:
    std::string where() const { return "subcircuit \"" + key_ + "\""; }
    std::string where(const Line& l) const { return where() + ", line " + std::to_string(l.number); }

    void build(const std::string& text) {
        std::vector<Def*> stack{&root_};
        for (const Line& l : logicalLines(text)) {
            const std::string low = lower(l.text);
            if (low.rfind(".subckt", 0) == 0) {
                const std::vector<std::string> t = tokens(l.text);
                if (t.size() < 2) throw std::runtime_error(where(l) + ": .subckt without a name");
                defs_.push_back(std::make_unique<Def>());
                Def* d = defs_.back().get();
                d->name = t[1];
                d->line = l.number;
                d->parent = stack.back();
                bool params = false;
                for (size_t k = 2; k < t.size(); ++k) {
                    const std::string lt = lower(t[k]);
                    if (lt == "params:") {
                        params = true;
                        continue;
                    }
                    if (lt == "optional:") throw std::runtime_error(where(l) + ": OPTIONAL: is not supported");
                    const size_t eq = t[k].find('=');
                    if (eq != std::string::npos) {
                        d->params[lower(t[k].substr(0, eq))] = value(t[k].substr(eq + 1));
                    } else if (params) {
                        throw std::runtime_error(where(l) + ": expected name=value after PARAMS:");
                    } else {
                        d->ports.push_back(lower(t[k]));
                    }
                }
                stack.back()->children.push_back(d);
                stack.push_back(d);
            } else if (low.rfind(".ends", 0) == 0) {
                if (stack.size() < 2) throw std::runtime_error(where(l) + ": .ends without .subckt");
                stack.pop_back();
            } else if (low.rfind(".model", 0) == 0) {
                const std::vector<std::string> t = tokens(l.text);
                if (t.size() < 3) throw std::runtime_error(where(l) + ": incomplete .model");
                stack.back()->models[lower(t[1])] = l;
            } else if (low.rfind(".param", 0) == 0) {
                const std::vector<std::string> t = tokens(l.text);
                std::vector<std::string> positional;
                json named = json::object();
                splitParams(t, 1, positional, named);
                for (auto it = named.begin(); it != named.end(); ++it) stack.back()->params[it.key()] = it.value();
            } else if (low.rfind(".include", 0) == 0 || low.rfind(".inc ", 0) == 0 || low.rfind(".lib", 0) == 0) {
                throw std::runtime_error(where(l) + ": " + l.text.substr(0, l.text.find(' ')) +
                                         " is not supported here; paste the definitions instead");
            } else if (low.rfind(".end", 0) == 0) {
                // the end of a whole file
            } else if (low[0] == '.') {
                out_.warnings.push_back(where(l) + ": " + l.text.substr(0, l.text.find(' ')) + " ignored");
            } else {
                stack.back()->elements.push_back(l);
            }
        }
        if (stack.size() > 1)
            throw std::runtime_error(where() + ": .subckt " + stack.back()->name + " (line " +
                                     std::to_string(stack.back()->line) + ") has no .ends");
    }

    std::string unique(const std::string& name) {
        std::string n = name;
        for (int k = 2; taken_.count(lower(n)); ++k) n = name + "#" + std::to_string(k);
        taken_.insert(lower(n));
        return n;
    }

    // names in the result: the main subcircuit is `key`, all else "key/name"
    void assign(Def* d) {
        if (d == main_) d->assigned = key_;
        else if (d != &root_) d->assigned = unique(key_ + "/" + lower(d->name));
        for (auto& [name, line] : d->models) modelNames_[d][name] = unique(key_ + "/" + name);
        for (Def* c : d->children) assign(c);
    }

    // a model or subcircuit as seen from definition d: its own, an enclosing one's, or the file's
    std::string modelName(const Def* d, const std::string& name, const Line& l) {
        const std::string n = lower(name);
        for (const Def* s = d; s; s = s->parent) {
            auto it = s->models.find(n);
            if (it == s->models.end()) continue;
            const std::vector<std::string> t = tokens(it->second.text);
            const std::string type = lower(t[2]);
            if (std::find(kModelTypes.begin(), kModelTypes.end(), type) == kModelTypes.end())
                throw std::runtime_error(where(l) + ": model " + name + " has type " + t[2] +
                                         ", which is not supported (D, NPN, PNP, NMOS, PMOS, NJF, PJF)");
            return modelNames_.at(s).at(n);
        }
        return name;  // a model of the file
    }
    const Line* modelLine(const Def* d, const std::string& name) const {
        for (const Def* s = d; s; s = s->parent) {
            auto it = s->models.find(lower(name));
            if (it != s->models.end()) return &it->second;
        }
        return nullptr;
    }
    std::string subcircuitName(const Def* d, const std::string& name) const {
        for (const Def* s = d; s; s = s->parent)
            for (const Def* c : s->children)
                if (lower(c->name) == lower(name)) return c->assigned;
        return name;
    }

    void translate(Def* d) {
        if (d != &root_) {
            json def = json::object();
            def["ports"] = d->ports;
            if (!d->params.empty()) def["parameters"] = d->params;
            json comps = json::array();
            for (const Line& l : d->elements) {
                try {
                    comps.push_back(element(d, l));
                } catch (const std::runtime_error& e) {
                    const std::string msg = e.what();
                    throw std::runtime_error(msg.rfind(where(), 0) == 0 ? msg : where(l) + ": " + msg);
                }
            }
            def["components"] = comps;
            out_.subcircuits[d->assigned] = def;
        }
        for (auto& [name, line] : d->models) {
            const std::vector<std::string> t = tokens(line.text);
            const std::string type = lower(t[2]);
            if (std::find(kModelTypes.begin(), kModelTypes.end(), type) != kModelTypes.end())
                out_.models[modelNames_[d][name]] = line.text;
        }
        for (Def* c : d->children) translate(c);
    }

    json source(const std::vector<std::string>& t, char kind) {
        json c = {{"type", std::string(1, kind)}, {"name", lower(t[0])}, {"nodes", {lower(t[1]), lower(t[2])}}};
        std::vector<std::string> rest;
        for (size_t k = 3; k < t.size(); ++k) rest.push_back(t[k]);
        const auto isWord = [](const std::string& s) {
            const std::string l = lower(s);
            return l == "dc" || l == "ac" || l == "sin" || l == "pulse" || l == "pwl" || l == "exp" || l == "sffm";
        };
        const auto numbers = [&](size_t& i) {
            std::vector<json> v;
            while (i < rest.size() && !isWord(rest[i])) v.push_back(value(rest[i++]));
            return v;
        };
        for (size_t i = 0; i < rest.size();) {
            const std::string w = lower(rest[i]);
            if (w == "dc") {
                ++i;
                const auto v = numbers(i);
                if (!v.empty()) c["dc"] = v[0];
            } else if (w == "ac") {
                ++i;
                const auto v = numbers(i);
                c["ac"] = v.empty() ? json(1.0) : v[0];
                if (v.size() > 1) c["acphase"] = v[1];
            } else if (w == "sin") {
                ++i;
                auto v = numbers(i);
                while (v.size() < 6) v.push_back(0.0);
                c["wave"] = {{"type", "sin"}, {"offset", v[0]}, {"amplitude", v[1]}, {"freq", v[2]},
                             {"delay", v[3]}, {"damping", v[4]}, {"phase", v[5]}};
            } else if (w == "pulse") {
                ++i;
                const auto v = numbers(i);
                if (v.size() < 2) throw std::runtime_error("PULSE needs at least v1 and v2");
                json wave = {{"type", "pulse"}, {"v1", v[0]}, {"v2", v[1]}};
                const char* keys[] = {"delay", "rise", "fall", "width", "period"};
                for (size_t k = 2; k < v.size() && k < 7; ++k) {
                    const std::string key = keys[k - 2];
                    if ((key == "rise" || key == "fall") && v[k].is_number() && v[k].get<double>() == 0.0) continue;
                    wave[key] = v[k];
                }
                c["wave"] = wave;
            } else if (w == "pwl") {
                ++i;
                const auto v = numbers(i);
                json points = json::array();
                for (size_t k = 0; k + 1 < v.size(); k += 2) points.push_back({v[k], v[k + 1]});
                c["wave"] = {{"type", "pwl"}, {"points", points}};
            } else if (w == "exp" || w == "sffm") {
                throw std::runtime_error("the source function " + rest[i] + " is not supported");
            } else {
                c["dc"] = value(rest[i++]);
            }
        }
        return c;
    }

    // polynomial of SPICE POLY(n): p0 + p1 x1 + ... + pn xn + x1^2 terms in SPICE order
    static std::string polynomial(const std::vector<std::string>& x, const std::vector<std::string>& coeff) {
        // monomials ordered by degree, then as SPICE lists them (x1x1, x1x2, ..., x2x2, ...)
        std::vector<std::vector<int>> monomials{{}};
        for (int degree = 1; monomials.size() < coeff.size(); ++degree) {
            std::vector<int> idx(size_t(degree), 0);
            while (true) {
                monomials.push_back(idx);
                if (monomials.size() >= coeff.size()) break;
                int k = degree - 1;
                while (k >= 0 && idx[size_t(k)] == int(x.size()) - 1) --k;
                if (k < 0) break;
                ++idx[size_t(k)];
                for (int j = k + 1; j < degree; ++j) idx[size_t(j)] = idx[size_t(k)];
            }
        }
        std::string out;
        for (size_t k = 0; k < coeff.size(); ++k) {
            if (coeff[k] == "0") continue;
            std::string term = coeff[k];
            for (int i : monomials[k]) term += " * " + x[size_t(i)];
            out += (out.empty() ? "" : " + ") + term;
        }
        return out.empty() ? "0" : out;
    }

    static std::string expressionOf(const json& v) {
        return v.is_number() ? numberText(v.get<double>()) : "(" + v.get<std::string>() + ")";
    }

    // E, G, F, H with POLY, VALUE or TABLE
    json controlled(const Def* d, const Line& l, const std::vector<std::string>& t) {
        (void)d;
        const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0][0])));
        const bool voltage = kind == 'E' || kind == 'H';
        if (t.size() < 4) throw std::runtime_error("too few nodes and values");
        json c = {{"name", lower(t[0])}, {"nodes", {lower(t[1]), lower(t[2])}}};
        // the text after the two nodes, as written
        std::string rest = l.text;
        for (int k = 0; k < 3; ++k) {
            size_t p = rest.find_first_not_of(" \t");
            p = rest.find_first_of(" \t(", p);
            rest = p == std::string::npos ? std::string() : rest.substr(p);
        }
        rest = trim(rest);
        const std::string low = lower(rest);
        const auto behavioural = [&](const std::string& expr) {
            c["type"] = "B";
            c[voltage ? "v" : "i"] = expr;
            return c;
        };
        if (low.rfind("value", 0) == 0) {
            std::string e = trim(rest.substr(5));
            if (!e.empty() && e[0] == '=') e = trim(e.substr(1));
            if (e.size() >= 2 && ((e.front() == '{' && e.back() == '}') || (e.front() == '\'' && e.back() == '\'')))
                e = e.substr(1, e.size() - 2);
            return behavioural(spiceExpression(e));
        }
        if (low.rfind("table", 0) == 0) {
            std::string r = trim(rest.substr(5));
            std::string e;
            if (!r.empty() && (r[0] == '{' || r[0] == '\'')) {
                const char close = r[0] == '{' ? '}' : '\'';
                const size_t end = r.find(close, 1);
                if (end == std::string::npos) throw std::runtime_error("TABLE: expression not closed");
                e = r.substr(1, end - 1);
                r = r.substr(end + 1);
            } else {
                const size_t eq = r.find('=');
                e = r.substr(0, eq);
                r = eq == std::string::npos ? std::string() : r.substr(eq);
            }
            for (char& ch : r)
                if (ch == '(' || ch == ')' || ch == ',' || ch == '=') ch = ' ';
            std::stringstream in(r);
            std::vector<double> v;
            for (std::string tok; in >> tok;) {
                double x;
                if (!spiceNumber(tok, x)) throw std::runtime_error("TABLE: cannot read \"" + tok + "\"");
                v.push_back(x);
            }
            if (v.size() < 4 || v.size() % 2) throw std::runtime_error("TABLE needs pairs (x, y), at least two");
            // linear between the points, constant outside
            const std::string x = "(" + spiceExpression(e) + ")";
            const size_t n = v.size() / 2;
            std::string expr = numberText(v[2 * n - 1]);
            for (size_t k = n - 1; k-- > 0;) {
                const double x0 = v[2 * k], y0 = v[2 * k + 1], x1 = v[2 * k + 2], y1 = v[2 * k + 3];
                const std::string seg = x1 > x0 ? numberText(y0) + " + (" + x + " - " + numberText(x0) + ") * " +
                                                      numberText((y1 - y0) / (x1 - x0))
                                                : numberText(y1);
                expr = "if(" + x + " < " + numberText(x1) + ", " + seg + ", " + expr + ")";
            }
            expr = "if(" + x + " < " + numberText(v[0]) + ", " + numberText(v[1]) + ", " + expr + ")";
            return behavioural(expr);
        }
        static const std::regex polyHead(R"(^poly\s*\(\s*(\d+)\s*\)\s*(.*)$)", std::regex::icase);
        std::smatch m;
        if (std::regex_match(rest, m, polyHead)) {
            const int n = std::stoi(m[1].str());
            const std::vector<std::string> p = tokens(m[2].str());
            const bool currents = kind == 'F' || kind == 'H';
            const size_t controls = currents ? size_t(n) : size_t(2 * n);
            if (n < 1 || p.size() < controls + 1) throw std::runtime_error("POLY(" + m[1].str() + ") needs more values");
            std::vector<std::string> x, coeff;
            for (int k = 0; k < n; ++k)
                x.push_back(currents ? "i(" + lower(p[size_t(k)]) + ")"
                                     : "v(" + lower(p[size_t(2 * k)]) + "," + lower(p[size_t(2 * k + 1)]) + ")");
            for (size_t k = controls; k < p.size(); ++k) coeff.push_back(expressionOf(value(p[k])));
            return behavioural(polynomial(x, coeff));
        }
        for (const char* word : {"laplace", "freq", "chebyshev"})
            if (low.rfind(word, 0) == 0)
                throw std::runtime_error(std::string(word) + " sources are not supported");
        return json();  // linear: the caller
    }

    // A PSpice VSWITCH: between Voff and Von the resistance goes smoothly
    // (on a log scale, with zero slope at both ends) from Roff to Ron:
    //   R = exp(Lm + 3 Lr (vc - Vm) / (2 Vd) - 2 Lr (vc - Vm)^3 / Vd^3)
    // with Lm = ln sqrt(Ron Roff), Lr = ln(Ron / Roff), Vm = (Von + Voff) / 2,
    // Vd = Von - Voff. As a behavioural current source.
    json vswitch(const std::string& name, const std::vector<std::string>& n, const json& mn) {
        const auto param = [&](const char* k, double fallback) {
            if (!mn.contains(k)) return fallback;
            if (!mn[k].is_number()) throw std::runtime_error(std::string("VSWITCH ") + k + " must be a number");
            return mn[k].get<double>();
        };
        const double ron = param("ron", 1.0), roff = param("roff", 1e6), von = param("von", 1.0), voff = param("voff", 0.0);
        if (!(ron > 0) || !(roff > 0) || von == voff) throw std::runtime_error("VSWITCH needs RON, ROFF > 0 and VON != VOFF");
        const double lm = std::log(std::sqrt(ron * roff)), lr = std::log(ron / roff), vm = (von + voff) / 2, vd = von - voff;
        const std::string vc = "v(" + n[2] + "," + n[3] + ")";
        const std::string x = "(" + vc + " - " + numberText(vm) + ")";
        const std::string g = "exp(-(" + numberText(lm) + " + " + numberText(3 * lr / (2 * vd)) + " * " + x + " - " +
                              numberText(2 * lr / (vd * vd * vd)) + " * " + x + "^3))";
        const std::string on = vd > 0 ? vc + " >= " + numberText(von) : vc + " <= " + numberText(von);
        const std::string off = vd > 0 ? vc + " <= " + numberText(voff) : vc + " >= " + numberText(voff);
        return {{"type", "B"}, {"name", name}, {"nodes", {n[0], n[1]}},
                {"i", "v(" + n[0] + "," + n[1] + ") * if(" + on + ", " + numberText(1 / ron) + ", if(" + off + ", " +
                          numberText(1 / roff) + ", " + g + "))"}};
    }

    json element(const Def* d, const Line& l) {
        const std::vector<std::string> t = tokens(l.text);
        const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(l.text[0])));
        const std::string name = lower(t[0]);
        std::vector<std::string> pos;
        json named = json::object();
        splitParams(t, 1, pos, named);
        const auto need = [&](size_t n) {
            if (pos.size() < n) throw std::runtime_error("too few nodes or values: " + l.text);
        };
        const auto node = [&](size_t k) { return lower(pos[k]); };
        const auto unused = [&](std::initializer_list<const char*> keep) {
            for (auto it = named.begin(); it != named.end(); ++it)
                if (std::find_if(keep.begin(), keep.end(), [&](const char* k) { return it.key() == k; }) == keep.end())
                    out_.warnings.push_back(where(l) + ": " + it.key() + " ignored");
        };
        switch (kind) {
            case 'R':
            case 'C':
            case 'L': {
                need(2);
                json c = {{"type", std::string(1, kind)}, {"name", name}, {"nodes", {node(0), node(1)}}};
                const char* key = kind == 'R' ? "r" : kind == 'C' ? "c" : "l";
                // PSpice: "R1 a b [model] value"; the model scales the value (R=, C=, L=),
                // T_ABS near absolute zero makes a resistor noiseless
                size_t at = 2;
                if (pos.size() > 3 && !isNumber(pos[2]) && pos[2][0] != '{' && pos[2][0] != '\'') {
                    const Line* m = modelLine(d, pos[2]);
                    if (!m) throw std::runtime_error("model " + pos[2] + " not found");
                    std::vector<std::string> mp;
                    json mn = json::object();
                    splitParams(tokens(m->text), 3, mp, mn);
                    double scale = 1.0;
                    for (auto it = mn.begin(); it != mn.end(); ++it) {
                        if (it.key() == key && it->is_number()) scale = it->get<double>();
                        else if (it.key() == "t_abs" && it->is_number() && it->get<double>() <= -273.0 && kind == 'R')
                            c["noise"] = false;
                        else if (warnedModels_.insert(lower(pos[2]) + "." + it.key()).second)
                            out_.warnings.push_back(where(l) + ": " + it.key() + " of model " + pos[2] + " ignored");
                    }
                    const json v = value(pos[3]);
                    c["value"] = scale == 1.0 ? v
                                 : v.is_number() ? json(v.get<double>() * scale)
                                                 : json("(" + v.get<std::string>() + ")*" + numberText(scale));
                    at = 4;
                } else if (pos.size() > 2) {
                    c["value"] = value(pos[2]);
                } else if (named.contains(key)) {
                    c["value"] = named[key];
                } else {
                    throw std::runtime_error("no value: " + l.text);
                }
                (void)at;
                if (kind != 'R' && named.contains("ic")) c["ic"] = named["ic"];
                unused({key, "ic"});
                return c;
            }
            case 'K':
                need(3);
                return {{"type", "K"}, {"name", name}, {"inductors", {node(0), node(1)}}, {"k", value(pos[2])}};
            case 'V':
            case 'I':
                need(2);
                return source(t, kind);
            case 'E':
            case 'G':
            case 'F':
            case 'H': {
                json c = controlled(d, l, t);
                if (!c.is_null()) return c;
                if (kind == 'E' || kind == 'G') {
                    need(5);
                    return {{"type", std::string(1, kind)}, {"name", name},
                            {"nodes", {node(0), node(1), node(2), node(3)}}, {kind == 'E' ? "gain" : "gm", value(pos[4])}};
                }
                need(4);
                return {{"type", std::string(1, kind)}, {"name", name}, {"nodes", {node(0), node(1)}},
                        {"control", node(2)}, {kind == 'F' ? "gain" : "r", value(pos[3])}};
            }
            case 'B': {
                // the expression as written: without braces it may hold parentheses and commas
                static const std::regex form(R"(^\S+\s+\S+\s+\S+\s+([vi])\s*=\s*(.*)$)", std::regex::icase);
                std::smatch m;
                if (!std::regex_match(l.text, m, form)) throw std::runtime_error("B needs V= or I=: " + l.text);
                std::string e = trim(m[2].str());
                if (e.size() >= 2 && ((e.front() == '{' && e.back() == '}') || (e.front() == '\'' && e.back() == '\'')))
                    e = e.substr(1, e.size() - 2);
                return {{"type", "B"}, {"name", name}, {"nodes", {node(0), node(1)}}, {lower(m[1].str()), spiceExpression(e)}};
            }
            case 'D': {
                need(3);
                if (pos.size() > 3 && !(isNumber(pos[3]) && value(pos[3]) == json(1.0)))
                    out_.warnings.push_back(where(l) + ": area of " + t[0] + " ignored");
                unused({});
                return {{"type", "D"}, {"name", name}, {"nodes", {node(0), node(1)}}, {"model", modelName(d, pos[2], l)}};
            }
            case 'Q': {
                need(4);
                // collector base emitter [substrate] model [area]
                const bool substrate = pos.size() > 4 && !isNumber(pos[4]);
                if (substrate) out_.warnings.push_back(where(l) + ": substrate node of " + t[0] + " ignored");
                unused({});
                return {{"type", "Q"}, {"name", name}, {"nodes", {node(0), node(1), node(2)}},
                        {"model", modelName(d, pos[substrate ? 4 : 3], l)}};
            }
            case 'J':
                need(4);
                unused({});
                return {{"type", "J"}, {"name", name}, {"nodes", {node(0), node(1), node(2)}},
                        {"model", modelName(d, pos[3], l)}};
            case 'M': {
                need(5);
                json c = {{"type", "M"}, {"name", name}, {"nodes", {node(0), node(1), node(2), node(3)}},
                          {"model", modelName(d, pos[4], l)}};
                for (auto it = named.begin(); it != named.end(); ++it) {
                    if (it.key() == "m" && it.value() != json(1.0))
                        throw std::runtime_error("M=" + it.value().dump() + " (parallel devices) is not supported");
                    if (it.key() != "m") c[it.key()] = it.value();
                }
                return c;
            }
            case 'S': {
                need(5);
                json c = {{"type", "S"}, {"name", name}, {"nodes", {node(0), node(1), node(2), node(3)}}};
                const Line* m = modelLine(d, pos[4]);
                if (!m) throw std::runtime_error("switch model " + pos[4] + " not found");
                const std::vector<std::string> mt = tokens(m->text);
                std::vector<std::string> mp;
                json mn = json::object();
                splitParams(mt, 3, mp, mn);
                if (lower(mt[2]) == "vswitch") return vswitch(name, {node(0), node(1), node(2), node(3)}, mn);
                for (const char* k : {"ron", "roff", "vt", "vh"})
                    if (mn.contains(k)) c[k] = mn[k];
                if (pos.size() > 5 && lower(pos[5]) == "on") c["initial"] = "on";
                return c;
            }
            case 'T': {
                need(4);
                json c = {{"type", "T"}, {"name", name}, {"nodes", {node(0), node(1), node(2), node(3)}}};
                for (const char* k : {"z0", "td", "f", "nl"})
                    if (named.contains(k)) c[k] = named[k];
                return c;
            }
            case 'X': {
                // nodes ..., subcircuit [PARAMS:] name=value ...
                std::vector<std::string> nodes;
                for (const std::string& p : pos)
                    if (lower(p) != "params:") nodes.push_back(p);
                if (nodes.empty()) throw std::runtime_error("X without a subcircuit");
                const std::string sub = nodes.back();
                nodes.pop_back();
                json c = {{"type", "X"}, {"name", name}, {"subcircuit", subcircuitName(d, sub)}};
                json n = json::array();
                for (const std::string& p : nodes) n.push_back(lower(p));
                c["nodes"] = n;
                if (!named.empty()) c["parameters"] = named;
                return c;
            }
            default:
                throw std::runtime_error(std::string("elements of type ") + kind + " are not supported: " + l.text);
        }
    }

    std::string key_;
    Def root_;
    Def* main_ = nullptr;
    std::vector<std::unique_ptr<Def>> defs_;
    std::map<const Def*, std::map<std::string, std::string>> modelNames_;
    std::set<std::string> taken_;
    std::set<std::string> warnedModels_;
    SpiceDefinitions out_;
};

}  // namespace

std::string spiceExpression(const std::string& text) { return ExpressionParser(text).parse(); }

SpiceDefinitions parseSpiceSubcircuit(const std::string& key, const std::string& text) {
    return Translator(key, text).run();
}

}  // namespace esim
