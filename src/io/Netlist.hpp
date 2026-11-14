#pragma once

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "circuit/Analysis.hpp"
#include "circuit/Circuit.hpp"

namespace esim {

// A parsed input file: the circuit plus what to do with it.
struct Netlist {
    std::string title;
    std::unique_ptr<Circuit> circuit;
    SimOptions options;
    std::vector<nlohmann::json> analyses;  // interpreted by app/Runner
    std::vector<std::string> outputs;      // probe expressions; empty: everything
    nlohmann::json source;                 // the input, for runs with changed parameters
    std::string baseDir;                   // directory of the input file (for data files)
};

// The input format is documented in README.md. JSON comments (// and /* */)
// are allowed. Errors are reported as std::runtime_error.
// warnings = false suppresses the hints about unknown keys (repeated parsing).
// File names inside the input are relative to baseDir.
Netlist parseNetlist(const nlohmann::json& root, bool warnings = true,
                     const std::string& baseDir = "");
Netlist parseNetlistText(const std::string& text);
Netlist loadNetlist(const std::string& path);

// Number from a JSON value: a JSON number, a string like "4.7k", or a constant
// expression of the given parameters like "2*R0".
double jsonNumber(const nlohmann::json& v, const std::string& where,
                  const std::map<std::string, double>* parameters = nullptr);

}  // namespace esim
