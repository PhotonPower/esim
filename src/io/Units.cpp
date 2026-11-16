#include "io/Units.hpp"
#include "core/ParseNumber.hpp"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace esim {

double parseValue(const std::string& text) {
    const auto bad = [&]() { return std::invalid_argument("cannot parse number '" + text + "'"); };

    // Must start like a number: strtod alone would also accept "inf", "nan", hex...
    size_t i = text.find_first_not_of(" \t");
    if (i == std::string::npos) throw bad();
    if (text[i] == '+' || text[i] == '-') ++i;
    if (i >= text.size() || !(std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.'))
        throw bad();

    const char* begin = text.c_str();
    char* end = nullptr;
    const double v = parseDouble(begin, &end);
    if (end == begin) throw bad();

    std::string suffix(end);
    suffix.erase(0, suffix.find_first_not_of(" \t"));
    suffix.erase(suffix.find_last_not_of(" \t") + 1);
    if (suffix.empty()) return v;

    auto startsWith = [&](const char* s) { return suffix.rfind(s, 0) == 0; };
    double factor = 1.0;
    size_t prefixLength = 1;
    if (startsWith("meg") || startsWith("Meg") || startsWith("MEG")) {
        factor = 1e6;
        prefixLength = 3;
    } else if (startsWith("\xC2\xB5") || startsWith("\xCE\xBC")) {  // micro sign, mu
        factor = 1e-6;
        prefixLength = 2;
    } else {
        switch (suffix[0]) {
            case 'T': factor = 1e12; break;
            case 'G': factor = 1e9; break;
            case 'M': factor = 1e6; break;
            case 'k':
            case 'K': factor = 1e3; break;
            case 'm': factor = 1e-3; break;
            case 'u': factor = 1e-6; break;
            case 'n': factor = 1e-9; break;
            case 'p': factor = 1e-12; break;
            case 'f': factor = 1e-15; break;
            default: prefixLength = 0;  // plain unit such as "V", "Hz", "F"
        }
    }
    // The rest is a unit: letters only. Anything else ("2*R") is not a number.
    for (size_t k = prefixLength; k < suffix.size(); ++k) {
        const auto c = static_cast<unsigned char>(suffix[k]);
        if (!std::isalpha(c) && c < 0x80) throw bad();
    }
    return v * factor;
}

}  // namespace esim
