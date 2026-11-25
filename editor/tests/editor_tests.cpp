// Tests of the parts of the editor that work without a window: the check of
// the input (every example must pass without errors, errors must point to
// the right line) and the preparation of curves from CSV files.

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QTranslator>
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QProcess>
#include <QTextStream>
#include <cmath>
#include <limits>
#include <cstdio>

#include "Checker.hpp"
#include "Format.hpp"
#include "JsonSpan.hpp"
#include "ModelLibrary.hpp"
#include "ResultData.hpp"
#include "Schematic.hpp"
#include "Symbols.hpp"

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

void testMeasures() {
    // a nested sweep: R outside, C inside, fc measured
    CsvTable t;
    t.header = {"R", "C", "fc"};
    t.columns = {{1, 1, 2, 2}, {3e-9, 1e-9, 3e-9, 1e-9}, {10, 30, 5, 15}};
    MeasureInfo info = measureInfo(t, {}, {"R", "C"});
    check(!info.monteCarlo && info.params == QStringList({"R", "C"}) && info.measures == QStringList{"fc"}, "sweep columns");
    PlotData d = measurePlot(t, info, "C", {});
    check(d.panels.size() == 1 && d.panels[0].curves.size() == 2, "one curve per R");
    if (d.panels.size() == 1 && d.panels[0].curves.size() == 2) {
        const Curve& c = d.panels[0].curves[0];
        check(c.x == QVector<double>({1e-9, 3e-9}) && c.y == QVector<double>({30, 10}), "sorted by C");
        check(c.group == "R = 1" && c.runs == 2 && c.markers == 2, "family labelled, line with points: " + c.group);
    }
    // Monte Carlo: the statistics name the measurements
    t.header = {"run", "C", "fc"};
    info = measureInfo(t, {"fc"}, {"run"});
    check(info.monteCarlo && info.params == QStringList{"C"} && info.measures == QStringList{"fc"}, "Monte Carlo columns");
    d = measurePlot(t, info, "C", {});
    check(d.panels.value(0).curves.size() == 1 && d.panels[0].curves[0].markers == 1, "Monte Carlo: one cloud");
    // decades of a parameter: logarithmic axis
    t.columns[1] = {1e-9, 1e-8, 1e-7, 1e-6};
    check(measurePlot(t, info, "C", {}).logX, "log axis over decades");

    QVector<double> v;
    for (int k = 0; k < 1000; ++k) v << std::sin(k * 1.3) * 3.0 + 10.0;
    v << std::numeric_limits<double>::quiet_NaN();
    const Histogram h = histogram("x", v);
    int sum = 0;
    for (int c : h.counts) sum += c;
    check(h.count == 1000 && sum == 1000, "every finite value in a bin");
    check(h.counts.size() >= 5 && h.counts.size() <= 60 && h.edges.size() == h.counts.size() + 1, "5 to 60 bins");
    check(std::abs(h.mean - 10.0) < 0.05 && std::abs(h.std - 3.0 / std::sqrt(2.0)) < 0.05, "mean and deviation");
    check(h.edges.first() == h.min && std::abs(h.edges.last() - h.max) < 1e-12, "bins span the values");
    const Histogram one = histogram("c", {2, 2, 2});
    check(one.counts.size() == 1 && one.counts[0] == 3, "constant values: one bin");
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
    check(parses(t) && t.contains("[1, 2, 3, 4]"), "item appended on the line: " + t);
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

bool errorFree(const QString& text, const QString& baseDir = QDir::tempPath()) {
    const CheckResult r = checkText(text, baseDir);
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
        check(errorFree(written, examples), f + ": still valid with the drawing");  // tables from files lie next to them
        Schematic back;
        check(readSchematic(written, back) && connectSchematic(back).conflicts.isEmpty(), f + ": drawing read back");
    }
}

void testSubcircuits(const QString& examples) {
    const QString ring = readFile(QDir(examples).filePath("ring_oscillator_bsim3.json"));
    Schematic s;
    QString error;
    check(readSchematic(ring, s, &error, "inv"), "subcircuit inv read: " + error);
    check(s.scope == "inv" && s.ports == QStringList({"in", "out", "vdd"}), "scope and ports: " + s.ports.join(","));
    check(s.find("MP") >= 0 && s.find("MN") >= 0 && s.components.size() == 2, "its two transistors");
    int ports = 0;
    for (const SLabel& l : s.labels) ports += l.port ? 1 : 0;
    check(ports == 6 && s.grounds.size() == 2, QString("pins on ports drawn as ports: %1").arg(ports));
    check(connectSchematic(s).conflicts.isEmpty(), "subcircuit drawing agrees with the text");
    check(!readSchematic(ring, s, &error, "nope"), "unknown subcircuit fails");

    // the drawing goes into the definition, the main circuit stays without
    readSchematic(ring, s, nullptr, "inv");
    QString t = writeSchematic(ring, s);
    check(errorFree(t), "written subcircuit drawing passes the check");
    Schematic main, back;
    check(readSchematic(t, main) && !main.hasSection, "main circuit still without drawing");
    check(readSchematic(t, back, nullptr, "inv") && back.hasSection && back.labels.size() == s.labels.size(),
          "subcircuit drawing read back");
    check(writeSchematic(t, back) == t, "writing again changes nothing");
    int b = int(t.indexOf("\"inv\": {")), e = int(t.indexOf("\"schematic\"", b));
    check(b > 0 && e > b && e < t.indexOf("\"components\": [", b + 200), "section inside the definition");

    // a port with a new name extends the ports
    back.labels.push_back({"en", QPoint(30, 30), 0, true});
    t = writeSchematic(t, back);
    check(t.contains("\"ports\": [\"in\", \"out\", \"vdd\", \"en\"]"), "port added to the list");

    // instances and the place of a component in the text
    const QVector<Instance> inst = instancesOf(ring, "inv");
    check(inst.size() >= 5 && inst.first().name == "U0" && inst.first().nodes == QStringList({"in", "out", "vdd0"}),
          QString("instances: %1").arg(inst.size()));
    QString scope;
    int index = -1;
    check(locateComponent(ring, int(ring.indexOf("\"MN\"")), scope, index) && scope == "inv" && index == 1,
          "MN found in inv");
    check(locateComponent(ring, int(ring.indexOf("\"U1\"")), scope, index) && scope.isEmpty(), "U1 in the main circuit");

    // a new subcircuit in a file without any
    const QString rc = readFile(QDir(examples).filePath("rc_lowpass.json"));
    t = addSubcircuit(rc, "amp", {"in", "out"});
    check(addSubcircuit(t, "amp", {"x"}) == t, "existing name: no change");
    check(errorFree(t) && t.contains("// RC low-pass"), "new subcircuit passes the check:\n" + t);
    Schematic amp;
    check(readSchematic(t, amp, nullptr, "amp") && amp.labels.size() == 2 && amp.labels[0].port,
          "its ports are drawn");
    SComponent r;
    for (const PartTemplate& p : partTemplates())
        if (p.id == "r") r = makeComponent(p, amp);
    r.place.at = QPoint(amp.labels[0].at.x() + 4, amp.labels[0].at.y() + 2);
    amp.components << r;
    amp.labels[0].at = r.place.at + QPoint(0, -2);
    amp.labels[1].at = r.place.at + QPoint(0, 2);
    t = writeSchematic(t, amp);
    check(errorFree(t) && t.contains("\"nodes\": [\"in\", \"out\"]"), "resistor between the ports:\n" + t);
    check(readSchematic(t, amp, nullptr, "amp") && amp.components.size() == 1 && connectSchematic(amp).conflicts.isEmpty(),
          "read back");
}

// A small circuit that uses the model, for the check and a run of esim.
QString circuitWith(const LibraryModel& m) {
    const bool bsim3 = m.text.contains(QRegularExpression("level\\s*=\\s*(8|49|53)", QRegularExpression::CaseInsensitiveOption));
    const bool p = m.type == "pnp" || m.type == "pmos" || m.type == "pjf";
    const QString supply = p ? "-1.8" : "1.8";
    QString device;
    if (m.kind() == 'd')
        device = R"({ "type": "D", "name": "D1", "nodes": ["out", "0"], "model": "%1" })";
    else if (m.kind() == 'q')
        device = R"({ "type": "Q", "name": "Q1", "nodes": ["out", "b", "0"], "model": "%1" },
    { "type": "R", "name": "RB", "nodes": ["vcc", "b"], "value": "1meg" })";
    else if (m.kind() == 'm')
        device = bsim3 ? R"({ "type": "M", "name": "M1", "nodes": ["out", "vcc", "0", "0"], "model": "%1", "w": "1u", "l": "0.18u" })"
                       : R"({ "type": "M", "name": "M1", "nodes": ["out", "vcc", "0"], "model": "%1" })";
    else
        device = R"({ "type": "J", "name": "J1", "nodes": ["out", "0", "0"], "model": "%1" })";
    return QString(R"({
  "components": [
    { "type": "V", "name": "V1", "nodes": ["vcc", "0"], "dc": %2 },
    { "type": "R", "name": "R1", "nodes": ["vcc", "out"], "value": "10k" },
    %3
  ],
  "analyses": [{ "type": "op" }]
}
)").arg(supply, device.arg(m.name));
}

void testLibrary(const QString& examples) {
    const QString lib = "* header\n\n"
                        "* QX Fast switch\n* second line\n"
                        ".MODEL QX npn(IS=1f BF=100\n"
                        "+\tVAF=50 ; a comment\n"
                        "* between\n"
                        "+ CJE=1p)\n"
                        ".model DY D (IS=2n)\n"
                        ".model SW1 SW(RON=1)\n"
                        ".subckt inner a b\n.model DIN D(IS=1n)\n.ends\n";
    const QVector<LibraryModel> ms = parseSpiceLibrary(lib, "test.lib");
    check(ms.size() == 3 && ms[2].type == "subckt" && ms[2].name == "inner",
          QString("two models and the subcircuit (switch and model inside it skipped): %1").arg(ms.size()));
    if (ms.size() == 3) {
        check(ms[0].name == "QX" && ms[0].type == "npn" && ms[0].kind() == 'q', "name and type of a card");
        check(ms[0].text == ".MODEL QX npn(IS=1f BF=100\n+ VAF=50\n+ CJE=1p)", "card with continuations: " + ms[0].text);
        check(ms[0].title() == "Fast switch" && ms[0].description.contains("second line"), "description: " + ms[0].title());
        check(ms[1].name == "DY" && ms[1].description.isEmpty(), "a card without comment");
    }
    const QVector<LibraryModel> js = parseJsonLibrary(readFile(QDir(examples).filePath("bjt_amplifier.json")), "x");
    check(js.size() == 1 && js[0].name == "NPN_GEN" && js[0].type == "npn", "model of an esim file");

    ModelLibrary library;
    library.load({}, examples);
    check(library.problems().isEmpty(), "libraries read: " + library.problems().join("; "));
    const LibraryModel* q = library.find("NPN_GEN");
    check(q && q->type == "npn" && q->source == "Standard" && q->title().contains("Lehrbuch"), "NPN_GEN in the standard library");
    check(library.find("D_RECT") && library.find("D_RECT")->source.startsWith("Beispiel"), "models of the examples");

    // every model of the standard library runs in esim
    QTemporaryDir dir;
    int standard = 0;
    for (const LibraryModel& m : library.models()) {
        if (m.source != "Standard") continue;
        ++standard;
        const QString text = addModel(circuitWith(m), m);
        check(fileModels(text) == QStringList{m.name}, m.name + ": model added");
        const CheckResult r = checkText(text, dir.path());
        bool errors = !r.ok;
        for (const Diagnostic& d : r.diagnostics) errors = errors || d.severity == Diagnostic::Error;
        check(!errors, m.name + ": " + describe(r));
#ifdef ESIM_BINARY
        const QString file = dir.filePath(m.name + ".json");
        QFile f(file);
        if (f.open(QIODevice::WriteOnly)) f.write(text.toUtf8());
        f.close();
        QProcess esim;
        esim.start(ESIM_BINARY, {file, "-o", dir.path()});
        esim.waitForFinished(30000);
        check(esim.exitStatus() == QProcess::NormalExit && esim.exitCode() == 0,
              m.name + ": esim runs the operating point: " + QString::fromUtf8(esim.readAllStandardError()));
#endif
    }
    check(standard >= 8, QString("standard library: %1 models").arg(standard));

    // added once, in the file's own layout
    const QString rc = readFile(QDir(examples).filePath("rc_lowpass.json"));
    const QString t = addModel(rc, *q);
    check(t.contains("\"models\": {\n    \"NPN_GEN\": \".model NPN_GEN NPN(") && errorFree(t), "card added:\n" + t);
    check(addModel(t, *q) == t, "a model is added only once");
    const QString amp = readFile(QDir(examples).filePath("bjt_amplifier.json"));
    const LibraryModel* d = library.find("D_GEN");
    check(d && fileModels(addModel(amp, *d)) == QStringList({"NPN_GEN", "D_GEN"}), "added to an existing models section");
    const LibraryModel* n18 = library.find("n18");
    const QString long_ = n18 ? addModel(rc, *n18) : QString();
    check(long_.contains("\"n18\": [\n      \".model n18 NMOS") && errorFree(long_), "a long card added as lines");
}

void testSpiceLibrary() {
    // a library as manufacturers write them: comment above the subcircuit,
    // models and a helper subcircuit outside it, more than it needs
    const QString lib = R"lib(* Test library
* OPX simple operational amplifier
* pins: non-inverting input, inverting input, output, supplies
.SUBCKT OPX inp inn out vp vn
Rin inp inn 1meg
Egain mid 0 POLY(1) inp inn 0 1e5
Xbuf mid out BUF
Dhi out vp DCLAMP
Dlo vn out DCLAMP
.ENDS OPX

.subckt BUF a b
Rb a b 10
.ends

.model DCLAMP D(IS=1e-14)
.model UNUSED NPN(BF=100)
.subckt OTHER x
R1 x 0 1
.ends
)lib";
    const QVector<LibraryModel> ms = parseSpiceLibrary(lib, "test.lib");
    const LibraryModel* opx = nullptr;
    for (const LibraryModel& m : ms)
        if (m.name == "OPX") opx = &m;
    check(opx && opx->type == "subckt" && opx->kind() == 'x', "subcircuit found");
    if (!opx) return;
    check(opx->ports == QStringList({"inp", "inn", "out", "vp", "vn"}), "ports: " + opx->ports.join(","));
    check(opx->title() == "simple operational amplifier", "description: " + opx->title());
    check(opx->text.contains(".subckt BUF") && opx->text.contains(".model DCLAMP") && !opx->text.contains("UNUSED") &&
              !opx->text.contains("OTHER"),
          "takes along what it uses, nothing else:\n" + opx->text);

    // into a file: a non-inverting amplifier with gain 10
    const QString base = R"json({
  "components": [
    { "type": "V", "name": "VP", "nodes": ["vp", "0"], "dc": 15 },
    { "type": "V", "name": "VN", "nodes": ["vn", "0"], "dc": -15 },
    { "type": "V", "name": "VIN", "nodes": ["in", "0"], "dc": 0.1 },
    { "type": "R", "name": "RF", "nodes": ["out", "fb"], "value": "9k" },
    { "type": "R", "name": "RG", "nodes": ["fb", "0"], "value": "1k" }
  ],
  "analyses": [{ "type": "op" }],
  "outputs": ["v(out)"]
}
)json";
    QString t = addModel(base, *opx);
    check(fileSubcircuits(t) == QStringList{"OPX"} && addModel(t, *opx) == t, "subcircuit added once");
    Schematic s;
    check(readSchematic(t, s) && s.spiceSubcircuits.contains("OPX") && s.subcircuits.value("OPX") == opx->ports,
          "the drawing knows its ports");
    SComponent x = makeSubcircuit("OPX", s);
    check(symbolOf(x).pins.size() == 5, "five pins");
    x.place.at = QPoint(0, 40);
    s.components << x;
    // connect the pins by labels at their positions
    const QStringList nets = {"in", "fb", "out", "vp", "vn"};
    for (int p = 0; p < 5; ++p) s.labels.push_back({nets[p], x.place.map(symbolOf(x).pins[p].pos), 0});
    t = writeSchematic(t, s);
    check(errorFree(t) && t.contains("\"subcircuit\": \"OPX\", \"nodes\": [\"in\", \"fb\", \"out\", \"vp\", \"vn\"]"),
          "instance in the text:\n" + t);
#ifdef ESIM_BINARY
    QTemporaryDir dir;
    QFile f(dir.filePath("amp.json"));
    if (f.open(QIODevice::WriteOnly)) f.write(t.toUtf8());
    f.close();
    QProcess esim;
    esim.start(ESIM_BINARY, {dir.filePath("amp.json"), "-o", dir.path(), "--summary", dir.filePath("s.json")});
    esim.waitForFinished(30000);
    QFile summary(dir.filePath("s.json"));
    double vout = 0;
    if (summary.open(QIODevice::ReadOnly)) {
        const QString j = QString::fromUtf8(summary.readAll());
        static const QRegularExpression re(R"re("v\(out\)":\s*([-0-9.eE+]+))re");
        vout = re.match(j).captured(1).toDouble();
    }
    check(esim.exitCode() == 0 && std::abs(vout - 1.0) < 1e-3, QString("gain 10 with the SPICE amplifier: v(out) = %1 %2")
                                                                   .arg(vout).arg(QString::fromUtf8(esim.readAllStandardError())));
#endif
}

// v(out) of the operating point, computed by esim; NaN if it fails
double opValue(const QString& text, const QString& quantity) {
#ifdef ESIM_BINARY
    QTemporaryDir dir;
    QFile f(dir.filePath("c.json"));
    if (f.open(QIODevice::WriteOnly)) f.write(text.toUtf8());
    f.close();
    QProcess esim;
    esim.start(ESIM_BINARY, {dir.filePath("c.json"), "-o", dir.path(), "--summary", dir.filePath("s.json")});
    esim.waitForFinished(30000);
    QFile summary(dir.filePath("s.json"));
    if (esim.exitCode() != 0 || !summary.open(QIODevice::ReadOnly)) return std::nan("");
    const QString j = QString::fromUtf8(summary.readAll());
    const QRegularExpression re("\"" + QRegularExpression::escape(quantity) + "\":\\s*([-0-9.eE+]+)");
    const auto m = re.match(j);
    return m.hasMatch() ? m.captured(1).toDouble() : std::nan("");
#else
    Q_UNUSED(text);
    Q_UNUSED(quantity);
    return 0.0;
#endif
}

void testExtraction() {
    // source, R1 lying, R2 standing to ground; "out" between R1 and R2
    const QString empty = "{\n  \"components\": [],\n  \"analyses\": [{ \"type\": \"op\" }],\n  \"outputs\": [\"v(out)\"]\n}\n";
    Schematic s;
    readSchematic(empty, s);
    const auto part = [](const char* id) {
        for (const PartTemplate& p : partTemplates())
            if (p.id == id) return p;
        return partTemplates().first();
    };
    SComponent v = makeComponent(part("v"), s);
    v.place.at = QPoint(0, 0);
    s.components << v;
    SComponent r1 = makeComponent(part("r"), s);
    r1.place.at = QPoint(4, -2);
    r1.place.rot = 270;
    s.components << r1;
    SComponent r2 = makeComponent(part("r"), s);
    r2.place.at = QPoint(8, 0);
    s.components << r2;
    s.wires.push_back({{QPoint(0, -2), QPoint(2, -2)}});
    s.wires.push_back({{QPoint(6, -2), QPoint(8, -2)}});
    s.grounds.push_back({QPoint(0, 2), 0});
    s.grounds.push_back({QPoint(8, 2), 0});
    s.labels.push_back({"out", QPoint(8, -2), 0});
    const QString drawn = writeSchematic(empty, s);
    const double before = opValue(drawn, "v(out)");
    check(errorFree(drawn) && std::abs(before - 0.5) < 1e-9, QString("divider drawn: v(out) = %1").arg(before));

    Schematic now;
    readSchematic(drawn, now);
    ItemSet sel;
    sel.comps = {"R1", "R2"};
    sel.wires = {1};
    sel.grounds = {1};
    check(extractSubcircuit(drawn, now, sel, "teil 1").text.isEmpty(), "a name with a blank is refused");
    const Extraction e = extractSubcircuit(drawn, now, sel, "teil1");
    check(!e.text.isEmpty(), "extracted: " + e.error);
    check(e.ports == QStringList({"n1", "out"}), "ports: the net to the source, then out: " + e.ports.join(","));
    check(errorFree(e.text), "result passes the check:\n" + e.text);
    const double after = opValue(e.text, "v(out)");
    check(std::abs(after - before) < 1e-12, QString("same v(out) as before: %1\n").arg(after) + e.text);

    Schematic top, sub;
    check(readSchematic(e.text, top) && top.find("R1") < 0 && top.find(e.instance) >= 0 &&
              top.components[top.find(e.instance)].nodes == QStringList({"n1", "out"}),
          "the instance replaces R1 and R2");
    check(connectSchematic(top).conflicts.isEmpty(), "main drawing agrees with the text");
    int outs = 0;
    for (const SLabel& l : top.labels) outs += l.name == "out" && l.at == QPoint(8, -2) ? 1 : 0;
    check(outs == 1, "no second label on top of an existing one");
    check(readSchematic(e.text, sub, nullptr, "teil1") && sub.components.size() == 2 && sub.hasSection,
          "the subcircuit has R1, R2 and a drawing");
    int ports = 0;
    for (const SLabel& l : sub.labels) ports += l.port ? 1 : 0;
    check(ports == 2 && sub.grounds.size() == 1 && sub.wires.size() == 1, "ports, ground and wire inside");
    check(connectSchematic(sub).conflicts.isEmpty(), "subcircuit drawing agrees with its text");
    check(extractSubcircuit(e.text, top, sel, "teil1").text.isEmpty(), "the name is taken now");
}

// Qt's own texts in German, from the catalog the editor loads (qt_de, as
// windeployqt ships it; qtbase_de as the fallback)
void testTranslations() {
    QTranslator t;
    const QString dir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (!t.load(QStringLiteral("qt_de"), dir) && !t.load(QStringLiteral("qtbase_de"), dir)) {
        std::printf("  (no German Qt translations installed: skipped)\n");
        return;
    }
    check(t.translate("QPlatformTheme", "Cancel") == QString::fromUtf8("Abbrechen"),
          "Qt's button texts in German: " + t.translate("QPlatformTheme", "Cancel"));
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
    std::printf("[test] measurements over runs\n");
    testMeasures();
    std::printf("[test] numbers with SI prefixes\n");
    testFormat();
    std::printf("[test] JSON with positions\n");
    testJsonSpan();
    std::printf("[test] schematic\n");
    testSchematic(examples);
    std::printf("[test] subcircuits\n");
    testSubcircuits(examples);
    std::printf("[test] model library\n");
    testLibrary(examples);
    std::printf("[test] SPICE subcircuits in the library\n");
    testSpiceLibrary();
    std::printf("[test] selection into a subcircuit\n");
    testExtraction();
    std::printf("[test] Qt's texts in German\n");
    testTranslations();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
