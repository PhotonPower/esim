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
#include "JsonSpan.hpp"
#include "ResultData.hpp"
#include "Schematic.hpp"

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

bool parses(const QString& text) {
    JValue v;
    return parseJsonc(text, v);
}

void testJsonSpan() {
    const QString text = "{\n  // comment\n  \"a\": [1, 2, 3],\n  \"o\": { \"x\": \"1k\", \"y\": 2 },\n"
                         "  \"m\": {\n    \"p\": 1,  // why\n    \"q\": 2\n  },\n  \"e\": []\n}\n";
    JValue root;
    QString error;
    check(parseJsonc(text, root, &error), "JSONC parses: " + error);
    const JValue* a = root.get("a");
    const JValue* o = root.get("o");
    const JValue* m = root.get("m");
    const JValue* e = root.get("e");
    check(a && o && m && e, "members found");
    if (!a || !o || !m || !e) return;
    check(text.mid(a->begin, a->end - a->begin) == "[1, 2, 3]", "array span");
    check(o->get("x")->string == "1k", "string value");

    QString t = replaceValue(text, *o->get("x"), "\"2k\"");
    check(t.contains("\"x\": \"2k\"") && t.contains("// comment") && t.contains("// why"), "replace keeps comments");

    t = setMember(text, *o, "z", "3");
    check(t.contains("{ \"x\": \"1k\", \"y\": 2, \"z\": 3 }") && parses(t), "member added on the line: " + t);
    t = setMember(text, *m, "r", "4");
    check(t.contains("\"q\": 2,\n    \"r\": 4\n  }") && parses(t), "member added on a line of its own: " + t);
    t = setMember(text, *e, "x", "1");  // not an object: replaced as a whole
    t = removeMember(text, *o, "x");
    check(t.contains("{ \"y\": 2 }") && parses(t), "first member removed: " + t);
    t = removeMember(text, *o, "y");
    check(t.contains("{ \"x\": \"1k\" }") && parses(t), "last member removed: " + t);
    t = removeMember(text, *m, "p");
    check(t.contains("\"m\": {\n    \"q\": 2\n  }") && parses(t), "member line removed with its comment: " + t);

    t = removeItem(text, *a, 1);
    check(t.contains("[1, 3]") && parses(t), "middle item removed: " + t);
    t = removeItem(text, *a, 2);
    check(t.contains("[1, 2]") && parses(t), "last item removed: " + t);
    t = appendItems(text, *a, {"4"});
    check(parses(t) && t.contains("3,\n    4]"), "item appended: " + t);
    t = appendItems(text, *e, {"{ \"k\": 1 }", "5"});
    check(parses(t) && t.contains("\"e\": [\n    { \"k\": 1 },\n    5\n  ]"), "items appended to an empty array: " + t);

    const QString list = "[\n  { \"n\": 1 },  // one\n  { \"n\": 2 },\n  { \"n\": 3 }\n]";
    JValue l;
    parseJsonc(list, l);
    t = removeItem(list, l, 0);
    check(t == "[\n  { \"n\": 2 },\n  { \"n\": 3 }\n]", "item line removed: " + t);
    t = removeItem(list, l, 2);
    check(t == "[\n  { \"n\": 1 },  // one\n  { \"n\": 2 }\n]", "last item line removed: " + t);
    t = appendItems(list, l, {"{ \"n\": 4 }"});
    check(t == "[\n  { \"n\": 1 },  // one\n  { \"n\": 2 },\n  { \"n\": 3 },\n  { \"n\": 4 }\n]", "appended: " + t);

    int pos = 0;
    check(!parseJsonc("{ \"a\": [1, 2 }", root, &error, &pos) && pos == 13, QString("error position %1").arg(pos));
}

QString readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

bool errorFree(const QString& text) {
    const CheckResult r = checkText(text, QDir::tempPath());
    for (const Diagnostic& d : r.diagnostics)
        if (d.severity == Diagnostic::Error) return false;
    return r.ok;
}

void testSchematic(const QString& examples) {
    // a file without drawing: placed automatically, labels name the nodes
    const QString rc = readFile(QDir(examples).filePath("rc_lowpass.json"));
    Schematic s;
    QString error;
    check(readSchematic(rc, s, &error), "rc_lowpass read: " + error);
    check(s.components.size() == 3 && !s.hasSection, "three components, no drawing yet");
    check(s.grounds.size() == 2 && s.labels.size() == 4, QString("labels %1, grounds %2").arg(s.labels.size()).arg(s.grounds.size()));
    Connectivity c = connectSchematic(s);
    check(c.conflicts.isEmpty(), "automatic drawing agrees with the text");
    check(c.pinNodes.value(1) == QStringList({"in", "out"}), "R1 between in and out: " + c.pinNodes.value(1).join(","));

    // writing the drawing keeps the text and adds the section
    QString t = writeSchematic(rc, s);
    check(t.contains("// RC low-pass") && t.contains("\"schematic\": {") && errorFree(t), "section written:\n" + t);
    check(t.contains("{ \"type\": \"R\", \"name\": \"R1\", \"nodes\": [\"in\", \"out\"], \"value\": \"1k\" }"),
          "components untouched");
    Schematic s2;
    check(readSchematic(t, s2) && s2.hasSection && s2.components.size() == 3, "read back");
    bool same = s2.labels.size() == s.labels.size() && s2.grounds.size() == s.grounds.size();
    for (int k = 0; k < s.components.size() && same; ++k)
        same = s2.components[k].placed && s2.components[k].place.at == s.components[k].place.at;
    check(same, "positions read back");
    check(writeSchematic(t, s2) == t, "writing again changes nothing");

    // a new drawing: source, resistor, wire, ground, label
    const QString empty = "{\n  \"components\": [],\n  \"analyses\": [{ \"type\": \"op\" }]\n}\n";
    check(readSchematic(empty, s), "empty file read");
    const auto part = [](const char* id) {
        for (const PartTemplate& p : partTemplates())
            if (p.id == id) return p;
        return partTemplates().first();
    };
    SComponent v = makeComponent(part("v"), s);
    v.place.at = QPoint(0, 0);
    s.components << v;
    SComponent r = makeComponent(part("r"), s);
    r.place.at = QPoint(4, -2);
    r.place.rot = 270;  // pin a on the left
    s.components << r;
    check(v.name == "V1" && r.name == "R1", "names " + v.name + " " + r.name);
    s.wires.push_back({{QPoint(0, -2), QPoint(2, -2)}});
    s.grounds.push_back({QPoint(0, 2), 0});
    s.labels.push_back({"out", QPoint(6, -2), 0});
    t = writeSchematic(empty, s);
    check(errorFree(t), "drawn circuit passes the check:\n" + t);
    check(t.contains("\"nodes\": [\"n1\", \"0\"]") && t.contains("\"nodes\": [\"n1\", \"out\"]"),
          "nodes from the drawing:\n" + t);
    check(t.contains("\"analyses\": [{ \"type\": \"op\" }]"), "rest of the text kept");
    check(readSchematic(t, s) && s.components.size() == 2 && s.components[1].placed, "drawn circuit read back");
    c = connectSchematic(s);
    check(c.conflicts.isEmpty() && c.junctions.isEmpty() && c.openEnds.isEmpty(), "no conflicts, dots or open ends");

    // the wire removed: the resistor keeps no connection to the source
    Schematic cut = s;
    cut.wires.clear();
    QString t2 = writeSchematic(t, cut);
    check(t2.contains("\"nodes\": [\"n1\", \"0\"]") && t2.contains("\"nodes\": [\"n2\", \"out\"]"), "split net renamed:\n" + t2);

    // a T junction is a dot
    Schematic tee = s;
    tee.wires.push_back({{QPoint(1, -2), QPoint(1, 2), QPoint(0, 2)}});
    c = connectSchematic(tee);
    check(c.junctions.size() == 1 && c.junctions.value(0) == QPoint(1, -2), "junction dot at the T");
    check(c.pinNodes[0][0] == "0" && c.pinNodes[1][0] == "0", "T to ground grounds the net");

    // renamed and removed
    Schematic renamed = s;
    renamed.components[1].name = "Rload";
    t2 = writeSchematic(t, renamed);
    check(t2.contains("\"name\": \"Rload\"") && t2.contains("\"Rload\": { \"at\"") && !t2.contains("\"R1\""), "renamed:\n" + t2);
    Schematic removed = s;
    removed.components.removeAt(1);
    t2 = writeSchematic(t, removed);
    check(readSchematic(t2, s2) && s2.components.size() == 1 && !t2.contains("R1"), "removed:\n" + t2);

    // the text changed against the drawing: reported, not repaired
    QString edited = t;
    edited.replace("[\"n1\", \"out\"]", "[\"x\", \"out\"]");
    check(readSchematic(edited, s2), "edited read");
    c = connectSchematic(s2);
    check(c.conflicts.size() == 1 && c.conflicts[0] == qMakePair(1, 0), QString("conflict reported: %1").arg(c.conflicts.size()));

    // all examples: the automatic drawing connects as the text
    for (const QString& f : QDir(examples).entryList({"*.json"}, QDir::Files, QDir::Name)) {
        const QString text = readFile(QDir(examples).filePath(f));
        Schematic e;
        if (!readSchematic(text, e, &error)) {
            check(false, f + ": " + error);
            continue;
        }
        c = connectSchematic(e);
        check(c.conflicts.isEmpty(), f + ": automatic drawing differs from the text");
        const QString written = writeSchematic(text, e);
        check(errorFree(written), f + ": still valid with the drawing");
        Schematic back;
        check(readSchematic(written, back) && connectSchematic(back).conflicts.isEmpty(), f + ": drawing read back");
    }
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
    std::printf("[test] JSON with positions\n");
    testJsonSpan();
    std::printf("[test] schematic\n");
    testSchematic(examples);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
