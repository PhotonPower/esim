#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace esim {

// Subcircuits written in SPICE, as manufacturers publish their models:
//
//   "subcircuits": { "LM741": [".subckt LM741 1 2 3 4 5", "R1 1 2 10k", ..., ".ends"] }
//
// (one string, or one string per line). The text may hold further
// .subckt and .model definitions, at its top level or inside the
// subcircuit; they belong to it and are renamed "LM741/NAME", so that they
// cannot collide with the definitions of the file.
//
// Understood: R C L K V I (DC, AC, SIN, PULSE, PWL), E G (linear, POLY(n),
// VALUE={expr}, TABLE {expr} = (x, y) ...), F H (linear, POLY(n)), B (V= or
// I=), D Q M J S T X, .param, PARAMS: on .subckt and X lines, .model.
// Anything else is an error that names the line.
struct SpiceDefinitions {
    std::map<std::string, nlohmann::json> subcircuits;  // name -> definition in the JSON form
    std::map<std::string, std::string> models;          // name -> ".model" card
    std::vector<std::string> warnings;
};

// The definitions of the SPICE text given for subcircuit `key`; the .subckt
// of that name (ignoring case; or the only one in the text) becomes `key`.
SpiceDefinitions parseSpiceSubcircuit(const std::string& key, const std::string& text);

// An expression of SPICE (LTspice, PSpice, ngspice: "{...}" without the
// braces) in the syntax of esim: limit(), pwr(), pwrs(), uramp(), u(),
// stp(), sgn(), "**", "a ? b : c", numbers with SPICE suffixes ("1meg").
std::string spiceExpression(const std::string& text);

}  // namespace esim
