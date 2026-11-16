#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // SetConsoleOutputCP
#endif

#include "app/Runner.hpp"
#include "circuit/Device.hpp"
#include "io/Netlist.hpp"

namespace {

void usage() {
    std::cout << "usage: esim <netlist.json> [-o <output directory>] [--summary <file.json>] [-j <n>]\n"
                 "            [--progress]\n"
                 "\n"
                 "Runs the analyses listed in the netlist. Sweep and transient results\n"
                 "are written to <output directory>/<netlist name>.<analysis>.csv\n"
                 "(default output directory: current directory).\n"
                 "--summary writes a machine-readable description of all results\n"
                 "(files, operating point values, measurements) for use by other programs.\n"
                 "-j <n> computes n runs of a parameter study at the same time (default: all cores).\n"
                 "--progress writes lines \"progress <analysis> <fraction>\" to stderr while a\n"
                 "transient or a series of runs is computed (at most one per percent).\n";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Texts are UTF-8 (the manifest makes arguments and file names UTF-8 as well).
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string input, summaryPath;
    esim::RunConfig cfg;
    nlohmann::json summary = nlohmann::json::array();

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
        } else if (arg == "--summary") {
            if (i + 1 >= argc) {
                std::cerr << "error: --summary needs a file name\n";
                return 2;
            }
            summaryPath = argv[++i];
        } else if (arg == "-j" || arg == "--threads") {
            if (i + 1 >= argc || std::atoi(argv[i + 1]) < 1) {
                std::cerr << "error: " << arg << " needs a number >= 1\n";
                return 2;
            }
            cfg.threads = std::atoi(argv[++i]);
        } else if (arg == "--progress") {
            cfg.progress = [last = std::string(), percent = -1](const std::string& type,
                                                                double fraction) mutable {
                const int now = static_cast<int>(fraction * 100.0);
                if (type == last && now == percent) return;
                last = type;
                percent = now;
                std::cerr << "progress " << type << ' ' << fraction << std::endl;
            };
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

        if (!summaryPath.empty()) cfg.summary = &summary;
        esim::runAnalyses(nl, cfg, std::cout);
        if (!summaryPath.empty()) {
            std::ofstream out(summaryPath);
            if (!out) throw std::runtime_error("cannot write '" + summaryPath + "'");
            out << nlohmann::json({{"title", nl.title}, {"analyses", summary}}).dump(2) << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
