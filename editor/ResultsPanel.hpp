#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

#include "ResultData.hpp"

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QTableWidget;
class PlotWidget;

// Results of the last run: the analyses of the summary file on the left,
// the quantities of the chosen one to tick, its scalar values (operating
// point, measurements, statistics) in a table, and the curves in a plot.
// After a new run the chosen analysis and quantities stay chosen.
class ResultsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ResultsPanel(QWidget* parent = nullptr);

    // Reads the summary file of the simulator; false if it cannot be read.
    bool load(const QString& summaryPath, QString& error);
    void clear();
    PlotWidget* plot() const { return plot_; }
    bool empty() const { return entries_.isEmpty(); }
    // Ticks a quantity ("v(out)", "i(R1)") in the shown analysis, or in the
    // first other analysis that has it; false if none has.
    bool probe(const QString& quantity);
    // Values of the operating point analyses ("v(out)" -> 2.5).
    QHash<QString, double> operatingPoint() const;

private:
    void showAnalysis(int row);
    void updatePlot();
    void fillValues(const QJsonObject& entry);

    QListWidget* analyses_;
    QLineEdit* filter_;
    QListWidget* quantities_;
    QTableWidget* values_;
    QStackedWidget* stack_;
    PlotWidget* plot_;
    QTableWidget* bigValues_;  // instead of the plot, for analyses without curves

    QJsonArray entries_;
    CsvTable table_;
    QString currentKey_;                       // "type/name" of the shown analysis
    QHash<QString, QStringList> checked_;      // ticked quantities per analysis
    bool filling_ = false;
};
