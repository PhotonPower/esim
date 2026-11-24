#include "Checker.hpp"

#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>
#include <sstream>

#include "circuit/Circuit.hpp"
#include "io/Netlist.hpp"

namespace {

// Line (0-based) of the first match of `pattern` at or after line `from`; -1 if none.
int findLine(const QStringList& lines, const QString& pattern, int from = 0) {
    const QRegularExpression re(pattern);
    for (int k = std::max(from, 0); k < lines.size(); ++k)
        if (re.match(lines[k]).hasMatch()) return k;
    return -1;
}

QString quoted(const QString& name) { return QRegularExpression::escape(name); }

// Where in the text a message of the parser points to. The messages name
// what they are about: "component 'R1': unknown parameter \"colour\"",
// "subcircuit \"amp\": ...", "model \"QGEN\": ...", "tran: tstop must be > 0".
int locate(const QStringList& lines, const QString& message) {
    int anchor = -1;
    static const QRegularExpression component(R"(component '([^']+)')");
    static const QRegularExpression indexed(R"(components\[(\d+)\])");
    static const QRegularExpression subcircuit(R"re(subcircuit "([^"]+)")re");
    static const QRegularExpression model(R"re(model "([^"]+)")re");
    static const QRegularExpression analysis(R"(^(op|dc|ac|tran|pss|noise|sens|fit|fourier)\b)");
    static const QRegularExpression key(R"re("([A-Za-z_][\w.\[\]]*)")re");
    if (auto m = component.match(message); m.hasMatch()) {
        const QString name = m.captured(1).section('.', -1);
        anchor = findLine(lines, "\"name\"\\s*:\\s*\"" + quoted(name) + "\"");
    } else if (auto m2 = indexed.match(message); m2.hasMatch()) {
        // the n-th component without a name: count the "type" keys after "components"
        int line = findLine(lines, R"("components"\s*:)");
        for (int n = m2.captured(1).toInt(); line >= 0 && n >= 0; --n)
            line = findLine(lines, R"("type"\s*:)", line + 1);
        anchor = line;
    } else if (auto m3 = subcircuit.match(message); m3.hasMatch()) {
        anchor = findLine(lines, "\"" + quoted(m3.captured(1)) + "\"\\s*:");
    } else if (auto m4 = model.match(message); m4.hasMatch()) {
        anchor = findLine(lines, "\"" + quoted(m4.captured(1)) + "\"\\s*:");
        if (anchor < 0) anchor = findLine(lines, "\"model\"\\s*:\\s*\"" + quoted(m4.captured(1)) + "\"");
    } else if (auto m5 = analysis.match(message); m5.hasMatch()) {
        anchor = findLine(lines, "\"type\"\\s*:\\s*\"" + m5.captured(1) + "\"");
    }
    // a quoted key in the message: look for it near the anchor
    const QString rest = anchor >= 0 ? message.section(':', 1) : message;
    auto it = key.globalMatch(rest);
    while (it.hasNext()) {
        const QString k = it.next().captured(1);
        const int line = findLine(lines, "\"" + quoted(k) + "\"\\s*:", std::max(anchor, 0));
        if (line >= 0 && (anchor < 0 || line - anchor < 40)) return line;
    }
    return anchor;
}

Diagnostic diagnostic(Diagnostic::Severity severity, const QString& message, const QStringList& lines) {
    Diagnostic d;
    d.severity = severity;
    d.message = message;
    // JSON syntax: "... parse error at line 3, column 5: ..."
    static const QRegularExpression at(R"(at line (\d+), column (\d+))");
    if (const auto m = at.match(message); m.hasMatch()) {
        d.line = m.captured(1).toInt() - 1;
        d.column = std::max(m.captured(2).toInt() - 1, 0);
        static const QRegularExpression noise(R"(\[json\.exception\.[a-z_]+\.\d+\]\s*)");
        d.message = QString(message).remove(noise);
        return d;
    }
    d.line = locate(lines, message);
    return d;
}

}  // namespace

CheckResult checkText(const QString& text, const QString& baseDir) {
    CheckResult r;
    const QStringList lines = text.split('\n');
    std::ostringstream warnings;
    std::ostream* previous = esim::setWarningStream(&warnings);
    try {
        const nlohmann::json root = nlohmann::json::parse(text.toStdString(), nullptr, true, /*ignore_comments=*/true);
        esim::Netlist nl = esim::parseNetlist(root, true, baseDir.toStdString());
        const esim::Circuit& ckt = *nl.circuit;
        int components = static_cast<int>(ckt.devices().size());
        for (int i = 0; i < ckt.size(); ++i)
            if (ckt.kind(i) == esim::UnknownKind::Variable) {
                --components;  // the block of equations counts as a device
                break;
            }
        r.summary = QObject::tr("%1 Bauteile, %2 Unbekannte").arg(components).arg(ckt.size());
        for (int i = 0; i < ckt.size(); ++i)
            if (ckt.kind(i) == esim::UnknownKind::Voltage) {
                QString name = QString::fromStdString(ckt.unknownName(i));  // "v(out)"
                // a plugin may declare unknowns of its own as voltages: not a node of the schematic
                if (!(name.startsWith("v(") && name.endsWith(')'))) continue;
                name = name.mid(2, name.size() - 3);
                r.nodes << name;
            }
        for (const std::string& note : ckt.notes)
            r.diagnostics << diagnostic(Diagnostic::Warning, QString::fromStdString(note), lines);
        r.ok = true;
    } catch (const nlohmann::json::parse_error& e) {
        r.diagnostics << diagnostic(Diagnostic::Error, QObject::tr("JSON-Syntaxfehler: %1").arg(e.what()), lines);
    } catch (const std::exception& e) {
        r.diagnostics << diagnostic(Diagnostic::Error, QString::fromStdString(e.what()), lines);
    }
    esim::setWarningStream(previous);
    std::istringstream in(warnings.str());
    for (std::string line; std::getline(in, line);) {
        QString w = QString::fromStdString(line);
        if (w.startsWith("warning: ")) w = w.mid(9);
        if (!w.trimmed().isEmpty()) r.diagnostics << diagnostic(Diagnostic::Warning, w, lines);
    }
    return r;
}

Checker::Checker(QObject* parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<CheckResult>::finished, this, [this]() {
        const CheckResult r = watcher_.result();
        if (pending_) {  // the text changed meanwhile: check the newest one
            pending_ = false;
            check(pendingText_, pendingDir_);
            return;
        }
        if (r.generation == generation_) emit checked(r);
    });
}

Checker::~Checker() { watcher_.waitForFinished(); }

void Checker::check(const QString& text, const QString& baseDir) {
    ++generation_;
    if (watcher_.isRunning()) {
        pending_ = true;
        pendingText_ = text;
        pendingDir_ = baseDir;
        return;
    }
    const int generation = generation_;
    watcher_.setFuture(QtConcurrent::run([text, baseDir, generation]() {
        CheckResult r = checkText(text, baseDir);
        r.generation = generation;
        return r;
    }));
}
