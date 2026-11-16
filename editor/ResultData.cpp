#include "ResultData.hpp"

#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QTextStream>
#include <cmath>
#include <limits>

#include "Format.hpp"
#include "Theme.hpp"

QColor Curve::color() const {
    return run >= 0 ? Theme::sequential(run, runs) : Theme::series(slot);
}

bool readCsv(const QString& path, CsvTable& table, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QObject::tr("%1 lässt sich nicht öffnen").arg(path);
        return false;
    }
    QTextStream in(&file);
    table = {};
    if (in.atEnd()) {
        error = QObject::tr("%1 ist leer").arg(path);
        return false;
    }
    table.header = in.readLine().split(',');
    table.columns.resize(table.header.size());
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.isEmpty()) continue;
        const QStringList cells = line.split(',');
        for (int k = 0; k < table.columns.size(); ++k) {
            bool ok = false;
            const double v = k < cells.size() ? cells[k].toDouble(&ok) : 0.0;
            table.columns[k].push_back(ok ? v : std::numeric_limits<double>::quiet_NaN());
        }
    }
    if (table.columns.isEmpty() || table.columns[0].isEmpty()) {
        error = QObject::tr("%1 enthält keine Daten").arg(path);
        return false;
    }
    return true;
}

namespace {

// Parameter sweeps write the parameter in front of time or frequency.
bool isGrouped(const CsvTable& t) {
    return t.header.size() > 2 && (t.header[1] == "time" || t.header[1] == "freq");
}

QString inner(const QString& column, QString* kind = nullptr) {
    static const QRegularExpression wrap(R"(^(mag|phase)\((.*)\)$)");
    const auto m = wrap.match(column);
    if (!m.hasMatch()) return column;
    if (kind) *kind = m.captured(1);
    return m.captured(2);
}

QVector<double> unwrapPhase(QVector<double> p) {
    double offset = 0.0;
    for (int k = 1; k < p.size(); ++k) {
        const double jump = p[k] + offset - p[k - 1];
        if (std::abs(jump) > 180.0) offset -= 360.0 * std::round(jump / 360.0);
        p[k] += offset;
    }
    return p;
}

void panelOf(const QString& name, QString& title, QString& unit) {
    if (name.startsWith("v(")) {
        title = QObject::tr("Spannung");
        unit = "V";
    } else if (name.startsWith("i(") || name.startsWith("-i(")) {
        title = QObject::tr("Strom");
        unit = "A";
    } else {
        title = QObject::tr("Größen");
        unit.clear();
    }
}

}  // namespace

QStringList quantities(const CsvTable& t) {
    QStringList out;
    const int first = isGrouped(t) ? 2 : 1;
    for (int k = first; k < t.header.size(); ++k) {
        const QString q = inner(t.header[k]);
        if (!out.contains(q)) out.push_back(q);
    }
    return out;
}

PlotData plotData(const CsvTable& t, const QStringList& selected) {
    PlotData d;
    const bool grouped = isGrouped(t);
    const int xCol = grouped ? 1 : 0;
    const QString xName = t.header.value(xCol);
    const bool ac = xName == "freq";
    d.logX = ac;
    if (xName == "time") {
        d.xLabel = QObject::tr("Zeit");
        d.xUnit = "s";
    } else if (ac) {
        d.xLabel = QObject::tr("Frequenz");
        d.xUnit = "Hz";
    } else {
        d.xLabel = xName;
        if (xName.startsWith('V') || xName.startsWith('v')) d.xUnit = "V";
        else if (xName.startsWith('I') || xName.startsWith('i')) d.xUnit = "A";
    }

    // Rows of each run of a sweep, in the order of the file.
    QVector<QPair<QString, QVector<int>>> groups;
    const int rows = t.columns[0].size();
    if (grouped) {
        QMap<double, int> index;
        for (int r = 0; r < rows; ++r) {
            const double v = t.columns[0][r];
            auto it = index.find(v);
            if (it == index.end()) {
                it = index.insert(v, groups.size());
                groups.push_back({QString("%1 = %2").arg(t.header[0], formatSI(v, QString(), 4)), {}});
            }
            groups[*it].second.push_back(r);
        }
    } else {
        QVector<int> all(rows);
        for (int r = 0; r < rows; ++r) all[r] = r;
        groups.push_back({QString(), all});
    }

    const QStringList names = quantities(t);
    QMap<QString, int> panelIndex;
    for (int c = xCol + 1; c < t.header.size(); ++c) {
        QString kind;
        const QString name = inner(t.header[c], &kind);
        if (!selected.isEmpty() && !selected.contains(name)) continue;
        QString title, unit;
        if (kind == "mag") {
            title = QObject::tr("Betrag");
            unit = "dB";
        } else if (kind == "phase") {
            title = QObject::tr("Phase");
            unit = "°";
        } else {
            panelOf(name, title, unit);
        }
        auto it = panelIndex.find(title);
        if (it == panelIndex.end()) {
            it = panelIndex.insert(title, d.panels.size());
            d.panels.push_back({title, unit, {}});
        }
        Panel& panel = d.panels[*it];
        for (int g = 0; g < groups.size(); ++g) {
            Curve curve;
            curve.name = name;
            curve.group = groups[g].first;
            for (int r : groups[g].second) {
                curve.x.push_back(t.columns[xCol][r]);
                double y = t.columns[c][r];
                if (kind == "mag") y = y > 0.0 ? 20.0 * std::log10(y) : std::numeric_limits<double>::quiet_NaN();
                curve.y.push_back(y);
            }
            if (kind == "phase") curve.y = unwrapPhase(curve.y);
            // Colour follows the quantity (its place among all quantities of
            // the file), or for a sweep the run on a light-to-dark scale.
            const int q = names.indexOf(name);
            if (grouped && groups.size() > 1) {
                curve.run = g;
                curve.runs = groups.size();
                curve.dash = q % 4;
            } else {
                // eight colours; beyond them the line style changes, colours never repeat alone
                curve.slot = q % Theme::kSeries;
                curve.dash = (q / Theme::kSeries) % 4;
            }
            panel.curves.push_back(curve);
        }
    }
    return d;
}

// --- measurements ---------------------------------------------------------------

MeasureInfo measureInfo(const CsvTable& t, const QStringList& statistics, const QStringList& sweepParams) {
    MeasureInfo info;
    info.monteCarlo = !t.header.isEmpty() && t.header[0] == "run";
    for (const QString& h : t.header) {
        if (h == "run" && info.monteCarlo) continue;
        // Monte Carlo: the statistics name the measurements; a sweep: its parameters are known
        const bool measure = !statistics.isEmpty() ? statistics.contains(h) : !sweepParams.contains(h);
        (measure ? info.measures : info.params) << h;
    }
    return info;
}

PlotData measurePlot(const CsvTable& t, const MeasureInfo& info, const QString& x, const QStringList& selected) {
    PlotData d;
    const int xCol = int(t.header.indexOf(x));
    if (xCol < 0) return d;
    d.xLabel = x;
    const QVector<double>& xs = t.columns[xCol];
    double lo = std::numeric_limits<double>::infinity(), hi = 0;
    bool positive = true;
    for (double v : xs) {
        if (!std::isfinite(v)) continue;
        positive = positive && v > 0;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    d.logX = positive && lo > 0 && hi / lo >= 20.0;  // values over decades

    // runs grouped by the other parameters (a nested sweep); Monte Carlo: one cloud
    QStringList others;
    if (!info.monteCarlo)
        for (const QString& p : info.params)
            if (p != x) others << p;
    QVector<QPair<QString, QVector<int>>> groups;
    QMap<QString, int> index;
    const int rows = int(xs.size());
    for (int r = 0; r < rows; ++r) {
        QStringList key;
        for (const QString& o : others)
            key << QStringLiteral("%1 = %2").arg(o, formatSI(t.columns[t.header.indexOf(o)][r], QString(), 4));
        const QString label = key.join(", ");
        auto it = index.find(label);
        if (it == index.end()) {
            it = index.insert(label, int(groups.size()));
            groups.push_back({label, {}});
        }
        groups[*it].second.push_back(r);
    }
    for (auto& g : groups)
        std::stable_sort(g.second.begin(), g.second.end(), [&](int a, int b) { return xs[a] < xs[b]; });

    int q = 0;
    for (const QString& m : info.measures) {
        if (!selected.isEmpty() && !selected.contains(m)) continue;
        const int col = int(t.header.indexOf(m));
        Panel panel{m, QString(), {}};
        for (int g = 0; g < groups.size(); ++g) {
            Curve c;
            c.name = m;
            c.group = groups[g].first;
            for (int r : groups[g].second) {
                c.x.push_back(xs[r]);
                c.y.push_back(t.columns[col][r]);
            }
            c.markers = info.monteCarlo ? 1 : 2;
            if (groups.size() > 1) {
                c.run = g;
                c.runs = int(groups.size());
            } else {
                c.slot = q % Theme::kSeries;
            }
            panel.curves.push_back(c);
        }
        d.panels.push_back(panel);
        ++q;
    }
    return d;
}

Histogram histogram(const QString& name, const QVector<double>& values) {
    Histogram h;
    h.name = name;
    QVector<double> v;
    for (double x : values)
        if (std::isfinite(x)) v.push_back(x);
    h.count = int(v.size());
    if (v.isEmpty()) return h;
    std::sort(v.begin(), v.end());
    h.min = v.front();
    h.max = v.back();
    double sum = 0, sq = 0;
    for (double x : v) sum += x;
    h.mean = sum / v.size();
    for (double x : v) sq += (x - h.mean) * (x - h.mean);
    h.std = v.size() > 1 ? std::sqrt(sq / (v.size() - 1)) : 0.0;
    const auto quantile = [&](double p) {
        const double pos = p * (v.size() - 1);
        const int i = int(pos);
        return i + 1 < v.size() ? v[i] + (pos - i) * (v[i + 1] - v[i]) : v[i];
    };
    const double range = h.max - h.min;
    int bins = 1;
    if (range > 0) {
        const double iqr = quantile(0.75) - quantile(0.25);
        const double width = 2.0 * iqr * std::pow(double(v.size()), -1.0 / 3.0);
        bins = width > 0 ? int(std::ceil(range / width)) : int(std::ceil(std::sqrt(double(v.size()))));
        bins = std::clamp(bins, 5, 60);
    }
    const double lo = range > 0 ? h.min : h.min - 0.5, w = range > 0 ? range / bins : 1.0;
    for (int k = 0; k <= bins; ++k) h.edges.push_back(lo + k * w);
    h.counts.fill(0, bins);
    for (double x : v) h.counts[std::clamp(int((x - lo) / w), 0, bins - 1)]++;
    return h;
}
