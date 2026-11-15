#pragma once

#include <string>
#include <vector>

#include "circuit/Circuit.hpp"
#include "circuit/devices/Equations.hpp"

namespace esim {

// The user equations of a netlist with their unknowns.
struct EquationSystem {
    std::vector<VariableSpec> variables;
    std::vector<EquationSpec> equations;
};

// Automatic index reduction for user equations.
//
// The integration methods handle index 1 and 2. If the structural analysis of
// the circuit `ckt` (built from `in`) finds a higher index, every algebraic
// equation g = 0 that would have to be differentiated k >= 2 times is
// supplemented by its time derivatives g' = 0, ..., g^(k-1) = 0. They are
// formed symbolically, with der(x) replaced by the right-hand sides of the
// equations "c * der(x) = ...".
//
// Imposing the derivatives in addition to g makes the system overdetermined.
// It is made square again the way Gear, Gupta and Leimkuhler stabilize
// constrained mechanical systems: every constraint below the highest
// derivative gets a multiplier mu, a new unknown, which enters the equations
// of the unknowns x in that constraint as
//
//     der(x) = (right-hand side) - dg/dx * mu .
//
// mu is zero for the exact solution; it removes the part of the integration
// error that would lead off the constraint. The result has index 2, and g and
// all its derivatives hold to the accuracy of the Newton iteration (no drift).
//
// Returns true with the new system in `out` if something was changed. `notes`
// says what was done, or why a reduction was not possible (the system is then
// left as it is and the transient analysis warns about its index).
bool reduceIndex(Circuit& ckt, const EquationSystem& in, EquationSystem& out,
                 std::vector<std::string>& notes);

}  // namespace esim
