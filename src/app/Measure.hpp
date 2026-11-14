#pragma once

#include <complex>
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
//   thd harmonic        distortion and amplitude of harmonic "n" for the fundamental "freq",
//                       from the last whole periods inside the window
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
    double freq = std::numeric_limits<double>::quiet_NaN();  // fundamental for thd, harmonic
};

// Fourier analysis of a sampled curve, taken as piecewise linear: the complex
// amplitudes A_0 .. A_n of  y(t) = A_0 + sum_k |A_k| cos(2 pi k f t + arg A_k)
// over the last whole periods that fit between `from` and the end (at most
// `periods`; 0: as many as fit). Returns false if not even one period fits.
bool fourier(const std::vector<double>& t, const std::vector<double>& y, double freq, int n,
             int periods, double from, std::vector<std::complex<double>>& amplitudes);

// Total harmonic distortion sqrt(sum_{k>=2} |A_k|^2) / |A_1|.
double totalHarmonicDistortion(const std::vector<std::complex<double>>& amplitudes);

std::vector<MeasureSpec> parseMeasures(const nlohmann::json& j,
                                       const std::map<std::string, double>& parameters);

double evaluateMeasure(const MeasureSpec& spec, const std::vector<double>& x,
                       const std::vector<double>& y);

}  // namespace esim
