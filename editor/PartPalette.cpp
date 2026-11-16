#include "PartPalette.hpp"

#include <QEvent>
#include <functional>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include "ModelLibrary.hpp"
#include "Schematic.hpp"
#include "Symbols.hpp"
#include "Theme.hpp"

namespace {

constexpr int kIcon = 28;

enum Role { IdRole = Qt::UserRole, SearchRole, LibraryRole };

// draws into a square icon, scaled to fit the drawing's box (grid units)
QIcon icon(const QRectF& box, const std::function<void(QPainter&)>& draw) {
    QPixmap pm(QSize(kIcon, kIcon) * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const double s = std::min((kIcon - 4) / box.width(), (kIcon - 4) / box.height());
    p.translate(kIcon / 2.0, kIcon / 2.0);
    p.scale(s, s);
    p.translate(-box.center());
    QPen pen(Theme::textPrimary(), 1.4 / s);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    draw(p);
    return QIcon(pm);
}

QIcon componentIcon(const SComponent& c) {
    const Symbol sym = symbolOf(c);
    QRectF box = sym.body;
    for (const SymPin& pin : sym.pins) box = box.united(QRectF(QPointF(pin.pos) - QPointF(0.01, 0.01), QSizeF(0.02, 0.02)));
    const double side = std::max(box.width(), box.height());
    box = QRectF(box.center() - QPointF(side / 2, side / 2), QSizeF(side, side));
    return icon(box, [&c](QPainter& p) { drawSymbol(p, c, nullptr); });
}

}  // namespace

PartPalette::PartPalette(QWidget* parent) : QWidget(parent) {
    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Bauteil suchen (A)"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    list_ = new QListWidget;
    list_->setIconSize(QSize(kIcon, kIcon));
    list_->setUniformItemSizes(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(search_);
    layout->addWidget(list_);
    connect(list_, &QListWidget::itemClicked, this, &PartPalette::choose);
    connect(list_, &QListWidget::itemActivated, this, &PartPalette::choose);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int k = 0; k < list_->count(); ++k) {
            QListWidgetItem* item = list_->item(k);
            // library models only while searching: there are many
            item->setHidden(!item->data(SearchRole).toString().contains(text, Qt::CaseInsensitive) ||
                            (item->data(LibraryRole).toBool() && text.trimmed().size() < 2));
        }
    });
    connect(search_, &QLineEdit::returnPressed, this, [this]() {
        for (int k = 0; k < list_->count(); ++k)
            if (!list_->item(k)->isHidden()) {
                choose(list_->item(k));
                return;
            }
    });
    rebuild();
}

void PartPalette::setSubcircuits(const QMap<QString, QStringList>& subcircuits) {
    if (subcircuits == subcircuits_) return;
    subcircuits_ = subcircuits;
    rebuild();
}

void PartPalette::setScope(const QString& scope) {
    if (scope == scope_) return;
    scope_ = scope;
    rebuild();
}

void PartPalette::setLibrary(const QVector<LibraryModel>* models) {
    library_ = models;
    rebuild();
}

void PartPalette::focusSearch() {
    search_->setFocus();
    search_->selectAll();
}

void PartPalette::choose(QListWidgetItem* item) {
    if (!item) return;
    emit partChosen(item->data(IdRole).toString());
}

bool PartPalette::eventFilter(QObject* o, QEvent* e) {
    if (o == search_ && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Down) {
            list_->setFocus();
            for (int r = 0; r < list_->count(); ++r)
                if (!list_->item(r)->isHidden()) {
                    list_->setCurrentRow(r);
                    break;
                }
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

void PartPalette::changeEvent(QEvent* e) {
    if (e->type() == QEvent::PaletteChange) rebuild();  // icons in the colours of the appearance
    QWidget::changeEvent(e);
}

void PartPalette::rebuild() {
    const QString filter = search_->text();
    list_->clear();
    const auto add = [&](const QString& id, const QString& title, const QString& keys, const QIcon& ic) {
        auto* item = new QListWidgetItem(ic, title, list_);
        item->setData(IdRole, id);
        item->setData(SearchRole, title + " " + keys);
        item->setHidden(!filter.isEmpty() && !(title + " " + keys).contains(filter, Qt::CaseInsensitive));
    };
    add("wire", tr("Leitung zeichnen  (W)"), "wire draht verbindung",
        icon(QRectF(-2, -2, 4, 4), [](QPainter& p) {
            p.drawPolyline(QPolygonF({QPointF(-1.6, 1.4), QPointF(-1.6, -1.2), QPointF(1.6, -1.2)}));
            p.setBrush(p.pen().color());
            p.drawEllipse(QPointF(-1.6, 1.4), 0.3, 0.3);
            p.drawEllipse(QPointF(1.6, -1.2), 0.3, 0.3);
        }));
    add("ground", tr("Masse  (G)"), "ground gnd 0 erde",
        icon(QRectF(-1, -0.5, 2, 2), [](QPainter& p) { drawGround(p); }));
    add("label", tr("Netzlabel  (N)"), "label name knoten net",
        icon(QRectF(-0.5, -1.6, 3.6, 3.2), [](QPainter& p) {
            QPainterPath f;
            f.moveTo(0, 0);
            f.lineTo(0.6, -0.7);
            f.lineTo(3, -0.7);
            f.lineTo(3, 0.7);
            f.lineTo(0.6, 0.7);
            f.closeSubpath();
            p.drawPath(f);
        }));
    const auto flag = [](bool pointed) {
        return icon(QRectF(-0.5, -1.6, 3.6, 3.2), [pointed](QPainter& p) {
            QPainterPath f;
            f.moveTo(0, 0);
            f.lineTo(0.6, -0.7);
            f.lineTo(pointed ? 2.5 : 3, -0.7);
            if (pointed) f.lineTo(3, 0);
            f.lineTo(pointed ? 2.5 : 3, 0.7);
            f.lineTo(0.6, 0.7);
            f.closeSubpath();
            p.drawPath(f);
        });
    };
    if (!scope_.isEmpty()) add("port", tr("Port der Teilschaltung  (P)"), "port anschluss pin", flag(true));
    Schematic none;
    for (const PartTemplate& t : partTemplates()) {
        SComponent c = makeComponent(t, none);
        add(t.id, t.title, t.type + " " + t.id, componentIcon(c));
    }
    for (auto it = subcircuits_.begin(); it != subcircuits_.end(); ++it) {
        if (it.key() == scope_) continue;
        Schematic s;
        s.subcircuits = subcircuits_;
        const SComponent c = makeSubcircuit(it.key(), s);
        add("sub:" + it.key(), QString::fromUtf8("Teilschaltung „%1“").arg(it.key()), "subcircuit x " + it.key(),
            componentIcon(c));
    }
    add("library", tr("Modellbibliothek …"), "modell model bibliothek library spice",
        icon(QRectF(-2.2, -2.2, 4.4, 4.4), [](QPainter& p) {
            for (int k = 0; k < 3; ++k) p.drawRect(QRectF(-1.6 + 1.1 * k, -1.6, 0.9, 3.2));
        }));
    add("newsub", tr("Neue Teilschaltung …"), "subcircuit teilschaltung neu new",
        icon(QRectF(-2.2, -2.2, 4.4, 4.4), [](QPainter& p) {
            p.drawRect(QRectF(-1.5, -1.5, 3, 3));
            p.drawLine(QPointF(0, -0.8), QPointF(0, 0.8));
            p.drawLine(QPointF(-0.8, 0), QPointF(0.8, 0));
        }));
    if (!library_) return;
    // components with a model of the library
    QHash<QString, SComponent> byType;
    for (const PartTemplate& t : partTemplates())
        if (!t.variant.isEmpty() || t.id == "d") byType.insert(t.id, makeComponent(t, none));
    for (int k = 0; k < library_->size(); ++k) {
        const LibraryModel& m = (*library_)[k];
        SComponent symbol;
        if (m.type == "subckt") {
            Schematic s;
            s.subcircuits.insert(m.name, m.ports);
            symbol = makeSubcircuit(m.name, s);
        } else if (byType.contains(m.type)) {
            symbol = byType.value(m.type);
        } else {
            continue;
        }
        const QString title = !m.title().isEmpty() ? m.title()
                              : m.type == "subckt" ? tr("Teilschaltung (SPICE)") : m.type.toUpper();
        add(QStringLiteral("lib:%1").arg(k), QString::fromUtf8("%1 · %2").arg(m.name, title),
            m.type + " " + m.source + " " + m.description, componentIcon(symbol));
        QListWidgetItem* item = list_->item(list_->count() - 1);
        item->setData(LibraryRole, true);
        item->setToolTip(m.description.isEmpty() ? m.source : m.description + "\n\n" + m.source);
        item->setHidden(filter.trimmed().size() < 2 || !item->data(SearchRole).toString().contains(filter, Qt::CaseInsensitive));
    }
}
