#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTemporaryDir>
#include <memory>

#include "Checker.hpp"

class CodeEditor;
class QAction;
class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QTimer;
class ResultsPanel;
class SimulationRun;

// One input file: the text in the middle, results, log and problems in
// docks around it. Simulation runs save the file first (as development
// environments do) and keep their results in a temporary directory.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    bool openFile(const QString& path);
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

    void scheduleCheck();
    void showCheck(const CheckResult& r);
    void runSimulation();
    void stopSimulation();
    void appendLog(const QString& text, int kind);
    void simulationFinished(bool ok, bool stopped);

    CodeEditor* editor_;
    ResultsPanel* results_;
    QPlainTextEdit* log_;
    QListWidget* problems_;
    QLabel* status_;
    QProgressBar* progress_;
    QMenu* recentMenu_;
    QMenu* examplesMenu_;
    QAction* runAction_;
    QAction* stopAction_;
    QDockWidget* resultsDock_;
    QDockWidget* logDock_;
    QDockWidget* problemsDock_;

    Checker checker_;
    QTimer* checkTimer_;
    SimulationRun* run_;
    std::unique_ptr<QTemporaryDir> outDir_;
    QString file_;
    QElapsedTimer clock_;
    QString snapshot_;  // see automate()
    void finishAutomation(bool ok);
};
