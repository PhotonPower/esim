#pragma once

#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace esim {

// A characteristic number computed from a result curve y(x), where x is the
// time, the frequency or the swept source value of the analysis.
//
//   "measure": {
//     "vavg":   { "avg": "v(out)", "from": "5m" },
//     "period": { "period": "v(c)" },
//     "t90":    { "cross": "v(out)", "level": 4.5 }
//   }
//
// Functions (the key that holds the output expression):
//   min max pp          extreme values, peak-to-peak
//   argmin argmax       x at which the extreme value occurs
//   avg rms integ       mean, root mean square, integral over x
//   final               last value
//   at                  value at x = "x" (interpolated)
//   cross               x of the "n"-th crossing of "level" ("edge": rise, fall, both)
//   period freq duty    from the rising crossings of "level" (default: middle of the range)
//   rise fall           transition time between "low" and "high" (default: 10 % and 90 %)
//   f3db f3db_low       where the curve has dropped to max/sqrt(2) above / below its maximum
//
// "from" and "to" restrict the evaluation to a window of x. A measurement that
// is not defined (no crossing found, ...) yields NaN.
struct MeasureSpec {
    std::string name;
    std::string function;
    std::string expression;
    double from = -std::numeric_limits<double>::infinity();
    double to = std::numeric_limits<double>::infinity();
    double level = std::numeric_limits<double>::quiet_NaN();
    double low = std::numeric_limits<double>::quiet_NaN();
    double high = std::numeric_limits<double>::quiet_NaN();
    double x = std::numeric_limits<double>::quiet_NaN();
    std::string edge = "rise";
    int n = 1;
};

std::vector<MeasureSpec> parseMeasures(const nlohmann::json& j,
                                       const std::map<std::string, double>& parameters);

double evaluateMeasure(const MeasureSpec& spec, const std::vector<double>& x,
                       const std::vector<double>& y);

}  // namespace esim
