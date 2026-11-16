#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTemporaryDir>
#include <memory>

#include "Checker.hpp"
#include "Schematic.hpp"

class CodeEditor;
class PartPalette;
class PropertyPanel;
class QAction;
class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QSplitter;
class QTimer;
class ResultsPanel;
class SchematicView;
class SimulationRun;

// One input file, shown twice side by side: as schematic and as text. The
// text is the reference; the schematic reads it after every change and
// writes its own changes into it as small edits, so both share one undo
// history. Parts and properties are docked on the left; results, log and
// problems below. Simulation runs save the file first (as development
// environments do) and keep their results in a temporary directory.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    bool openFile(const QString& path);
    void showSubcircuit(const QString& name) { enterScope(name); }
    // For tests and documentation: start the simulation right away (run), and
    // when it is done (or the check, without run) save the window as image
    // and quit. exitCode: 0 if the simulation succeeded.
    void automate(bool run, const QString& snapshot);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void createActions();
    void createDocks();
    void newFile();
    void openDialog();
    bool save();
    bool saveAs();
    bool saveTo(const QString& path);
    bool maybeSave();
    void setCurrentFile(const QString& path);
    void updateRecentMenu();
    void fillExamplesMenu();
    void loaded();  // after new or open

    void scheduleCheck();
    void showCheck(const CheckResult& r);
    void updateProblems();
    void runSimulation();
    void stopSimulation();
    void appendLog(const QString& text, int kind);
    void simulationFinished(bool ok, bool stopped);

    // schematic <-> text
    void refreshSchematic();
    void applyText(const QString& text);
    void applySchematic(const Schematic& after);
    void schematicSelectionChanged(const QStringList& names);
    void textCursorMoved();
    void showProperties();
    void revealInText(int index, bool focus);
    void choosePart(const QString& id);
    void renameComponent(int index, const QString& name);
    void probe(const QString& quantity);
    void enterScope(const QString& scope);
    void newSubcircuit();
    void updateOperatingPoint();
    // In a subcircuit: the name of a quantity in the simulation for one of its
    // instances ("v(out)" -> "v(U1.out)", a port -> the node outside).
    QString instanceQuantity(const QString& quantity, const Instance& in) const;

    CodeEditor* editor_;
    SchematicView* schematic_;
    QSplitter* split_;
    QWidget* planBox_;
    QWidget* scopeBar_;
    QLabel* scopeLabel_;
    PartPalette* palette_;
    PropertyPanel* props_;
    ResultsPanel* results_;
    QPlainTextEdit* log_;
    QListWidget* problems_;
    QLabel* status_;
    QProgressBar* progress_;
    QMenu* recentMenu_;
    QMenu* examplesMenu_;
    QAction* runAction_;
    QAction* stopAction_;
    QAction* showPlan_;
    QAction* showText_;
    QDockWidget* paletteDock_;
    QDockWidget* propsDock_;
    QDockWidget* resultsDock_;
    QDockWidget* logDock_;
    QDockWidget* problemsDock_;

    Checker checker_;
    CheckResult lastCheck_;
    QVector<Diagnostic> schematicDiagnostics_;
    QTimer* checkTimer_;
    QTimer* schematicTimer_;
    bool writing_ = false;         // the schematic writes into the text
    bool syncingCursor_ = false;   // the text cursor is moved to a component
    QString scope_;                // the subcircuit shown, "" the main circuit
    QHash<QString, double> op_;    // operating point of the last run
    SimulationRun* run_;
    std::unique_ptr<QTemporaryDir> outDir_;
    QString file_;
    QElapsedTimer clock_;
    QString snapshot_;  // see automate()
    void finishAutomation(bool ok);
};
