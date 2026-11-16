#include "SimulationRun.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

SimulationRun::SimulationRun(QObject* parent) : QObject(parent) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &SimulationRun::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &SimulationRun::readErrors);
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput();
        readErrors();
        // a last line without line break
        if (!outBuffer_.isEmpty()) emit line(QString::fromUtf8(outBuffer_), LineKind::Output);
        if (!errBuffer_.isEmpty()) emit line(QString::fromUtf8(errBuffer_), LineKind::Error);
        outBuffer_.clear();
        errBuffer_.clear();
        if (status == QProcess::CrashExit && !stopped_)
            emit line(tr("Der Simulator ist abgestürzt."), LineKind::Error);
        emit finished(status == QProcess::NormalExit && code == 0, stopped_);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit line(tr("Der Simulator ließ sich nicht starten: %1").arg(process_.errorString()), LineKind::Error);
            emit finished(false, false);
        }
    });
}

SimulationRun::~SimulationRun() {
    if (running()) {
        process_.kill();
        process_.waitForFinished(2000);
    }
}

QString SimulationRun::program() {
#ifdef Q_OS_WIN
    const QString name = "esim.exe";
#else
    const QString name = "esim";
#endif
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    if (QFileInfo(beside).isExecutable()) return beside;
    const QString env = qEnvironmentVariable("ESIM_BIN");
    if (!env.isEmpty() && QFileInfo(env).isExecutable()) return env;
    return QStandardPaths::findExecutable("esim");
}

bool SimulationRun::start(const QString& netlist, const QString& outDir) {
    const QString exe = program();
    if (exe.isEmpty()) {
        emit line(tr("Das Programm esim wurde nicht gefunden (neben dem Editor, über ESIM_BIN oder im PATH)."),
                  LineKind::Error);
        return false;
    }
    stopped_ = false;
    summary_ = QDir(outDir).filePath("summary.json");
    QFile::remove(summary_);
    process_.setWorkingDirectory(QFileInfo(netlist).absolutePath());
    process_.start(exe, {netlist, "-o", outDir, "--summary", summary_, "--progress"});
    return true;
}

void SimulationRun::stop() {
    if (!running()) return;
    stopped_ = true;
    process_.kill();
}

void SimulationRun::readOutput() {
    outBuffer_ += process_.readAllStandardOutput();
    for (int nl; (nl = outBuffer_.indexOf('\n')) >= 0;) {
        emit line(QString::fromUtf8(outBuffer_.left(nl)), LineKind::Output);
        outBuffer_.remove(0, nl + 1);
    }
}

void SimulationRun::readErrors() {
    errBuffer_ += process_.readAllStandardError();
    for (int nl; (nl = errBuffer_.indexOf('\n')) >= 0;) {
        const QString text = QString::fromUtf8(errBuffer_.left(nl));
        errBuffer_.remove(0, nl + 1);
        if (text.startsWith("progress ")) {
            const QStringList parts = text.split(' ');
            if (parts.size() == 3) emit progress(parts[1], parts[2].toDouble());
            continue;
        }
        emit line(text, text.startsWith("warning:") ? LineKind::Warning : LineKind::Error);
    }
}
