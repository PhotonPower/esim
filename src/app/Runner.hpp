#pragma once

#include <iosfwd>
#include <string>

#include "io/Netlist.hpp"

namespace esim {

struct RunConfig {
    std::string outDir = ".";   // directory for the CSV files
    std::string stem = "esim";  // file name prefix: <stem>.<analysis>.csv
    // If set, a machine-readable description of every analysis is appended:
    // type, name, result files, operating point values, measurements, statistics.
    nlohmann::json* summary = nullptr;
    // Runs of a series (parameter sweep, Monte Carlo, sensitivities, fit) that
    // are computed at the same time. 0: as many as the machine has cores.
    int threads = 0;
};

// Run all analyses of the netlist in order. Progress and the operating point
// go to log, sweep and transient results to CSV files.
void runAnalyses(Netlist& nl, const RunConfig& cfg, std::ostream& log);

}  // namespace esim
