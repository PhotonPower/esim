#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "app/Runner.hpp"
#include "circuit/Device.hpp"
#include "io/Netlist.hpp"

namespace {

void usage() {
    std::cout << "usage: esim <netlist.json> [-o <output directory>]\n"
                 "\n"
                 "Runs the analyses listed in the netlist. Sweep and transient results\n"
                 "are written to <output directory>/<netlist name>.<analysis>.csv\n"
                 "(default output directory: current directory).\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;
    esim::RunConfig cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "error: -o needs a directory\n";
                return 2;
            }
            cfg.outDir = argv[++i];
        } else if (input.empty() && !arg.empty() && arg[0] != '-') {
            input = arg;
        } else {
            std::cerr << "error: unexpected argument '" << arg << "'\n";
            return 2;
        }
    }
    if (input.empty()) {
        usage();
        return 2;
    }

    try {
        esim::Netlist nl = esim::loadNetlist(input);
        cfg.stem = std::filesystem::path(input).stem().string();

        const esim::Circuit& ckt = *nl.circuit;
        int voltages = 0, currents = 0, variables = 0;
        for (int i = 0; i < ckt.size(); ++i) {
            switch (ckt.kind(i)) {
                case esim::UnknownKind::Voltage: ++voltages; break;
                case esim::UnknownKind::Current: ++currents; break;
                case esim::UnknownKind::Variable: ++variables; break;
            }
        }
        const size_t components = ckt.devices().size() - (variables > 0 ? 1 : 0);
        if (!nl.title.empty()) std::cout << nl.title << '\n';
        std::cout << components << " components, " << ckt.size() << " unknowns (" << voltages
                  << " node voltages, " << currents << " branch currents, " << variables
                  << " equation variables)\n";

        esim::runAnalyses(nl, cfg, std::cout);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
