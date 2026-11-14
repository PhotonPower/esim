#pragma once

#include <iosfwd>
#include <string>

#include "io/Netlist.hpp"

namespace esim {

struct RunConfig {
    std::string outDir = ".";   // directory for the CSV files
    std::string stem = "esim";  // file name prefix: <stem>.<analysis>.csv
};

// Run all analyses of the netlist in order. Progress and the operating point
// go to log, sweep and transient results to CSV files.
void runAnalyses(Netlist& nl, const RunConfig& cfg, std::ostream& log);

}  // namespace esim
