#pragma once

#include <vector>

#include "core/DaeSystem.hpp"

namespace esim {

// Structural analysis of a DAE  d/dt q(x) + f(x, t) = 0  with the algorithm
// of Pantelides. It looks only at which unknowns occur in which equation (and
// which of them under the time derivative) and finds out how often each
// equation would have to be differentiated before the system determines the
// highest derivatives of all unknowns.
//
// index: 0 for an ordinary differential equation, 1 if there are algebraic
// equations that determine their unknowns directly, and 1 + the largest number
// of differentiations otherwise. This is the structural index; it agrees with
// the differentiation index for most systems written down by hand, but exact
// cancellations can make the true index higher.
struct IndexAnalysis {
    bool analyzed = false;               // false: the system was too large, nothing was done
    bool singular = false;               // no assignment of equations to unknowns exists
    int index = 0;
    std::vector<int> differentiations;   // per equation
};

// occurs[i]: the unknowns in f_i; differentiated[i]: the unknowns in q_i.
IndexAnalysis structuralIndex(const std::vector<std::vector<int>>& occurs,
                              const std::vector<std::vector<int>>& differentiated);

// The same for a system, with the occurrences taken from its Jacobians at a
// point near x (entries that are exactly zero there do not count).
IndexAnalysis structuralIndex(DaeSystem& sys, const Vector& x, double t = 0.0);

}  // namespace esim
