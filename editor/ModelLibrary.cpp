#include "ModelLibrary.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

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
    QString t = description.section('\n', 0, 0).trimmed();
    if (t.startsWith(name + ' ', Qt::CaseInsensitive)) t = t.mid(name.size() + 1).trimmed();
    return t;
}

char LibraryModel::kind() const {
    if (type == "d") return 'd';
    if (type == "npn" || type == "pnp") return 'q';
    if (type == "nmos" || type == "pmos") return 'm';
    if (type == "njf" || type == "pjf") return 'j';
    return 0;
}

QVector<LibraryModel> parseSpiceLibrary(const QString& text, const QString& source) {
    QVector<LibraryModel> out;
    const QStringList lines = text.split('\n');
    QStringList comment;  // the comment block right above the current line
    int subckt = 0;       // models inside a .subckt belong to it
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        line.remove('\r');
        const QString t = line.trimmed();
        if (t.startsWith('*')) {
            comment << t.mid(1).trimmed();
            continue;
        }
        if (t.isEmpty()) {
            comment.clear();
            continue;
        }
        const QString lower = t.toLower();
        if (lower.startsWith(".subckt")) ++subckt;
        if (lower.startsWith(".ends") && subckt > 0) --subckt;
        if (!lower.startsWith(".model") || subckt > 0) {
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
        if (cardHead(m.text, m.name, m.type) && !m.type.isEmpty()) {
            m.description = comment.join('\n');
            m.source = source;
            out << m;
        }
        comment.clear();
    }
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

QString addModel(const QString& text, const LibraryModel& m) {
    JValue root;
    if (!parseJsonc(text, root) || !root.isObject()) return text;
    const JValue* models = root.get("models");
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
    return setMember(text, root, QStringLiteral("models"),
                     QStringLiteral("{\n") + indent + jsonQuote(m.name) + QStringLiteral(": ") + json +
                         QStringLiteral("\n") + outer + QStringLiteral("}"));
}
