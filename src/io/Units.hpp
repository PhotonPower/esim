#pragma once

#include <string>

namespace esim {

// Parse a number with an optional SI prefix and unit, e.g. "4.7k", "100nF",
// "1e-3", "2.2 MOhm". Prefixes are case sensitive:
//   f p n u(µ) m  k(K) M(meg) G T
// Letters after the prefix are treated as a unit and ignored.
// Throws std::invalid_argument if the string is not a number of this form
// (for example an expression like "2*R").
double parseValue(const std::string& text);

}  // namespace esim
