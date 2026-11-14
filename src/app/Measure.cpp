#include "app/Measure.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "io/Netlist.hpp"

namespace esim {

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

const std::set<std::string>& functions() {
    static const std::set<std::string> f = {"min",    "max",  "pp",     "argmin", "argmax", "avg",
                                            "rms",    "integ", "final",  "at",     "cross",  "period",
                                            "freq",   "duty", "rise",   "fall",   "f3db",   "f3db_low"};
    return f;
}

// The part of the curve inside [from, to], with interpolated end points.
void window(const std::vector<double>& x, const std::vector<double>& y, double from, double to,
            std::vector<double>& xs, std::vector<double>& ys) {
    xs.clear();
    ys.clear();
    auto interpolate = [&](size_t k, double at) {  // between k - 1 and k
        const double s = (at - x[k - 1]) / (x[k] - x[k - 1]);
        return y[k - 1] + s * (y[k] - y[k - 1]);
    };
    for (size_t k = 0; k < x.size(); ++k) {
        if (k > 0 && x[k] > x[k - 1]) {
            if (x[k - 1] < from && x[k] > from) {
                xs.push_back(from);
                ys.push_back(interpolate(k, from));
            }
            if (x[k - 1] < to && x[k] > to) {
                xs.push_back(to);
                ys.push_back(interpolate(k, to));
            }
        }
        if (x[k] >= from && x[k] <= to) {
            xs.push_back(x[k]);
            ys.push_back(y[k]);
        }
    }
}

// x positions where the curve crosses a level (edge: +1 rising, -1 falling, 0 both).
std::vector<double> crossings(const std::vector<double>& x, const std::vector<double>& y,
                              double level, int edge) {
    std::vector<double> out;
    for (size_t k = 1; k < x.size(); ++k) {
        const double a = y[k - 1] - level, b = y[k] - level;
        const bool rising = a < 0.0 && b >= 0.0;
        const bool falling = a > 0.0 && b <= 0.0;
        if (!((rising && edge >= 0) || (falling && edge <= 0))) continue;
        out.push_back(x[k - 1] + (x[k] - x[k - 1]) * a / (a - b));
    }
    return out;
}

double integral(const std::vector<double>& x, const std::vector<double>& y, bool squared) {
    double sum = 0.0;
    for (size_t k = 1; k < x.size(); ++k) {
        const double h = x[k] - x[k - 1], a = y[k - 1], b = y[k];
        // exact for the piecewise linear curve
        sum += squared ? h * (a * a + a * b + b * b) / 3.0 : 0.5 * h * (a + b);
    }
    return sum;
}

}  // namespace

std::vector<MeasureSpec> parseMeasures(const nlohmann::json& j,
                                       const std::map<std::string, double>& parameters) {
    if (!j.is_object())
        throw std::runtime_error("measure: expected an object {\"name\": {\"avg\": \"v(out)\"}, ...}");
    std::vector<MeasureSpec> out;
    for (const auto& [name, m] : j.items()) {
        const std::string where = "measure \"" + name + "\"";
        if (!m.is_object()) throw std::runtime_error(where + ": expected an object");
        MeasureSpec spec;
        spec.name = name;
        for (const auto& item : m.items()) {
            const std::string& key = item.key();
            const nlohmann::json& value = item.value();
            auto number = [&]() { return jsonNumber(value, where + ", \"" + key + "\"", &parameters); };
            if (functions().count(key)) {
                if (!spec.function.empty())
                    throw std::runtime_error(where + ": more than one function (\"" +
                                             spec.function + "\" and \"" + key + "\")");
                if (!value.is_string())
                    throw std::runtime_error(where + ": \"" + key +
                                             "\" needs an output expression like \"v(out)\"");
                spec.function = key;
                spec.expression = value.get<std::string>();
            } else if (key == "from") spec.from = number();
            else if (key == "to") spec.to = number();
            else if (key == "level") spec.level = number();
            else if (key == "low") spec.low = number();
            else if (key == "high") spec.high = number();
            else if (key == "x") spec.x = number();
            else if (key == "n") spec.n = static_cast<int>(number());
            else if (key == "edge") {
                if (!value.is_string() ||
                    (value != "rise" && value != "fall" && value != "both"))
                    throw std::runtime_error(where + ": \"edge\" must be \"rise\", \"fall\" or \"both\"");
                spec.edge = value.get<std::string>();
            } else {
                throw std::runtime_error(where + ": unknown key \"" + key + "\"");
            }
        }
        if (spec.function.empty()) {
            std::string list;
            for (const std::string& f : functions()) list += (list.empty() ? "" : ", ") + f;
            throw std::runtime_error(where + ": no function given (" + list + ")");
        }
        if (spec.function == "cross" && std::isnan(spec.level))
            throw std::runtime_error(where + ": \"cross\" needs a \"level\"");
        if (spec.function == "at" && std::isnan(spec.x))
            throw std::runtime_error(where + ": \"at\" needs the position \"x\"");
        if (spec.n < 1) throw std::runtime_error(where + ": \"n\" must be at least 1");
        out.push_back(spec);
    }
    return out;
}

double evaluateMeasure(const MeasureSpec& spec, const std::vector<double>& xAll,
                       const std::vector<double>& yAll) {
    std::vector<double> x, y;
    window(xAll, yAll, spec.from, spec.to, x, y);
    if (x.empty()) return kNaN;

    const auto [itMin, itMax] = std::minmax_element(y.begin(), y.end());
    const double ymin = *itMin, ymax = *itMax;
    const double span = x.back() - x.front();
    const std::string& f = spec.function;

    if (f == "min") return ymin;
    if (f == "max") return ymax;
    if (f == "pp") return ymax - ymin;
    if (f == "argmin") return x[itMin - y.begin()];
    if (f == "argmax") return x[itMax - y.begin()];
    if (f == "final") return y.back();
    if (f == "integ") return integral(x, y, false);
    if (f == "avg") return span > 0.0 ? integral(x, y, false) / span : y.front();
    if (f == "rms") return span > 0.0 ? std::sqrt(integral(x, y, true) / span) : std::abs(y.front());

    if (f == "at") {
        if (spec.x < x.front() || spec.x > x.back()) return kNaN;
        const size_t k = std::upper_bound(x.begin(), x.end(), spec.x) - x.begin();
        if (k == 0) return y.front();
        if (k >= x.size()) return y.back();
        const double s = (spec.x - x[k - 1]) / (x[k] - x[k - 1]);
        return y[k - 1] + s * (y[k] - y[k - 1]);
    }

    if (f == "cross") {
        const int edge = spec.edge == "rise" ? 1 : spec.edge == "fall" ? -1 : 0;
        const std::vector<double> c = crossings(x, y, spec.level, edge);
        return static_cast<size_t>(spec.n) <= c.size() ? c[spec.n - 1] : kNaN;
    }

    if (f == "period" || f == "freq" || f == "duty") {
        const double level = std::isnan(spec.level) ? 0.5 * (ymin + ymax) : spec.level;
        const std::vector<double> up = crossings(x, y, level, 1);
        if (up.size() < 2) return kNaN;
        const double period = (up.back() - up.front()) / static_cast<double>(up.size() - 1);
        if (f == "period") return period;
        if (f == "freq") return 1.0 / period;
        // Share of the time above the level, over whole periods.
        std::vector<double> xs, ys;
        window(x, y, up.front(), up.back(), xs, ys);
        const std::vector<double> down = crossings(xs, ys, level, -1);
        double above = 0.0;
        size_t d = 0;
        for (size_t k = 0; k + 1 < up.size(); ++k) {
            while (d < down.size() && down[d] <= up[k]) ++d;
            if (d < down.size() && down[d] < up[k + 1]) above += down[d] - up[k];
            else above += up[k + 1] - up[k];
        }
        return above / (up.back() - up.front());
    }

    if (f == "rise" || f == "fall") {
        const double low = std::isnan(spec.low) ? ymin + 0.1 * (ymax - ymin) : spec.low;
        const double high = std::isnan(spec.high) ? ymin + 0.9 * (ymax - ymin) : spec.high;
        const bool rise = f == "rise";
        // First crossing of the start level, then the next one of the end level.
        const std::vector<double> start = crossings(x, y, rise ? low : high, rise ? 1 : -1);
        const std::vector<double> end = crossings(x, y, rise ? high : low, rise ? 1 : -1);
        if (start.empty()) return kNaN;
        for (double e : end)
            if (e >= start.front()) return e - start.front();
        return kNaN;
    }

    if (f == "f3db" || f == "f3db_low") {
        const double level = ymax / std::sqrt(2.0);
        const double xPeak = x[itMax - y.begin()];
        if (f == "f3db") {
            for (double c : crossings(x, y, level, -1))
                if (c >= xPeak) return c;
            return kNaN;
        }
        double last = kNaN;
        for (double c : crossings(x, y, level, 1))
            if (c <= xPeak) last = c;
        return last;
    }
    return kNaN;
}

}  // namespace esim
