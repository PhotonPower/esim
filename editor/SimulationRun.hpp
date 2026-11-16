#pragma once

#include <QObject>
#include <QProcess>

// One run of the simulator as a separate process: esim <file> -o <dir>
// --summary <dir>/summary.json --progress. A crash or an endless run of the
// simulator cannot take the editor with it, and stopping is ending the process.
class SimulationRun : public QObject {
    Q_OBJECT
public:
    enum class LineKind { Output, Warning, Error };

    explicit SimulationRun(QObject* parent = nullptr);
    ~SimulationRun() override;

    // The esim program: next to the editor, ESIM_BIN, or on the PATH. Empty if none.
    static QString program();

    bool start(const QString& netlist, const QString& outDir);
    void stop();
    bool running() const { return process_.state() != QProcess::NotRunning; }
    QString summaryPath() const { return summary_; }

signals:
    void line(const QString& text, SimulationRun::LineKind kind);
    void progress(const QString& analysis, double fraction);
    void finished(bool ok, bool stopped);

private:
    void readOutput();
    void readErrors();

    QProcess process_;
    QString summary_;
    QByteArray outBuffer_, errBuffer_;
    bool stopped_ = false;
};
