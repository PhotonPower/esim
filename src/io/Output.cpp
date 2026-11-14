#include "io/Output.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "circuit/Device.hpp"
#include "circuit/Symbols.hpp"

namespace esim {

std::complex<double> Probe::phasor(const Eigen::VectorXcd& x) const {
    if (expression)
        throw std::runtime_error("output \"" + label +
                                 "\": the ac analysis supports only v(node), v(a,b), "
                                 "i(component) and variable names, not general expressions");
    return (pos >= 0 ? x[pos] : std::complex<double>(0.0)) -
           (neg >= 0 ? x[neg] : std::complex<double>(0.0));
}

Probe parseProbe(const Circuit& ckt, const std::string& expr) {
    std::string s;
    for (char ch : expr)
        if (!std::isspace(static_cast<unsigned char>(ch))) s += ch;

    if (s.empty()) throw std::runtime_error("output: empty expression");

    Probe p;
    p.label = s;

    // The plain forms stay linear, which the ac analysis needs.
    const int variable = ckt.findVariable(s);
    if (variable >= 0) {
        p.pos = variable;
        return p;
    }
    const char kind = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    const bool plain = s.size() >= 4 && (kind == 'v' || kind == 'i') && s[1] == '(' &&
                       s.find('(', 2) == std::string::npos && s.find(')') == s.size() - 1;
    if (!plain) {
        try {
            p.expression = std::make_shared<expr::Program>(
                expr::Program::compile(expr::parseExpression(s), CircuitSymbols(ckt)));
        } catch (const std::runtime_error& e) {
            throw std::runtime_error(std::string("output ") +
                                     (e.what()[0] == '"' ? "" : "\"" + s + "\": ") + e.what());
        }
        return p;
    }

    const std::string arg = s.substr(2, s.size() - 3);
    try {
        if (kind == 'v') {
            const auto comma = arg.find(',');
            if (comma == std::string::npos) {
                p.pos = ckt.findNode(arg);
            } else {
                p.pos = ckt.findNode(arg.substr(0, comma));
                p.neg = ckt.findNode(arg.substr(comma + 1));
            }
        } else {
            const Device* d = ckt.findDevice(arg);
            if (!d) throw std::runtime_error("unknown component '" + arg + "'");
            if (d->branch() < 0)
                throw std::runtime_error("component '" + arg +
                                         "' has no branch current (only voltage sources, "
                                         "inductors, E and H sources have one)");
            p.pos = d->branch();
        }
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("output \"" + expr + "\": " + e.what());
    }
    return p;
}

std::vector<Probe> makeProbes(const Circuit& ckt, const std::vector<std::string>& exprs) {
    std::vector<Probe> probes;
    if (exprs.empty()) {
        for (int i = 0; i < ckt.size(); ++i)
            if (!ckt.isInternal(i)) probes.push_back({ckt.unknownName(i), i, -1, nullptr});
    } else {
        for (const std::string& e : exprs) probes.push_back(parseProbe(ckt, e));
    }
    return probes;
}

CsvWriter::CsvWriter(const std::string& path, const std::vector<std::string>& header)
    : out_(path) {
    if (!out_) throw std::runtime_error("cannot write '" + path + "'");
    out_.precision(10);
    for (size_t i = 0; i < header.size(); ++i) {
        if (i) out_ << ',';
        // Quote labels like v(a,b) that contain the separator.
        if (header[i].find(',') != std::string::npos) out_ << '"' << header[i] << '"';
        else out_ << header[i];
    }
    out_ << '\n';
}

void CsvWriter::row(const std::vector<double>& values) {
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out_ << ',';
        out_ << values[i];
    }
    out_ << '\n';
}

}  // namespace esim
