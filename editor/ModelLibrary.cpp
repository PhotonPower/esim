#include "ModelLibrary.hpp"

#include <QDir>
#include <algorithm>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include "JsonSpan.hpp"

namespace {

const QStringList kTypes = {"d", "npn", "pnp", "nmos", "pmos", "njf", "pjf"};

QString normalType(QString t) {
    t = t.toLower();
    if (t == "diode") return QStringLiteral("d");
    return kTypes.contains(t) ? t : QString();
}

// name and type of a ".model NAME TYPE(...)" card
bool cardHead(const QString& card, QString& name, QString& type) {
    static const QRegularExpression head(QStringLiteral(R"(^\s*\.model\s+(\S+)\s+([A-Za-z]+))"),
                                         QRegularExpression::CaseInsensitiveOption);
    const auto m = head.match(card);
    if (!m.hasMatch()) return false;
    name = m.captured(1);
    type = normalType(m.captured(2));
    return true;
}

QString indentOf(const QString& text, const JValue& obj) {
    if (obj.keyBegins.empty()) return indentationAt(text, obj.begin) + QStringLiteral("  ");
    return indentationAt(text, obj.keyBegins.front());
}

}  // namespace

QString LibraryModel::title() const {
    // the comment line that starts with the name, else the first one
    const QStringList lines = description.split('\n');
    for (const QString& l : lines) {
        const QString t = l.trimmed();
        if (t.startsWith(name + ' ', Qt::CaseInsensitive))
            return t.mid(name.size() + 1).remove(QRegularExpression(QStringLiteral("^[\\s:–-]+"))).trimmed();
    }
    return lines.value(0).trimmed();
}

char LibraryModel::kind() const {
    if (type == "d") return 'd';
    if (type == "npn" || type == "pnp") return 'q';
    if (type == "nmos" || type == "pmos") return 'm';
    if (type == "njf" || type == "pjf") return 'j';
    if (type == "subckt") return 'x';
    return 0;
}

QVector<LibraryModel> parseSpiceLibrary(const QString& text, const QString& source) {
    QVector<LibraryModel> out;
    const QStringList lines = text.split('\n');
    // the top-level definitions of the file: models (their cards) and
    // subcircuits (their lines from .subckt to .ends), for the dependencies
    struct Block {
        QString name;
        QStringList lines;
    };
    QVector<Block> blocks;  // models and subcircuits at the top level, in order
    QStringList comment;    // the comment block right above the current line
    QStringList header;     // the first comment block of the file
    int depth = 0;          // inside a .subckt
    Block current;
    QString currentComment;
    QStringList currentPorts;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        line.remove('\r');
        const QString t = line.trimmed();
        const QString lower = t.toLower();
        if (depth > 0) {
            current.lines << line;
            if (lower.startsWith(".subckt")) ++depth;
            if (lower.startsWith(".ends") && --depth == 0) {
                LibraryModel m;
                m.name = current.name;
                m.type = QStringLiteral("subckt");
                m.ports = currentPorts;
                m.description = currentComment;
                m.source = source;
                m.text = current.lines.join('\n');
                out << m;
                blocks << current;
                current = Block();
            }
            continue;
        }
        if (t.startsWith('*')) {
            const QString c = t.mid(1).trimmed();
            if (!c.contains(QRegularExpression(QStringLiteral("^[*=#~-]*$")))) comment << c;  // not a ruler line
            continue;
        }
        if (t.isEmpty()) {
            if (header.isEmpty()) header = comment;
            comment.clear();
            continue;
        }
        if (header.isEmpty()) header = comment;
        if (lower.startsWith(".subckt")) {
            // ".subckt NAME port ... [PARAMS: a=1 ...]"
            QString head = t;
            while (i + 1 < lines.size() && QString(lines[i + 1]).trimmed().startsWith('+'))
                head += ' ' + QString(lines[++i]).trimmed().mid(1);
            const QStringList words = head.simplified().split(' ');
            current = Block{words.value(1), {head}};
            currentPorts.clear();
            for (int k = 2; k < words.size(); ++k) {
                if (words[k].contains('=') || words[k].compare("params:", Qt::CaseInsensitive) == 0) break;
                currentPorts << words[k].toLower();
            }
            currentComment = comment.join('\n');
            comment.clear();
            depth = 1;
            continue;
        }
        if (!lower.startsWith(".model")) {
            comment.clear();
            continue;
        }
        // the card with its continuation lines; comments within are dropped
        QStringList card{t.section(';', 0, 0).trimmed()};
        while (i + 1 < lines.size()) {
            const QString next = QString(lines[i + 1]).remove('\r').trimmed();
            if (next.startsWith('+')) {
                card << QStringLiteral("+ ") + next.mid(1).section(';', 0, 0).trimmed();
                ++i;
            } else if (next.startsWith('*') && i + 2 < lines.size() &&
                       QString(lines[i + 2]).trimmed().startsWith('+')) {
                ++i;
            } else {
                break;
            }
        }
        LibraryModel m;
        m.text = card.join('\n');
        QString type;
        if (cardHead(m.text, m.name, type)) {
            blocks << Block{m.name, card};
            if (!type.isEmpty()) {
                m.type = type;
                m.description = comment.join('\n');
                m.source = source;
                out << m;
            }
        }
        comment.clear();
    }
    // a subcircuit takes along the top-level definitions it uses, and theirs
    QSet<QString> helpers;  // subcircuits that others of the file use
    for (LibraryModel& m : out) {
        if (m.type != "subckt") continue;
        QSet<QString> used{m.name.toLower()};
        QStringList extra;
        QStringList todo{m.text};
        while (!todo.isEmpty()) {
            const QString body = todo.takeFirst();
            QSet<QString> words;
            for (const QString& l : body.split('\n')) {
                const QString t = l.trimmed();
                if (t.startsWith('*')) continue;
                for (const QString& w : t.section(';', 0, 0).split(QRegularExpression(QStringLiteral("[\\s(),=]+")),
                                                                Qt::SkipEmptyParts))
                    words.insert(w.toLower());
            }
            for (const Block& b : blocks) {
                const QString n = b.name.toLower();
                if (used.contains(n) || !words.contains(n)) continue;
                used.insert(n);
                helpers.insert(n);
                extra << b.lines.join('\n');
                todo << b.lines.join('\n');
            }
        }
        if (!extra.isEmpty()) m.text += '\n' + extra.join('\n');
    }
    // parts of other subcircuits are not offered on their own
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const LibraryModel& m) { return m.type == "subckt" && helpers.contains(m.name.toLower()); }),
              out.end());
    // a file with a single subcircuit: the head of the file describes it
    int subckts = 0;
    for (const LibraryModel& m : out) subckts += m.type == "subckt";
    if (subckts == 1)
        for (LibraryModel& m : out)
            if (m.type == "subckt" && m.description.isEmpty()) m.description = header.join('\n');
    return out;
}

QVector<LibraryModel> parseJsonLibrary(const QString& text, const QString& source) {
    QVector<LibraryModel> out;
    JValue root;
    if (!parseJsonc(text, root) || !root.isObject()) return out;
    const JValue* models = root.get("models");
    if (!models || !models->isObject()) return out;
    for (size_t k = 0; k < models->items.size(); ++k) {
        const JValue& v = models->items[k];
        LibraryModel m;
        m.name = models->keys[int(k)];
        m.source = source;
        QString cardName;
        if (v.isString()) {
            m.text = v.string;
            cardHead(m.text, cardName, m.type);
        } else if (v.isArray()) {
            QStringList lines;
            for (const JValue& l : v.items) lines << l.string;
            m.text = lines.join('\n');
            cardHead(m.text, cardName, m.type);
        } else if (v.isObject()) {
            m.value = text.mid(v.begin, v.end - v.begin);
            m.text = m.value;
            const JValue* type = v.get("type");
            m.type = type ? normalType(type->string) : QString();
        }
        if (!m.type.isEmpty()) out << m;
    }
    return out;
}

void ModelLibrary::loadFile(const QString& path, const QString& label) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        problems_ << QStringLiteral("%1: %2").arg(path, f.errorString());
        return;
    }
    const QString text = QString::fromUtf8(f.readAll());
    if (path.endsWith(".json", Qt::CaseInsensitive)) models_ += parseJsonLibrary(text, label);
    else models_ += parseSpiceLibrary(text, label);
}

void ModelLibrary::load(const QStringList& paths, const QString& examplesDir) {
    models_.clear();
    problems_.clear();
    loadFile(QStringLiteral(":/library/standard.lib"), QStringLiteral("Standard"));
    const QStringList patterns = {"*.lib", "*.mod", "*.txt", "*.cir", "*.sp", "*.json"};
    for (const QString& p : paths) {
        const QFileInfo info(p);
        if (info.isDir()) {
            QDirIterator it(p, patterns, QDir::Files, QDirIterator::Subdirectories);
            QStringList files;
            while (it.hasNext()) files << it.next();
            files.sort();
            for (const QString& f : files) loadFile(f, f);
        } else if (info.isFile()) {
            loadFile(p, p);
        } else {
            problems_ << QStringLiteral("%1: nicht gefunden").arg(p);
        }
    }
    if (!examplesDir.isEmpty())
        for (const QString& f : QDir(examplesDir).entryList({"*.json"}, QDir::Files, QDir::Name))
            loadFile(QDir(examplesDir).filePath(f), QStringLiteral("Beispiel %1").arg(f));
}

const LibraryModel* ModelLibrary::find(const QString& name) const {
    for (const LibraryModel& m : models_)
        if (m.name.compare(name, Qt::CaseInsensitive) == 0) return &m;
    return nullptr;
}

QStringList fileModels(const QString& text) {
    JValue root;
    if (!parseJsonc(text, root) || !root.isObject()) return {};
    const JValue* models = root.get("models");
    return models && models->isObject() ? models->keys : QStringList();
}

QStringList fileSubcircuits(const QString& text) {
    JValue root;
    if (!parseJsonc(text, root) || !root.isObject()) return {};
    const JValue* subs = root.get("subcircuits");
    return subs && subs->isObject() ? subs->keys : QStringList();
}

QString addModel(const QString& text, const LibraryModel& m) {
    JValue root;
    if (!parseJsonc(text, root) || !root.isObject()) return text;
    const QString section = m.type == "subckt" ? QStringLiteral("subcircuits") : QStringLiteral("models");
    const JValue* models = root.get(section);
    if (models && !models->isObject()) return text;  // left to the check
    if (models && models->get(m.name)) return text;
    const QString indent = models ? indentOf(text, *models) : indentOf(text, root) + QStringLiteral("  ");
    QString json = m.value;
    if (json.isEmpty()) {
        const QStringList lines = m.text.split('\n');
        if (lines.size() == 1) {
            json = jsonQuote(lines.first());
        } else {
            QStringList quoted;
            for (const QString& l : lines) quoted << jsonQuote(l);
            const QString in = indent + QStringLiteral("  ");
            json = QStringLiteral("[\n") + in + quoted.join(QStringLiteral(",\n") + in) + QStringLiteral("\n") + indent +
                   QStringLiteral("]");
        }
    }
    if (models) return setMember(text, *models, m.name, json);
    const QString outer = indentOf(text, root);
    return setMember(text, root, section,
                     QStringLiteral("{\n") + indent + jsonQuote(m.name) + QStringLiteral(": ") + json +
                         QStringLiteral("\n") + outer + QStringLiteral("}"));
}
