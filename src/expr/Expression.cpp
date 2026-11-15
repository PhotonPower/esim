#include "expr/Expression.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace esim::expr {

enum class Program::Op : unsigned char {
    Const, Var, Time,
    Neg, Not,
    Add, Sub, Mul, Div, Pow,
    Lt, Le, Gt, Ge, Eq, Ne, And, Or,
    If,
    Sin, Cos, Tan, Asin, Acos, Atan, Sinh, Cosh, Tanh,
    Exp, Log, Log10, Sqrt, Abs, Sign,
    Min, Max, Atan2,
    Lookup,
    Delay,
};

AstPtr makeNumber(double value);

namespace {

using Op = Program::Op;
using Instr = Program::Instr;

// --- AST construction -------------------------------------------------------

AstPtr makeUnary(const std::string& op, AstPtr a) {
    auto n = std::make_shared<Ast>();
    n->kind = Ast::Kind::Unary;
    n->name = op;
    n->args = {std::move(a)};
    return n;
}

AstPtr makeBinary(const std::string& op, AstPtr a, AstPtr b) {
    auto n = std::make_shared<Ast>();
    n->kind = Ast::Kind::Binary;
    n->name = op;
    n->args = {std::move(a), std::move(b)};
    return n;
}

bool siPrefix(const std::string& s, double& factor) {
    static const std::unordered_map<std::string, double> table = {
        {"f", 1e-15}, {"p", 1e-12}, {"n", 1e-9},  {"u", 1e-6}, {"\xC2\xB5", 1e-6},
        {"\xCE\xBC", 1e-6}, {"m", 1e-3}, {"k", 1e3}, {"K", 1e3}, {"M", 1e6},
        {"meg", 1e6}, {"Meg", 1e6}, {"MEG", 1e6}, {"G", 1e9}, {"T", 1e12},
    };
    auto it = table.find(s);
    if (it == table.end()) return false;
    factor = it->second;
    return true;
}

// --- Parser (recursive descent) ----------------------------------------------
//
//   or      := and ('||' and)*
//   and     := cmp ('&&' cmp)*
//   cmp     := add (('<' | '<=' | '>' | '>=' | '==' | '!=') add)*
//   add     := mul (('+' | '-') mul)*
//   mul     := unary (('*' | '/') unary)*
//   unary   := ('-' | '+' | '!') unary | pow
//   pow     := primary (('^' | '**') unary)?        right associative, -x^2 = -(x^2)
//   primary := number | name | name '(' args ')' | '(' or ')'

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    AstPtr expression() {
        AstPtr e = parseOr();
        expectEnd();
        return e;
    }

    AstPtr equation() {
        AstPtr lhs = parseOr();
        skipWs();
        if (pos_ >= s_.size() || s_[pos_] != '=')
            fail("expected '=' (an equation has the form \"lhs = rhs\")");
        ++pos_;
        AstPtr rhs = parseOr();
        expectEnd();
        return makeBinary("-", std::move(lhs), std::move(rhs));
    }

private:
    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error("\"" + s_ + "\": " + msg + " at position " +
                                 std::to_string(pos_ + 1));
    }

    void skipWs() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool startsWith(const char* tok) const { return s_.compare(pos_, std::strlen(tok), tok) == 0; }

    bool eat(const char* tok) {
        skipWs();
        if (!startsWith(tok)) return false;
        pos_ += std::strlen(tok);
        return true;
    }

    void expectEnd() {
        skipWs();
        if (pos_ != s_.size()) fail(std::string("unexpected '") + s_[pos_] + "'");
    }

    AstPtr parseOr() {
        AstPtr a = parseAnd();
        while (eat("||")) a = makeBinary("||", a, parseAnd());
        return a;
    }

    AstPtr parseAnd() {
        AstPtr a = parseCmp();
        while (eat("&&")) a = makeBinary("&&", a, parseCmp());
        return a;
    }

    AstPtr parseCmp() {
        AstPtr a = parseAdd();
        for (;;) {
            skipWs();
            const char* op = nullptr;
            for (const char* candidate : {"<=", ">=", "==", "!=", "<", ">"}) {
                if (startsWith(candidate)) {
                    op = candidate;
                    break;
                }
            }
            if (!op) return a;
            pos_ += std::strlen(op);
            a = makeBinary(op, a, parseAdd());
        }
    }

    AstPtr parseAdd() {
        AstPtr a = parseMul();
        for (;;) {
            if (eat("+")) a = makeBinary("+", a, parseMul());
            else if (eat("-")) a = makeBinary("-", a, parseMul());
            else return a;
        }
    }

    AstPtr parseMul() {
        AstPtr a = parseUnary();
        for (;;) {
            if (eat("*")) a = makeBinary("*", a, parseUnary());
            else if (eat("/")) a = makeBinary("/", a, parseUnary());
            else return a;
        }
    }

    AstPtr parseUnary() {
        if (eat("-")) return makeUnary("-", parseUnary());
        if (eat("+")) return parseUnary();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '!' && !startsWith("!=")) {
            ++pos_;
            return makeUnary("!", parseUnary());
        }
        return parsePow();
    }

    AstPtr parsePow() {
        AstPtr base = parsePrimary();
        if (eat("**") || eat("^")) return makeBinary("^", base, parseUnary());
        return base;
    }

    AstPtr parsePrimary() {
        skipWs();
        if (pos_ >= s_.size()) fail("unexpected end of expression");
        const char c = s_[pos_];
        if (c == '(') {
            ++pos_;
            AstPtr e = parseOr();
            if (!eat(")")) fail("expected ')'");
            return e;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') return parseNumber();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return parseName();
        fail(std::string("unexpected '") + c + "'");
    }

    AstPtr parseNumber() {
        const char* begin = s_.c_str() + pos_;
        char* end = nullptr;
        double v = std::strtod(begin, &end);
        if (end == begin) fail("invalid number");
        pos_ += static_cast<size_t>(end - begin);

        // Letters directly behind a number must be an SI prefix. Units are not
        // accepted here: "2pi" should be an error, not 2e-12.
        size_t q = pos_;
        while (q < s_.size() && (std::isalpha(static_cast<unsigned char>(s_[q])) ||
                                 static_cast<unsigned char>(s_[q]) >= 0x80))
            ++q;
        if (q > pos_) {
            const std::string suffix = s_.substr(pos_, q - pos_);
            double factor;
            if (!siPrefix(suffix, factor))
                fail("unexpected '" + suffix +
                     "' after a number (SI prefixes: f p n u m k M meg G T; write 2*x for a "
                     "product)");
            v *= factor;
            pos_ = q;
        }
        return makeNumber(v);
    }

    AstPtr parseName() {
        const size_t start = pos_;
        while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) ||
                                    s_[pos_] == '_' || s_[pos_] == '.'))
            ++pos_;
        const std::string name = s_.substr(start, pos_ - start);

        skipWs();
        auto node = std::make_shared<Ast>();
        node->name = name;
        if (pos_ >= s_.size() || s_[pos_] != '(') {
            node->kind = Ast::Kind::Symbol;
            return node;
        }
        ++pos_;  // '('

        // v(node), v(a,b), i(component): the arguments are names, not expressions.
        if (name == "v" || name == "V" || name == "i" || name == "I") {
            const size_t close = s_.find(')', pos_);
            if (close == std::string::npos) fail("expected ')'");
            node->kind = Ast::Kind::Probe;
            node->name = (name == "v" || name == "V") ? "v" : "i";
            size_t from = pos_;
            while (from <= close) {
                size_t to = s_.find(',', from);
                if (to == std::string::npos || to > close) to = close;
                std::string item = s_.substr(from, to - from);
                const size_t first = item.find_first_not_of(" \t");
                const size_t last = item.find_last_not_of(" \t");
                if (first == std::string::npos) fail("empty name in " + node->name + "(...)");
                node->names.push_back(item.substr(first, last - first + 1));
                from = to + 1;
            }
            const size_t maxNames = node->name == "v" ? 2 : 1;
            if (node->names.size() > maxNames)
                fail(node->name == "v" ? "v() takes one or two node names"
                                       : "i() takes one component name");
            pos_ = close + 1;
            return node;
        }

        node->kind = Ast::Kind::Call;
        if (!eat(")")) {
            do node->args.push_back(parseOr());
            while (eat(","));
            if (!eat(")")) fail("expected ')' or ','");
        }
        return node;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

// --- Operations ---------------------------------------------------------------

double applyOp(Op op, double a, double b, double c) {
    switch (op) {
        case Op::Neg: return -a;
        case Op::Not: return a == 0.0 ? 1.0 : 0.0;
        case Op::Add: return a + b;
        case Op::Sub: return a - b;
        case Op::Mul: return a * b;
        case Op::Div: return a / b;
        case Op::Pow: return std::pow(a, b);
        case Op::Lt: return a < b ? 1.0 : 0.0;
        case Op::Le: return a <= b ? 1.0 : 0.0;
        case Op::Gt: return a > b ? 1.0 : 0.0;
        case Op::Ge: return a >= b ? 1.0 : 0.0;
        case Op::Eq: return a == b ? 1.0 : 0.0;
        case Op::Ne: return a != b ? 1.0 : 0.0;
        case Op::And: return (a != 0.0 && b != 0.0) ? 1.0 : 0.0;
        case Op::Or: return (a != 0.0 || b != 0.0) ? 1.0 : 0.0;
        case Op::If: return a != 0.0 ? b : c;
        case Op::Sin: return std::sin(a);
        case Op::Cos: return std::cos(a);
        case Op::Tan: return std::tan(a);
        case Op::Asin: return std::asin(a);
        case Op::Acos: return std::acos(a);
        case Op::Atan: return std::atan(a);
        case Op::Sinh: return std::sinh(a);
        case Op::Cosh: return std::cosh(a);
        case Op::Tanh: return std::tanh(a);
        case Op::Exp: return std::exp(a);
        case Op::Log: return std::log(a);
        case Op::Log10: return std::log10(a);
        case Op::Sqrt: return std::sqrt(a);
        case Op::Abs: return std::abs(a);
        case Op::Sign: return a > 0.0 ? 1.0 : a < 0.0 ? -1.0 : 0.0;
        case Op::Min: return std::min(a, b);
        case Op::Max: return std::max(a, b);
        case Op::Atan2: return std::atan2(a, b);
        case Op::Const:
        case Op::Var:
        case Op::Time:
        case Op::Lookup:
        case Op::Delay: break;
    }
    return 0.0;
}

struct Function {
    Op op;
    size_t arity;
};

const Function* findFunction(const std::string& name) {
    static const std::unordered_map<std::string, Function> table = {
        {"sin", {Op::Sin, 1}},     {"cos", {Op::Cos, 1}},     {"tan", {Op::Tan, 1}},
        {"asin", {Op::Asin, 1}},   {"acos", {Op::Acos, 1}},   {"atan", {Op::Atan, 1}},
        {"sinh", {Op::Sinh, 1}},   {"cosh", {Op::Cosh, 1}},   {"tanh", {Op::Tanh, 1}},
        {"exp", {Op::Exp, 1}},     {"log", {Op::Log, 1}},     {"ln", {Op::Log, 1}},
        {"log10", {Op::Log10, 1}}, {"sqrt", {Op::Sqrt, 1}},   {"abs", {Op::Abs, 1}},
        {"sign", {Op::Sign, 1}},   {"min", {Op::Min, 2}},     {"max", {Op::Max, 2}},
        {"atan2", {Op::Atan2, 2}}, {"pow", {Op::Pow, 2}},     {"if", {Op::If, 3}},
    };
    auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

Op binaryOp(const std::string& op) {
    static const std::unordered_map<std::string, Op> table = {
        {"+", Op::Add}, {"-", Op::Sub},  {"*", Op::Mul},  {"/", Op::Div},  {"^", Op::Pow},
        {"<", Op::Lt},  {"<=", Op::Le},  {">", Op::Gt},   {">=", Op::Ge},  {"==", Op::Eq},
        {"!=", Op::Ne}, {"&&", Op::And}, {"||", Op::Or},
    };
    return table.at(op);
}

// --- Compiler: AST -> instruction list -----------------------------------------

class Compiler {
public:
    Compiler(const Symbols& symbols, std::vector<Instr>& code, std::vector<int>& vars,
             std::vector<int>& events, std::vector<std::shared_ptr<const Table>>& tables,
             std::vector<std::shared_ptr<DelayLine>>& delays, bool& usesTime)
        : symbols_(symbols), code_(code), vars_(vars), events_(events), tables_(tables),
          delays_(delays), usesTime_(usesTime) {}

    int emit(const AstPtr& n) {
        switch (n->kind) {
            case Ast::Kind::Number: return constant(n->number);
            case Ast::Kind::Symbol: return symbol(n->name);
            case Ast::Kind::Probe: return probe(*n);
            case Ast::Kind::Unary: {
                const int a = emit(n->args[0]);
                return push(n->name == "-" ? Op::Neg : Op::Not, a);
            }
            case Ast::Kind::Binary: {
                const int a = emit(n->args[0]);
                const int b = emit(n->args[1]);
                const Op op = binaryOp(n->name);
                int e = -1;
                if (op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge)
                    e = noteEvent(push(Op::Sub, a, b));
                return push(op, a, b, -1, e);
            }
            case Ast::Kind::Call: {
                if (n->name == "der")
                    throw std::runtime_error("der() is only allowed in equations");
                if (n->name == "delay") return delay(*n);
                const Function* fn = findFunction(n->name);
                if (!fn) {
                    if (const FunctionDef* def = symbols_.function(n->name)) return call(*def, *n);
                    const std::shared_ptr<const Table> tb = symbols_.table(n->name);
                    if (!tb) throw std::runtime_error("unknown function '" + n->name + "'");
                    if (n->args.size() != 1)
                        throw std::runtime_error("table '" + n->name + "' takes one argument");
                    const int arg = emit(n->args[0]);
                    if (code_[arg].op == Op::Const) {
                        double slope;
                        return constant(tb->eval(code_[arg].k, slope));
                    }
                    Instr in{Op::Lookup};
                    in.a = arg;
                    in.table = static_cast<int>(tables_.size());
                    tables_.push_back(tb);
                    code_.push_back(in);
                    return static_cast<int>(code_.size()) - 1;
                }
                if (n->args.size() != fn->arity)
                    throw std::runtime_error("function '" + n->name + "' takes " +
                                             std::to_string(fn->arity) + " argument(s)");
                int slot[3] = {-1, -1, -1};
                for (size_t k = 0; k < n->args.size(); ++k) slot[k] = emit(n->args[k]);
                int e = -1;
                if (fn->op == Op::Abs || fn->op == Op::Sign) e = noteEvent(slot[0]);
                if (fn->op == Op::Min || fn->op == Op::Max)
                    e = noteEvent(push(Op::Sub, slot[0], slot[1]));
                return push(fn->op, slot[0], slot[1], slot[2], e);
            }
        }
        return -1;
    }

private:
    // Call of a user-defined function: compile its body with the arguments
    // substituted for the parameters.
    int call(const FunctionDef& def, const Ast& n) {
        if (n.args.size() != def.params.size())
            throw std::runtime_error("function '" + n.name + "' takes " +
                                     std::to_string(def.params.size()) + " argument(s)");
        if (++depth_ > 50)
            throw std::runtime_error("function '" + n.name + "' calls itself without end");
        const AstPtr body = mapLeaves(def.body, [&](const Ast& leaf) -> AstPtr {
            if (leaf.kind != Ast::Kind::Symbol) return nullptr;
            for (size_t k = 0; k < def.params.size(); ++k)
                if (def.params[k] == leaf.name) return n.args[k];
            return nullptr;
        });
        const int slot = emit(body);
        --depth_;
        return slot;
    }

    // delay(e, td): the argument is compiled twice, into this program (its
    // present value, used outside of a time integration) and into a program
    // of its own from which the history is recorded.
    int delay(const Ast& n) {
        if (n.args.size() != 2) throw std::runtime_error("delay() takes two arguments: delay(x, td)");
        const int tdSlot = emit(n.args[1]);
        if (code_[tdSlot].op != Op::Const || !(code_[tdSlot].k > 0.0))
            throw std::runtime_error("delay(x, td): td must be a positive constant");
        const int arg = emit(n.args[0]);
        if (code_[arg].op == Op::Const) return arg;
        auto line = std::make_shared<DelayLine>();
        line->inner = Program::compile(n.args[0], symbols_);
        line->td = code_[tdSlot].k;
        if (!symbols_.addDelay(line)) throw std::runtime_error("delay() is not available here");
        Instr in{Op::Delay};
        in.a = arg;
        in.table = static_cast<int>(delays_.size());
        delays_.push_back(line);
        usesTime_ = true;
        code_.push_back(in);
        return static_cast<int>(code_.size()) - 1;
    }

    int constant(double v) {
        Instr in{Op::Const};
        in.k = v;
        code_.push_back(in);
        return static_cast<int>(code_.size()) - 1;
    }

    int variable(int unknown) {
        size_t pos = 0;
        while (pos < vars_.size() && vars_[pos] != unknown) ++pos;
        if (pos == vars_.size()) vars_.push_back(unknown);
        Instr in{Op::Var};
        in.a = static_cast<int>(pos);
        code_.push_back(in);
        return static_cast<int>(code_.size()) - 1;
    }

    // Variables shadow constants, constants shadow the built-in names.
    int symbol(const std::string& name) {
        const int unknown = symbols_.variable(name);
        if (unknown >= 0) return variable(unknown);
        double value;
        if (symbols_.constant(name, value)) return constant(value);
        if (name == "t" || name == "time") {
            usesTime_ = true;
            code_.push_back(Instr{Op::Time});
            return static_cast<int>(code_.size()) - 1;
        }
        if (name == "pi") return constant(kPi);
        throw std::runtime_error("unknown symbol '" + name + "'");
    }

    int probe(const Ast& n) {
        if (n.name == "i") return variable(symbols_.branchCurrent(n.names[0]));
        auto voltage = [&](const std::string& node) {
            const int idx = symbols_.nodeVoltage(node);
            return idx < 0 ? constant(0.0) : variable(idx);
        };
        const int a = voltage(n.names[0]);
        if (n.names.size() == 1) return a;
        return push(Op::Sub, a, voltage(n.names[1]));
    }

    // A slot whose sign change is an event (unless it is a constant). Returns
    // the index of the event function or -1.
    int noteEvent(int slot) {
        if (code_[slot].op == Op::Const) return -1;
        events_.push_back(slot);
        return static_cast<int>(events_.size()) - 1;
    }

    // Append an operation; fold it if all operands are constants.
    int push(Op op, int a, int b = -1, int c = -1, int e = -1) {
        auto isConst = [&](int s) { return s < 0 || code_[s].op == Op::Const; };
        if (isConst(a) && isConst(b) && isConst(c)) {
            auto value = [&](int s) { return s < 0 ? 0.0 : code_[s].k; };
            return constant(applyOp(op, value(a), value(b), value(c)));
        }
        Instr in{op};
        in.a = a;
        in.b = b;
        in.c = c;
        in.e = e;
        code_.push_back(in);
        return static_cast<int>(code_.size()) - 1;
    }

    const Symbols& symbols_;
    std::vector<Instr>& code_;
    std::vector<int>& vars_;
    std::vector<int>& events_;
    std::vector<std::shared_ptr<const Table>>& tables_;
    std::vector<std::shared_ptr<DelayLine>>& delays_;
    bool& usesTime_;
    int depth_ = 0;  // nesting of user-defined function calls
};

// --- Equations: separate the der() terms ----------------------------------------

bool containsDer(const AstPtr& n) {
    if (n->kind == Ast::Kind::Call && n->name == "der") return true;
    for (const AstPtr& a : n->args)
        if (containsDer(a)) return true;
    return false;
}

struct Term {
    double coeff;
    AstPtr expr;
};

// Value of a subexpression that must not depend on unknowns or time.
bool constantValue(const AstPtr& n, const Symbols& symbols, double& value) {
    const Program p = Program::compile(n, symbols);
    if (!p.isConstant()) return false;
    value = p.eval(Vector(), 0.0);
    return true;
}

// Decompose coeff * n into  sum c_k * der(q_k)  +  sum c_j * f_j.
void splitTerms(const AstPtr& n, double coeff, const Symbols& symbols, std::vector<Term>& q,
                std::vector<Term>& f) {
    if (!containsDer(n)) {
        f.push_back({coeff, n});
        return;
    }
    const auto nonlinear = []() {
        return std::runtime_error(
            "der() must enter an equation linearly with a constant factor, e.g. "
            "\"m*der(v) = ...\". Otherwise introduce an auxiliary variable: instead of "
            "\"x*der(y) = ...\" write \"der(y) = dy\" and \"x*dy = ...\"");
    };
    if (n->kind == Ast::Kind::Call) {  // der(...)
        if (n->args.size() != 1) throw std::runtime_error("der() takes one argument");
        if (containsDer(n->args[0])) throw std::runtime_error("der() cannot be nested");
        q.push_back({coeff, n->args[0]});
        return;
    }
    if (n->kind == Ast::Kind::Unary && n->name == "-") {
        splitTerms(n->args[0], -coeff, symbols, q, f);
        return;
    }
    if (n->kind == Ast::Kind::Binary) {
        const AstPtr& a = n->args[0];
        const AstPtr& b = n->args[1];
        double factor;
        if (n->name == "+") {
            splitTerms(a, coeff, symbols, q, f);
            splitTerms(b, coeff, symbols, q, f);
            return;
        }
        if (n->name == "-") {
            splitTerms(a, coeff, symbols, q, f);
            splitTerms(b, -coeff, symbols, q, f);
            return;
        }
        if (n->name == "*") {
            if (!containsDer(a) && constantValue(a, symbols, factor)) {
                splitTerms(b, coeff * factor, symbols, q, f);
                return;
            }
            if (!containsDer(b) && constantValue(b, symbols, factor)) {
                splitTerms(a, coeff * factor, symbols, q, f);
                return;
            }
        }
        if (n->name == "/" && !containsDer(b) && constantValue(b, symbols, factor)) {
            splitTerms(a, coeff / factor, symbols, q, f);
            return;
        }
    }
    throw nonlinear();
}

AstPtr sumOfTerms(const std::vector<Term>& terms) {
    AstPtr sum;
    for (const Term& t : terms) {
        AstPtr term = t.coeff == 1.0 ? t.expr : makeBinary("*", makeNumber(t.coeff), t.expr);
        sum = sum ? makeBinary("+", sum, term) : term;
    }
    return sum ? sum : makeNumber(0.0);
}

}  // namespace

// --- Public interface ---------------------------------------------------------

// --- Table ---------------------------------------------------------------------

Table::Table(std::vector<double> x, std::vector<double> y, Interp interp, Extrap extrap)
    : x_(std::move(x)), y_(std::move(y)), interp_(interp), extrap_(extrap) {
    if (x_.size() != y_.size()) throw std::runtime_error("x and y have different lengths");
    if (x_.size() < 2) throw std::runtime_error("at least two points are required");
    for (size_t k = 1; k < x_.size(); ++k)
        if (!(x_[k] > x_[k - 1])) throw std::runtime_error("x must be strictly increasing");

    // Slopes at the points. Linear: only the outer ones are used (extrapolation).
    const size_t n = x_.size();
    std::vector<double> secant(n - 1);
    for (size_t k = 0; k + 1 < n; ++k) secant[k] = (y_[k + 1] - y_[k]) / (x_[k + 1] - x_[k]);
    d_.assign(n, 0.0);
    d_.front() = secant.front();
    d_.back() = secant.back();
    if (interp_ == Interp::Cubic) {
        // Fritsch-Carlson: weighted harmonic mean of the neighbouring secants,
        // zero at local extrema. Keeps the curve monotone between the points.
        for (size_t k = 1; k + 1 < n; ++k) {
            const double a = secant[k - 1], b = secant[k];
            if (a * b <= 0.0) continue;
            const double ha = x_[k] - x_[k - 1], hb = x_[k + 1] - x_[k];
            const double w1 = 2.0 * hb + ha, w2 = hb + 2.0 * ha;
            d_[k] = (w1 + w2) / (w1 / a + w2 / b);
        }
        // End points: one-sided three-point estimate, limited so that the
        // curve stays monotone.
        if (n > 2) {
            auto endSlope = [](double h0, double h1, double s0, double s1) {
                double d = ((2.0 * h0 + h1) * s0 - h0 * s1) / (h0 + h1);
                if (d * s0 <= 0.0) d = 0.0;
                else if (s0 * s1 <= 0.0 && std::abs(d) > 3.0 * std::abs(s0)) d = 3.0 * s0;
                return d;
            };
            d_.front() = endSlope(x_[1] - x_[0], x_[2] - x_[1], secant[0], secant[1]);
            d_.back() = endSlope(x_[n - 1] - x_[n - 2], x_[n - 2] - x_[n - 3], secant[n - 2],
                                 secant[n - 3]);
        }
    }
}

double Table::eval(double x, double& slope) const {
    const size_t n = x_.size();
    if (x <= x_.front() || x >= x_.back()) {
        const bool low = x <= x_.front();
        const double xe = low ? x_.front() : x_.back();
        const double ye = low ? y_.front() : y_.back();
        slope = extrap_ == Extrap::Linear ? (low ? d_.front() : d_.back()) : 0.0;
        return ye + slope * (x - xe);
    }
    const size_t k = std::upper_bound(x_.begin(), x_.end(), x) - x_.begin() - 1;  // x_[k] <= x
    const double h = x_[k + 1] - x_[k], s = (x - x_[k]) / h;
    if (interp_ == Interp::Linear || k + 1 >= n) {
        slope = (y_[k + 1] - y_[k]) / h;
        return y_[k] + slope * (x - x_[k]);
    }
    // Cubic Hermite polynomial on the interval.
    const double s2 = s * s, s3 = s2 * s;
    const double h00 = 2.0 * s3 - 3.0 * s2 + 1.0, h10 = s3 - 2.0 * s2 + s;
    const double h01 = -2.0 * s3 + 3.0 * s2, h11 = s3 - s2;
    slope = ((6.0 * s2 - 6.0 * s) * y_[k] + (3.0 * s2 - 4.0 * s + 1.0) * h * d_[k] +
             (-6.0 * s2 + 6.0 * s) * y_[k + 1] + (3.0 * s2 - 2.0 * s) * h * d_[k + 1]) / h;
    return h00 * y_[k] + h10 * h * d_[k] + h01 * y_[k + 1] + h11 * h * d_[k + 1];
}

AstPtr makeNumber(double v) {
    auto n = std::make_shared<Ast>();
    n->kind = Ast::Kind::Number;
    n->number = v;
    return n;
}

AstPtr mapLeaves(const AstPtr& ast, const std::function<AstPtr(const Ast&)>& replace) {
    if (ast->kind == Ast::Kind::Symbol || ast->kind == Ast::Kind::Probe) {
        AstPtr r = replace(*ast);
        return r ? r : ast;
    }
    if (ast->args.empty()) return ast;
    auto copy = std::make_shared<Ast>(*ast);
    for (AstPtr& a : copy->args) a = mapLeaves(a, replace);
    return copy;
}

bool isBuiltinFunction(const std::string& name) {
    return findFunction(name) != nullptr || name == "der" || name == "delay";
}

AstPtr parseExpression(const std::string& text) { return Parser(text).expression(); }

AstPtr parseEquation(const std::string& text) { return Parser(text).equation(); }

bool ConstantSymbols::constant(const std::string& name, double& value) const {
    auto it = constants_.find(name);
    if (it == constants_.end()) return false;
    value = it->second;
    return true;
}

int ConstantSymbols::nodeVoltage(const std::string& node) const {
    throw std::runtime_error("v(" + node + ") is not available here: a constant is required");
}

int ConstantSymbols::branchCurrent(const std::string& component) const {
    throw std::runtime_error("i(" + component + ") is not available here: a constant is required");
}

Program Program::compile(const AstPtr& ast, const Symbols& symbols) {
    Program p;
    Compiler compiler(symbols, p.code_, p.vars_, p.events_, p.tables_, p.delays_, p.usesTime_);
    const int result = compiler.emit(ast);
    // The result has to be the last slot (a folded constant may sit earlier).
    if (result != static_cast<int>(p.code_.size()) - 1) p.code_.push_back(p.code_[result]);
    p.val_.resize(p.code_.size());
    p.adj_.resize(p.code_.size());
    p.modes_.assign(p.events_.size(), 0);
    return p;
}

void Program::run(const Vector& x, double t, bool frozen) const {
    for (size_t i = 0; i < code_.size(); ++i) {
        const Instr& in = code_[i];
        switch (in.op) {
            case Op::Const: val_[i] = in.k; continue;
            case Op::Var: val_[i] = x[vars_[in.a]]; continue;
            case Op::Time: val_[i] = t; continue;
            default: break;
        }
        const double a = val_[in.a];
        if (in.op == Op::Lookup) {
            double slope;
            val_[i] = tables_[in.table]->eval(a, slope);
            continue;
        }
        if (in.op == Op::Delay) {
            const DelayLine& line = *delays_[in.table];
            val_[i] = line.active() ? line.value(t) : a;
            continue;
        }
        const double b = in.b >= 0 ? val_[in.b] : 0.0;
        if (frozen && in.e >= 0) {
            // The stored mode says whether the event function (a - b, or the
            // argument of abs and sign) was positive.
            const bool positive = modes_[in.e] != 0;
            switch (in.op) {
                case Op::Lt:
                case Op::Le: val_[i] = positive ? 0.0 : 1.0; continue;
                case Op::Gt:
                case Op::Ge: val_[i] = positive ? 1.0 : 0.0; continue;
                case Op::Abs: val_[i] = positive ? a : -a; continue;
                case Op::Sign: val_[i] = positive ? 1.0 : -1.0; continue;
                case Op::Min: val_[i] = positive ? b : a; continue;
                case Op::Max: val_[i] = positive ? a : b; continue;
                default: break;
            }
        }
        val_[i] = applyOp(in.op, a, b, in.c >= 0 ? val_[in.c] : 0.0);
    }
}

void Program::events(const Vector& x, double t, double* out, bool frozen) const {
    if (events_.empty()) return;
    run(x, t, frozen);
    for (size_t k = 0; k < events_.size(); ++k) out[k] = val_[events_[k]];
}

void Program::updateModes(const Vector& x, double t) {
    if (events_.empty()) return;
    run(x, t, false);
    for (size_t k = 0; k < events_.size(); ++k) modes_[k] = val_[events_[k]] > 0.0;
}

double Program::eval(const Vector& x, double t, bool frozen) const {
    if (code_.empty()) return 0.0;
    run(x, t, frozen);
    return val_.back();
}

double Program::evalGrad(const Vector& x, double t, std::vector<double>& grad,
                         bool frozen) const {
    grad.assign(vars_.size(), 0.0);
    if (code_.empty()) return 0.0;
    run(x, t, frozen);
    const auto positive = [&](const Instr& in, bool live) {
        return (frozen && in.e >= 0) ? modes_[in.e] != 0 : live;
    };

    // Reverse sweep: adj_[i] = d result / d slot i.
    std::fill(adj_.begin(), adj_.end(), 0.0);
    adj_.back() = 1.0;
    for (size_t i = code_.size(); i-- > 0;) {
        const double w = adj_[i];
        if (w == 0.0) continue;  // also keeps NaNs of unused if() branches out
        const Instr& in = code_[i];
        const double a = in.a >= 0 && in.op != Op::Var ? val_[in.a] : 0.0;
        const double b = in.b >= 0 ? val_[in.b] : 0.0;
        const double v = val_[i];
        switch (in.op) {
            case Op::Const:
            case Op::Time: break;
            case Op::Var: grad[in.a] += w; break;
            case Op::Neg: adj_[in.a] -= w; break;
            case Op::Add: adj_[in.a] += w; adj_[in.b] += w; break;
            case Op::Sub: adj_[in.a] += w; adj_[in.b] -= w; break;
            case Op::Mul: adj_[in.a] += w * b; adj_[in.b] += w * a; break;
            case Op::Div: adj_[in.a] += w / b; adj_[in.b] -= w * v / b; break;
            case Op::Pow:
                if (b != 0.0) adj_[in.a] += w * b * std::pow(a, b - 1.0);
                if (code_[in.b].op != Op::Const && a > 0.0) adj_[in.b] += w * v * std::log(a);
                break;
            case Op::If: adj_[a != 0.0 ? in.b : in.c] += w; break;
            case Op::Lookup: {
                double slope;
                tables_[in.table]->eval(a, slope);
                adj_[in.a] += w * slope;
                break;
            }
            case Op::Delay:  // a value from the past does not depend on the present
                if (!delays_[in.table]->active()) adj_[in.a] += w;
                break;
            case Op::Sin: adj_[in.a] += w * std::cos(a); break;
            case Op::Cos: adj_[in.a] -= w * std::sin(a); break;
            case Op::Tan: adj_[in.a] += w * (1.0 + v * v); break;
            case Op::Asin: adj_[in.a] += w / std::sqrt(1.0 - a * a); break;
            case Op::Acos: adj_[in.a] -= w / std::sqrt(1.0 - a * a); break;
            case Op::Atan: adj_[in.a] += w / (1.0 + a * a); break;
            case Op::Sinh: adj_[in.a] += w * std::cosh(a); break;
            case Op::Cosh: adj_[in.a] += w * std::sinh(a); break;
            case Op::Tanh: adj_[in.a] += w * (1.0 - v * v); break;
            case Op::Exp: adj_[in.a] += w * v; break;
            case Op::Log: adj_[in.a] += w / a; break;
            case Op::Log10: adj_[in.a] += w / (a * 2.302585092994046); break;
            case Op::Sqrt: adj_[in.a] += w * 0.5 / v; break;
            case Op::Abs: adj_[in.a] += w * (positive(in, a >= 0.0) ? 1.0 : -1.0); break;
            case Op::Min: adj_[positive(in, a > b) ? in.b : in.a] += w; break;
            case Op::Max: adj_[positive(in, a >= b) ? in.a : in.b] += w; break;
            case Op::Atan2: {
                const double r2 = a * a + b * b;
                adj_[in.a] += w * b / r2;
                adj_[in.b] -= w * a / r2;
                break;
            }
            // Piecewise constant: derivative 0.
            case Op::Not: case Op::Sign:
            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
            case Op::Eq: case Op::Ne: case Op::And: case Op::Or: break;
        }
    }
    return val_.back();
}

double evalConstant(const std::string& text, const std::map<std::string, double>& constants) {
    try {
        const ConstantSymbols symbols(constants);
        const Program p = Program::compile(parseExpression(text), symbols);
        if (!p.isConstant()) throw std::runtime_error("must not depend on time");
        return p.eval(Vector(), 0.0);
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        if (msg.rfind("\"" + text + "\"", 0) == 0) throw;  // syntax errors quote the text
        throw std::runtime_error("\"" + text + "\": " + msg);
    }
}

DaeEquation compileEquation(const AstPtr& residual, const Symbols& symbols) {
    std::vector<Term> q, f;
    splitTerms(residual, 1.0, symbols, q, f);
    DaeEquation eq;
    eq.f = Program::compile(sumOfTerms(f), symbols);
    eq.differential = !q.empty();
    if (eq.differential) {
        eq.q = Program::compile(sumOfTerms(q), symbols);
        if (eq.q.dependsOnTime())
            throw std::runtime_error("the argument of der() must not depend on time explicitly");
    }
    return eq;
}

DaeEquation compileEquation(const std::string& text, const Symbols& symbols) {
    try {
        return compileEquation(parseEquation(text), symbols);
    } catch (const std::runtime_error& e) {
        // Syntax errors already quote the text.
        const std::string msg = e.what();
        if (msg.rfind("\"" + text + "\"", 0) == 0) throw;
        throw std::runtime_error("\"" + text + "\": " + msg);
    }
}

}  // namespace esim::expr
