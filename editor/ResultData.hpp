#pragma once

#include <QColor>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

// Results of one analysis as the plot shows them: curves sorted into panels
// that share the x axis. Built from the CSV files of the simulator with the
// rules of tools/plot.py.

struct Curve {
    QString name;          // what it shows, e.g. "v(out)"
    QString group;         // run of a parameter sweep, e.g. "R = 2000"; empty otherwise
    QVector<double> x, y;  // x ascending within a curve (time, frequency, swept value)
    // Colour: categorical slot, or for the runs of a sweep run k of n on a
    // light-to-dark scale (see Theme). Resolved when drawn, so that it follows
    // the appearance of the system.
    int slot = 0;
    int run = -1, runs = 0;
    int dash = 0;  // 0 solid, 1 dashed, 2 dotted, 3 dash-dot: tells quantities apart
                   // where the colour stands for something else (runs of a sweep)
    int markers = 0;  // 0 a line, 1 points only (Monte Carlo), 2 a line through points (sweep values)
    QColor color() const;
};

struct Panel {
    QString title;  // axis label, e.g. "Spannung [V]"
    QString unit;   // "V", "A", "dB", "°" or empty
    QVector<Curve> curves;
};

struct PlotData {
    QString xLabel;
    QString xUnit;
    bool logX = false;
    QVector<Panel> panels;
};

// A CSV file of the simulator: header and columns.
struct CsvTable {
    QStringList header;
    QVector<QVector<double>> columns;
};

// Reads a CSV file; returns false and sets `error` if it cannot be read.
bool readCsv(const QString& path, CsvTable& table, QString& error);

// The names of the quantities in a table, without the x column, the sweep
// parameter and the mag()/phase() wrapping of AC results ("v(out)" once for
// its magnitude and phase).
QStringList quantities(const CsvTable& table);

// Curves of the selected quantities (all if `selected` is empty).
PlotData plotData(const CsvTable& table, const QStringList& selected);

// --- measurements over a sweep or Monte Carlo ---------------------------------
// A measurement table has one row per run: the swept or varied parameters
// (Monte Carlo: first the run number), then the measured quantities.

struct MeasureInfo {
    bool monteCarlo = false;
    QStringList params;    // parameter columns, in the order of the table
    QStringList measures;  // measured quantities
};

// statistics: the quantities of the analysis' "statistics" (Monte Carlo),
// sweepParams: its swept parameters; both from the summary of the run.
MeasureInfo measureInfo(const CsvTable& t, const QStringList& statistics, const QStringList& sweepParams);

// The measured quantities (one panel each) over parameter x. Further
// parameters of a nested sweep give a family of curves; Monte Carlo runs
// are points.
PlotData measurePlot(const CsvTable& t, const MeasureInfo& info, const QString& x, const QStringList& selected);

struct Histogram {
    QString name;
    int slot = 0;           // categorical colour, as the quantity in the list
    QVector<double> edges;  // bins: [edges[k], edges[k+1]), the last one closed
    QVector<int> counts;
    int count = 0;          // finite values
    double mean = 0, std = 0, min = 0, max = 0;
};

// Bins after Freedman and Diaconis (5 to 60 bins).
Histogram histogram(const QString& name, const QVector<double>& values);
