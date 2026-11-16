// Terminal charges of a transistor at given terminal voltages, for the
// comparison with the Python reference in tests/test_bsim3.py.
//
//   bsim3_charges input.json
//
// input.json: {"netlist": {...}, "bias": [[vd, vg, vs, vb], ...]}. The netlist
// holds one transistor with the nodes "d", "g", "s", "b" (none of them
// ground; a resistor from one of them to ground gives the circuit its ground
// node without adding charge). Prints one line "qd qg qs qb" per bias point: the charges the
// circuit equations see at these node voltages.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "circuit/Circuit.hpp"
#include "io/Netlist.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bsim3_charges input.json\n";
        return 2;
    }
    try {
        std::ifstream in(argv[1]);
        std::stringstream text;
        text << in.rdbuf();
        const nlohmann::json input = nlohmann::json::parse(text.str());
        esim::Netlist nl = esim::parseNetlistText(input.at("netlist").dump());
        esim::Circuit& ckt = *nl.circuit;
        const int node[4] = {ckt.findNode("d"), ckt.findNode("g"), ckt.findNode("s"), ckt.findNode("b")};
        esim::Evaluation ev;
        for (const auto& bias : input.at("bias")) {
            esim::Vector x = esim::Vector::Zero(ckt.size());
            for (int k = 0; k < 4; ++k) x[node[k]] = bias.at(k).get<double>();
            ckt.evaluate(0.0, x, esim::LimitMode::Off, ev);
            std::printf("%.17g %.17g %.17g %.17g\n", ev.q[node[0]], ev.q[node[1]], ev.q[node[2]], ev.q[node[3]]);
        }
    } catch (const std::exception& e) {
        std::cerr << "bsim3_charges: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
