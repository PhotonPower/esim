// Tests of the parts of the editor that work without a window: the check of
// the input (every example must pass without errors, errors must point to
// the right line) and the preparation of curves from CSV files.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>
#include <cstdio>

#include "Checker.hpp"
#include "Format.hpp"
#include "ResultData.hpp"

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const QString& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s\n", qPrintable(what));
    }
}

QString describe(const CheckResult& r) {
    QStringList out;
    for (const Diagnostic& d : r.diagnostics)
        out << QString("%1 line %2: %3").arg(d.severity == Diagnostic::Error ? "error" : "warning").arg(d.line + 1).arg(d.message);
    return out.join("; ");
}

void testExamples(const QString& dir) {
    const QStringList files = QDir(dir).entryList({"*.json"}, QDir::Files, QDir::Name);
    check(files.size() > 10, "examples found in " + dir);
    for (const QString& f : files) {
        QFile file(QDir(dir).filePath(f));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const CheckResult r = checkText(QString::fromUtf8(file.readAll()), dir);
        bool errors = false;
        for (const Diagnostic& d : r.diagnostics) errors = errors || d.severity == Diagnostic::Error;
        check(r.ok && !errors, f + ": " + describe(r));
    }
}

void testLocations() {
    const QString text = R"({
  "components": [
    { "type": "V", "name": "V1", "nodes": ["a", "0"], "dc": 1 },
    { "type": "R", "name": "R1", "nodes": ["a", "0"],
      "value": "1k", "colour": "red" },
    { "type": "D", "name": "D1", "nodes": ["a", "0"], "model": "nope" }
  ]
})";
    CheckResult r = checkText(text, QDir::tempPath());
    check(!r.ok && r.diagnostics.size() >= 1, "undefined model is an error");
    for (const Diagnostic& d : r.diagnostics)
        if (d.severity == Diagnostic::Error) check(d.line == 5, QString("error at the line of D1, got %1").arg(d.line + 1));

    QString fixed = text;
    fixed.replace(R"(, "model": "nope")", R"(, "is": 1e-14)");
    r = checkText(fixed, QDir::tempPath());
    check(r.ok, "valid without the model: " + describe(r));
    bool found = false;
    for (const Diagnostic& d : r.diagnostics)
        if (d.message.contains("colour")) found = d.line == 4;
    check(found, "warning about \"colour\" at its line");

    r = checkText("{\n  \"components\": [\n    { \"type\": \"R\" \n  ]\n}", QDir::tempPath());
    check(!r.ok && !r.diagnostics.isEmpty() && r.diagnostics[0].line == 3,
          "JSON syntax error at its line: " + describe(r));
}

void testCurves() {
    QTemporaryDir dir;
    const QString path = dir.filePath("t.ac.csv");
    {
        QFile f(path);
        check(f.open(QIODevice::WriteOnly), "temporary file");
        QTextStream out(&f);
        out << "freq,mag(v(out)),phase(v(out)),mag(v(in)),phase(v(in))\n";
        // phase passes -180 degrees: it must come out unwrapped
        out << "1,1,-170,1,0\n10,0.1,170,1,0\n100,0.01,150,1,0\n";
    }
    CsvTable t;
    QString error;
    check(readCsv(path, t, error), "csv read: " + error);
    check(quantities(t) == QStringList({"v(out)", "v(in)"}), "quantities of an AC file");
    const PlotData d = plotData(t, {"v(out)"});
    check(d.logX && d.panels.size() == 2, "AC: magnitude and phase panels on a log axis");
    if (d.panels.size() == 2) {
        check(std::abs(d.panels[0].curves[0].y[1] + 20.0) < 1e-12, "magnitude in dB");
        check(std::abs(d.panels[1].curves[0].y[1] + 190.0) < 1e-12, "phase unwrapped");
    }

    const QString sweep = dir.filePath("s.sweep.csv");
    {
        QFile f(sweep);
        check(f.open(QIODevice::WriteOnly), "temporary file");
        QTextStream out(&f);
        out << "R,time,v(out),i(V1)\n1000,0,0,0\n1000,1,1,1\n2000,0,0,0\n2000,1,2,2\n3000,0,0,0\n3000,1,3,3\n";
    }
    check(readCsv(sweep, t, error), "sweep read");
    const PlotData s = plotData(t, {});
    check(s.panels.size() == 2 && s.panels[0].curves.size() == 3, "sweep: one curve per run and quantity");
    if (!s.panels.isEmpty() && s.panels[0].curves.size() == 3)
        check(s.panels[0].curves[2].run == 2 && s.panels[0].curves[2].runs == 3, "runs numbered for the colour scale");
    if (!s.panels.isEmpty() && s.panels[0].curves.size() == 3)
        check(s.panels[0].curves[1].group == "R = 2k", "run named with SI prefix: " + s.panels[0].curves[1].group);
}

void testFormat() {
    check(formatSI(4700, "Ω") == QString::fromUtf8("4.7 kΩ"), "4.7 kΩ: " + formatSI(4700, "Ω"));
    check(formatSI(1.5e-3, "V") == "1.5 mV", "1.5 mV: " + formatSI(1.5e-3, "V"));
    check(formatSI(999.99999, "Hz", 4) == "1 kHz", "rounding carries to the next prefix: " + formatSI(999.99999, "Hz", 4));
    check(formatSI(-20, "dB") == QString::fromUtf8("−20 dB"), "dB without prefix: " + formatSI(-20, "dB"));
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString examples = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(ESIM_SOURCE_DIR "/examples");
    std::printf("[test] examples pass the check\n");
    testExamples(examples);
    std::printf("[test] locations of errors and warnings\n");
    testLocations();
    std::printf("[test] curves from CSV files\n");
    testCurves();
    std::printf("[test] numbers with SI prefixes\n");
    testFormat();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
