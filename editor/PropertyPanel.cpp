#include "PropertyPanel.hpp"

#include <QApplication>
#include <QCompleter>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "JsonSpan.hpp"
#include "Symbols.hpp"
#include "Theme.hpp"

namespace {

QString kindTitle(const SComponent& c) {
    switch (c.kind) {
        case 'r': return QObject::tr("Widerstand");
        case 'c': return QObject::tr("Kondensator");
        case 'l': return QObject::tr("Spule");
        case 'v': return QObject::tr("Spannungsquelle");
        case 'i': return QObject::tr("Stromquelle");
        case 'd': return QObject::tr("Diode");
        case 'y': return QObject::tr("Ideale Diode");
        case 'q': return c.variant == "pnp" ? QObject::tr("PNP-Transistor") : QObject::tr("NPN-Transistor");
        case 'm': return c.variant == "pmos" ? QObject::tr("P-MOSFET") : QObject::tr("N-MOSFET");
        case 'j': return c.variant == "pjf" ? QObject::tr("P-JFET") : QObject::tr("N-JFET");
        case 'e': return QObject::tr("Spannungsgesteuerte Spannungsquelle");
        case 'g': return QObject::tr("Spannungsgesteuerte Stromquelle");
        case 'f': return QObject::tr("Stromgesteuerte Stromquelle");
        case 'h': return QObject::tr("Stromgesteuerte Spannungsquelle");
        case 'b': return QObject::tr("Verhaltensquelle");
        case 's': return QObject::tr("Schalter");
        case 't': return QObject::tr("Leitung");
        case 'k': return QObject::tr("Kopplung");
        case 'x': return QString::fromUtf8("Teilschaltung „%1“").arg(c.subcircuit);
        default: return c.type;
    }
}

// members offered even when missing
QStringList standardKeys(char kind) {
    switch (kind) {
        case 'r': return {"value"};
        case 'c':
        case 'l': return {"value", "ic"};
        case 'v':
        case 'i': return {"dc", "ac", "wave"};
        case 'd':
        case 'q':
        case 'm':
        case 'j': return {"model"};
        case 'y': return {"vf", "ron", "roff"};
        case 'e': return {"gain"};
        case 'g': return {"gm"};
        case 'f': return {"control", "gain"};
        case 'h': return {"control", "r"};
        case 's': return {"vt", "vh", "ron", "roff"};
        case 't': return {"z0", "td"};
        case 'k': return {"inductors", "k"};
        case 'x': return {"subcircuit"};
        default: return {};
    }
}

}  // namespace

QString fieldJson(const QString& input) {
    const QString t = input.trimmed();
    if (t.isEmpty()) return QString();
    JValue v;
    if (parseJsonc(t, v)) return t;
    return jsonQuote(t);
}

PropertyPanel::PropertyPanel(QWidget* parent) : QWidget(parent) {
    title_ = new QLabel;
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    title_->setWordWrap(true);
    message_ = new QLabel;
    message_->setWordWrap(true);
    message_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    QPalette pal = message_->palette();
    pal.setColor(QPalette::WindowText, Theme::textSecondary());
    message_->setPalette(pal);

    formHost_ = new QWidget;
    form_ = new QFormLayout(formHost_);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form_->setLabelAlignment(Qt::AlignRight);
    auto* scroll = new QScrollArea;
    scroll->setWidget(formHost_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->addWidget(title_);
    layout->addWidget(message_);
    layout->addWidget(scroll, 1);
    showComponent(QString(), nullptr, tr("Ein Bauteil im Plan oder im Text auswählen."));
}

void PropertyPanel::setModelNames(const QStringList& names) { models_ = names; }

bool PropertyPanel::editing() const {
    const QWidget* f = QApplication::focusWidget();
    return f && formHost_->isAncestorOf(f);
}

void PropertyPanel::focusFirstField() {
    if (firstField_) {
        firstField_->setFocus();
        firstField_->selectAll();
    } else if (name_) {
        name_->setFocus();
    }
}

QLineEdit* PropertyPanel::addField(const QString& key, const QString& value, bool present) {
    auto* edit = new QLineEdit(value);
    if (!present) edit->setPlaceholderText(tr("nicht gesetzt"));
    if (key == "model" && !models_.isEmpty()) {
        auto* completer = new QCompleter(models_, edit);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        edit->setCompleter(completer);
    }
    const int index = index_;
    connect(edit, &QLineEdit::editingFinished, this, [this, edit, key, index, value]() {
        if (edit->text() == value || index != index_) return;
        emit memberChanged(index, key, fieldJson(edit->text()));
    });
    form_->addRow(key, edit);
    if (!firstField_) firstField_ = edit;
    return edit;
}

void PropertyPanel::showComponent(const QString& text, const SComponent* c, const QString& message) {
    // rebuild the form (the old fields must not report while they go)
    for (QLineEdit* e : formHost_->findChildren<QLineEdit*>()) e->blockSignals(true);
    while (form_->rowCount() > 0) form_->removeRow(0);
    name_ = nullptr;
    firstField_ = nullptr;
    if (!c) {
        index_ = -1;
        shownName_.clear();
        title_->setText(tr("Eigenschaften"));
        message_->setText(message);
        message_->show();
        return;
    }
    index_ = c->index;
    shownName_ = c->name;
    title_->setText(QString::fromUtf8("%1 · %2").arg(c->name, kindTitle(*c)));
    message_->hide();

    JValue root;
    const JValue* obj = nullptr;
    if (parseJsonc(text, root) && root.isObject())
        if (const JValue* comps = root.get("components"); comps && comps->isArray() && c->index >= 0 &&
                                                          c->index < int(comps->items.size()))
            obj = &comps->items[size_t(c->index)];

    name_ = new QLineEdit(c->name);
    const int index = index_;
    const QString oldName = c->name;
    connect(name_, &QLineEdit::editingFinished, this, [this, index, oldName]() {
        const QString n = name_ ? name_->text().trimmed() : QString();
        if (n.isEmpty() || n == oldName || index != index_) return;
        emit renameRequested(index, n);
    });
    form_->addRow(tr("Name"), name_);

    QStringList shown;
    if (obj && obj->isObject()) {
        QStringList keys = standardKeys(c->kind);
        for (const QString& k : obj->keys)
            if (!keys.contains(k)) keys << k;
        for (const QString& k : keys) {
            if (k == "type" || k == "name" || k == "nodes") continue;
            const JValue* v = obj->get(k);
            QString value;
            if (v) value = v->isString() ? v->string : text.mid(v->begin, v->end - v->begin).simplified();
            addField(k, value, v != nullptr);
            shown << k;
        }
    }

    // a further parameter
    auto* key = new QLineEdit;
    key->setPlaceholderText(tr("Name"));
    auto* value = new QLineEdit;
    value->setPlaceholderText(tr("Wert"));
    auto* add = new QToolButton;
    add->setText("+");
    add->setToolTip(tr("Parameter hinzufügen"));
    auto* row = new QWidget;
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(key, 2);
    rl->addWidget(value, 3);
    rl->addWidget(add);
    const auto addMember = [this, key, value, index]() {
        const QString k = key->text().trimmed();
        if (k.isEmpty() || value->text().trimmed().isEmpty() || index != index_) return;
        emit memberChanged(index, k, fieldJson(value->text()));
    };
    connect(add, &QToolButton::clicked, this, addMember);
    connect(value, &QLineEdit::returnPressed, this, addMember);
    form_->addRow(tr("Neu"), row);

    // the connections, for information
    const Symbol sym = symbolOf(*c);
    if (!sym.pins.isEmpty()) {
        QStringList lines;
        for (int p = 0; p < sym.pins.size(); ++p)
            lines << QString::fromUtf8("%1 → %2").arg(sym.pins[p].name, c->nodes.value(p).isEmpty() ? QString::fromUtf8("–")
                                                                                                   : c->nodes.value(p));
        auto* nodes = new QLabel(lines.join("\n"));
        nodes->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form_->addRow(tr("Anschlüsse"), nodes);
    }
    auto* reveal = new QPushButton(tr("Im Text zeigen"));
    connect(reveal, &QPushButton::clicked, this, [this, index]() { emit showInText(index); });
    form_->addRow(QString(), reveal);
}
