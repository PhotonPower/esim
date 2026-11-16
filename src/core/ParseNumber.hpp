#pragma once

#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>

namespace esim {

// std::strtod without the locale: the decimal separator is always '.'.
// strtod follows the C locale of the process, and programs that set it from
// the environment (every Qt application does) would read "4.7" as 4 under a
// German locale. Same contract as strtod: leading white space and a '+' are
// accepted, *end is set behind the number (to s if there is none), values
// beyond the range of double become +-inf, below it 0.
inline double parseDouble(const char* s, char** end) {
    const char* p = s;
    while (std::isspace(static_cast<unsigned char>(*p))) ++p;
    const char* q = p;
    if (*q == '+') {
        ++q;
        if (*q == '-' || *q == '+') {  // "+-1" is no number
            if (end) *end = const_cast<char*>(s);
            return 0.0;
        }
    }
    double v = 0.0;
    const auto r = std::from_chars(q, q + std::strlen(q), v);
    if (r.ec == std::errc::invalid_argument) {
        if (end) *end = const_cast<char*>(s);
        return 0.0;
    }
    if (r.ec == std::errc::result_out_of_range) {
        // too large: infinity; too small: zero (sign of the mantissa kept)
        const bool negative = *q == '-';
        bool tiny = false;
        for (const char* c = q; c < r.ptr; ++c)
            if ((*c == 'e' || *c == 'E') && c[1] == '-') tiny = true;
        v = tiny ? 0.0 : std::numeric_limits<double>::infinity();
        if (negative) v = -v;
    }
    if (end) *end = const_cast<char*>(r.ptr);
    return v;
}

}  // namespace esim
