#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>

#include "CodeEditor.hpp"

// Result of reading the input as the simulator does.
struct CheckResult {
    int generation = 0;
    bool ok = false;               // no errors
    QVector<Diagnostic> diagnostics;
    QString summary;               // e.g. "12 Bauteile, 30 Unbekannte"
    QStringList nodes;             // node names of the circuit (for later use: completion)
};

// Checks the input text in the background with the parser of the simulator
// (library esimcircuit), so that errors appear while typing. Results of
// texts that have changed since are dropped.
class Checker : public QObject {
    Q_OBJECT
public:
    explicit Checker(QObject* parent = nullptr);
    ~Checker() override;

    // baseDir: directory of the file, for data files named in the input.
    void check(const QString& text, const QString& baseDir);

signals:
    void checked(const CheckResult& result);

private:
    QFutureWatcher<CheckResult> watcher_;
    int generation_ = 0;
    bool pending_ = false;
    QString pendingText_, pendingDir_;
};

// The work itself, without threads (also used by tests).
CheckResult checkText(const QString& text, const QString& baseDir);
